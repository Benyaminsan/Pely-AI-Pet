#include "cloud_sync.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "cJSON.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

namespace {
constexpr char kNamespace[] = "cloud_sync";
constexpr char kProgressKey[] = "progress_v1";
constexpr uint32_t kDirty = 1, kResetPending = 2;
constexpr EventBits_t kOnline = 1, kRequest = 2, kStop = 4, kStopped = 8;
constexpr uint64_t kSecondsPerDay = 86400;
constexpr uint64_t kMaxCareSeconds = uint64_t{UINT32_MAX} * kSecondsPerDay + kSecondsPerDay - 1;
struct StoredProgress {
    uint32_t version, flags;
    uint64_t generation, revision, care_seconds;
    uint32_t care_days, reserved;
};
static_assert(sizeof(StoredProgress) == 40, "Change NVS version when the layout changes");
class Lock final {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SemaphoreHandle_t mutex_;
};
const esp_event_base_t kWifiBases[] = {IP_EVENT, IP_EVENT, WIFI_EVENT, WIFI_EVENT};
const int32_t kWifiIds[] = {IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP,
                           WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_STA_STOP};
} // namespace

CloudSync::CloudSync() : mutex_(xSemaphoreCreateMutex()) {}
CloudSync::~CloudSync() {
    if (signals_) {
        const esp_err_t error = stop();
        configASSERT(error == ESP_OK);
        (void)error;
    }
    if (mutex_) { vSemaphoreDelete(mutex_); }
}

esp_err_t CloudSync::check_context() const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    return xPortGetCoreID() == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t CloudSync::init(PetLogic& pet, esp_event_loop_handle_t pet_loop, const CloudSyncConfig& config) {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    if (!pet_loop || !config.station_interface_key || !config.station_interface_key[0] ||
        std::strlen(config.station_interface_key) >= sizeof(station_key_) ||
        (config.device_id && (!config.device_id[0] || std::strlen(config.device_id) >= sizeof(device_id_))) ||
        !config.autosave_interval_seconds || config.autosave_interval_seconds > 86400 ||
        config.retry_interval_ms < 1000 || config.retry_interval_ms > 3600000) { return ESP_ERR_INVALID_ARG; }
    {
        Lock lock(mutex_);
        if (signals_) { return ESP_ERR_INVALID_STATE; }
        // nvs_flash_init is idempotent. Preserve data on NO_FREE_PAGES/NEW_VERSION errors.
        esp_err_t error = nvs_flash_init();
        if (error != ESP_OK) { return error; }
        error = nvs_open(kNamespace, NVS_READWRITE, &nvs_);
        if (error != ESP_OK) { return error; }
        config_ = config;
        std::snprintf(station_key_, sizeof(station_key_), "%s", config.station_interface_key);
        if (config.device_id) { std::snprintf(device_id_, sizeof(device_id_), "%s", config.device_id); }
        else {
            uint8_t mac[6]{};
            error = esp_read_mac(mac, ESP_MAC_WIFI_STA);
            if (error == ESP_OK) {
                std::snprintf(device_id_, sizeof(device_id_), "pely_%02x%02x%02x%02x%02x%02x",
                    unsigned(mac[0]), unsigned(mac[1]), unsigned(mac[2]), unsigned(mac[3]), unsigned(mac[4]), unsigned(mac[5]));
            }
        }
        if (error == ESP_OK) { error = read_locked(); }
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            SavedProgress fresh;
            fresh.pending_sync = true;
            error = write_locked(fresh);
        }
        if (error != ESP_OK) { nvs_close(nvs_); nvs_ = 0; return error; }
        signals_ = xEventGroupCreate();
        if (!signals_) { nvs_close(nvs_); nvs_ = 0; return ESP_ERR_NO_MEM; }
        pet_ = &pet;
        pet_loop_ = pet_loop;
        initialized_ = true;
        stopping_ = erased_current_dead_pet_ = sync_inflight_ = false;
        last_checkpoint_us_ = esp_timer_get_time();
        last_reset_attempt_us_ = -1;
        last_sync_error_ = last_storage_error_ = ESP_OK;
    }
    esp_err_t error = ESP_OK;
    for (unsigned i = 0; i < 4 && error == ESP_OK; ++i) {
        error = esp_event_handler_instance_register(kWifiBases[i], kWifiIds[i], wifi_event, this, &wifi_handlers_[i]);
    }
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register_with(pet_loop_, PET_LOGIC_EVENT, ESP_EVENT_ANY_ID,
            pet_event, this, &pet_handler_);
    }
    if (error == ESP_OK) {
        Lock lock(mutex_);
        if (progress_.reset_pending) { error = erase_locked(); }
    }
    if (error == ESP_OK) { error = save_progress(); }
    if (error == ESP_OK && xTaskCreatePinnedToCore(task_entry, "cloud_sync", 6144,
        this, 2, &worker_, 0) != pdPASS) { error = ESP_ERR_NO_MEM; }
    if (error != ESP_OK) {
        const esp_err_t cleaned = stop();
        return cleaned != ESP_OK ? cleaned : error;
    }
    // Also handle initialization after Wi-Fi already obtained its IP address.
    esp_netif_t* station = esp_netif_get_handle_from_ifkey(station_key_);
    esp_netif_ip_info_t ip{};
    if (station && esp_netif_is_netif_up(station) && esp_netif_get_ip_info(station, &ip) == ESP_OK && ip.ip.addr) {
        xEventGroupSetBits(signals_, kOnline | kRequest);
    }
    return ESP_OK;
}

esp_err_t CloudSync::read_locked() {
    StoredProgress stored{};
    std::size_t size = 0;
    esp_err_t error = nvs_get_blob(nvs_, kProgressKey, nullptr, &size);
    if (error != ESP_OK) { return error; }
    if (size != sizeof(stored)) { return ESP_ERR_INVALID_SIZE; }
    error = nvs_get_blob(nvs_, kProgressKey, &stored, &size);
    if (error != ESP_OK) { return error; }
    if (stored.version != 1) { return ESP_ERR_NOT_SUPPORTED; }
    if ((stored.flags & ~(kDirty | kResetPending)) || !stored.generation ||
        stored.care_seconds > kMaxCareSeconds || stored.care_days != stored.care_seconds / kSecondsPerDay ||
        ((stored.flags & kResetPending) && (!(stored.flags & kDirty) || stored.care_seconds != 0))) {
        return ESP_ERR_INVALID_STATE;
    }
    progress_ = {stored.care_seconds, stored.care_days, stored.generation, stored.revision,
        (stored.flags & kDirty) != 0, (stored.flags & kResetPending) != 0};
    return ESP_OK;
}

esp_err_t CloudSync::write_locked(const SavedProgress& progress) {
    StoredProgress stored{1, (progress.pending_sync ? kDirty : 0) | (progress.reset_pending ? kResetPending : 0),
        progress.generation, progress.revision, progress.care_seconds, progress.care_days, 0};
    esp_err_t error = nvs_set_blob(nvs_, kProgressKey, &stored, sizeof(stored));
    if (error == ESP_OK) { error = nvs_commit(nvs_); }
    if (error == ESP_OK) { progress_ = progress; }
    return error;
}

esp_err_t CloudSync::save_progress() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_ || stopping_) { return ESP_ERR_INVALID_STATE; }
    if (progress_.reset_pending) {
        const esp_err_t error = erase_locked();
        if (error != ESP_OK) { return error; }
    }
    PetSnapshot pet;
    esp_err_t error = pet_->get_state(pet);
    if (error != ESP_OK) { return error; }
    if (!pet.alive) { return erase_locked(); }
    // Persist the authoritative age before its derived day counter, avoiding an
    // independently saved cloud age that rolls backward when PetLogic reboots.
    error = pet_->save_memory();
    const esp_err_t snapshot_error = pet_->get_state(pet);
    if (snapshot_error != ESP_OK) { return snapshot_error; }
    if (!pet.alive) { return erase_locked(); }
    if (error != ESP_OK) { return error; }
    if (!std::isfinite(pet.care_age_seconds) || pet.care_age_seconds < 0 || pet.care_age_seconds > kMaxCareSeconds) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t seconds = static_cast<uint64_t>(pet.care_age_seconds);
    if (seconds != progress_.care_seconds) {
        SavedProgress next = progress_;
        if (next.revision == UINT64_MAX || (seconds < next.care_seconds && next.generation == UINT64_MAX)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (seconds < next.care_seconds) { ++next.generation; }
        next.care_seconds = seconds;
        next.care_days = static_cast<uint32_t>(seconds / kSecondsPerDay);
        ++next.revision;
        next.pending_sync = true;
        error = write_locked(next);
        if (error != ESP_OK) { return error; }
    }
    erased_current_dead_pet_ = false;
    last_checkpoint_us_ = esp_timer_get_time();
    xEventGroupSetBits(signals_, kRequest);
    return ESP_OK;
}

esp_err_t CloudSync::erase_locked() {
    PetSnapshot pet;
    esp_err_t error = pet_->get_state(pet);
    if (error != ESP_OK) { return error; }
    if (!pet.alive && erased_current_dead_pet_ && !progress_.reset_pending) { return ESP_OK; }
    if (!progress_.reset_pending) {
        if (progress_.generation == UINT64_MAX || progress_.revision == UINT64_MAX) { return ESP_ERR_INVALID_STATE; }
        SavedProgress reset;
        reset.generation = progress_.generation + 1;
        reset.revision = progress_.revision + 1;
        reset.pending_sync = reset.reset_pending = true;
        // Durable reset intent survives a power failure between the two namespaces.
        error = write_locked(reset);
        if (error != ESP_OK) { return error; }
    }
    error = pet_->erase_memory(pet.alive); // Dead stays dead in RAM; live manual reset starts a new pet.
    if (error != ESP_OK) { return error; }
    SavedProgress done = progress_;
    done.reset_pending = false;
    error = write_locked(done);
    if (error == ESP_OK) {
        erased_current_dead_pet_ = !pet.alive;
        last_checkpoint_us_ = esp_timer_get_time();
        xEventGroupSetBits(signals_, kRequest);
    }
    return error;
}

esp_err_t CloudSync::erase_all_memory() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    return initialized_ && !stopping_ ? erase_locked() : ESP_ERR_INVALID_STATE;
}

esp_err_t CloudSync::load_progress(SavedProgress& result) const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    result = progress_;
    return ESP_OK;
}

esp_err_t CloudSync::json_locked(char* buffer, std::size_t capacity) const {
    if (!buffer || !capacity || capacity > INT32_MAX) { return ESP_ERR_INVALID_ARG; }
    buffer[0] = '\0';
    cJSON* json = cJSON_CreateObject();
    if (!json) { return ESP_ERR_NO_MEM; }
    char generation[21], revision[21];
    std::snprintf(generation, sizeof(generation), "%llu", static_cast<unsigned long long>(progress_.generation));
    std::snprintf(revision, sizeof(revision), "%llu", static_cast<unsigned long long>(progress_.revision));
    const bool added = cJSON_AddNumberToObject(json, "schema_version", 1) &&
        cJSON_AddStringToObject(json, "device_id", device_id_) &&
        cJSON_AddNumberToObject(json, "care_days", progress_.care_days) &&
        cJSON_AddNumberToObject(json, "care_seconds", static_cast<double>(progress_.care_seconds)) &&
        cJSON_AddStringToObject(json, "generation", generation) &&
        cJSON_AddStringToObject(json, "revision", revision) &&
        cJSON_AddBoolToObject(json, "reset", progress_.care_seconds == 0 && progress_.generation > 1);
    const bool printed = added && cJSON_PrintPreallocated(json, buffer, static_cast<int>(capacity), false);
    cJSON_Delete(json);
    if (!added) { return ESP_ERR_NO_MEM; }
    if (!printed) { buffer[0] = '\0'; return ESP_ERR_INVALID_SIZE; }
    return ESP_OK;
}

esp_err_t CloudSync::get_progress_json(char* buffer, std::size_t capacity) const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    Lock lock(mutex_);
    return initialized_ ? json_locked(buffer, capacity) : ESP_ERR_INVALID_STATE;
}

esp_err_t CloudSync::sync_to_firebase() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    SavedProgress sent;
    char json[512];
    FirebaseJsonTransport transport = nullptr;
    void* transport_context = nullptr;
    {
        Lock lock(mutex_);
        if (!initialized_ || stopping_ || sync_inflight_ || progress_.reset_pending || !(xEventGroupGetBits(signals_) & kOnline)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!progress_.pending_sync) { return ESP_OK; }
        const esp_err_t error = json_locked(json, sizeof(json));
        if (error != ESP_OK) { return error; }
        sent = progress_;
        transport = config_.transport;
        transport_context = config_.transport_context;
        sync_inflight_ = true;
    }
    // Placeholder: no HTTP/auth endpoint is configured and no request is sent by default.
    // Keep dirty data durable until a real transport reports server acknowledgement.
    const esp_err_t error = transport ? transport(json, std::strlen(json), transport_context) : ESP_ERR_NOT_SUPPORTED;
    Lock lock(mutex_);
    sync_inflight_ = false;
    if (error != ESP_OK) { return error; }
    if (!initialized_ || stopping_) { return ESP_ERR_INVALID_STATE; }
    if (progress_.revision == sent.revision && progress_.generation == sent.generation && !progress_.reset_pending) {
        SavedProgress acknowledged = progress_;
        acknowledged.pending_sync = false;
        return write_locked(acknowledged);
    }
    return ESP_OK; // A newer local save/reset remains dirty; an old ack cannot clear it.
}

esp_err_t CloudSync::request_sync() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_ || stopping_) { return ESP_ERR_INVALID_STATE; }
    xEventGroupSetBits(signals_, kRequest);
    return ESP_OK;
}

esp_err_t CloudSync::last_sync_error() const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    Lock lock(mutex_);
    return last_sync_error_;
}

esp_err_t CloudSync::last_storage_error() const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    Lock lock(mutex_);
    return last_storage_error_;
}

void CloudSync::wifi_event(void* argument, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<CloudSync*>(argument);
    Lock lock(self->mutex_);
    if (!self->initialized_ || self->stopping_) { return; }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* got = static_cast<ip_event_got_ip_t*>(data);
        if (got && got->esp_netif == esp_netif_get_handle_from_ifkey(self->station_key_) && got->ip_info.ip.addr) {
            xEventGroupSetBits(self->signals_, kOnline | kRequest);
        }
    } else {
        xEventGroupClearBits(self->signals_, kOnline);
        xEventGroupSetBits(self->signals_, kRequest);
    }
}

void CloudSync::pet_event(void* argument, esp_event_base_t, int32_t id, void*) {
    if (id != PET_EVENT_STATE_CHANGED && id != PET_EVENT_DIED && id != PET_EVENT_MEMORY_ERASED) { return; }
    auto* self = static_cast<CloudSync*>(argument);
    bool save = false, reset = false;
    {
        Lock lock(self->mutex_);
        if (!self->initialized_ || self->stopping_) { return; }
        PetSnapshot current;
        if (self->pet_->get_state(current) != ESP_OK) { return; }
        // Read live state: a queued death event from an older pet must not reset a new one.
        const int64_t now = esp_timer_get_time();
        reset = !current.alive && !self->erased_current_dead_pet_ &&
            (self->last_reset_attempt_us_ < 0 || now - self->last_reset_attempt_us_ >= 1000000);
        save = current.alive && (id == PET_EVENT_MEMORY_ERASED || now - self->last_checkpoint_us_ >=
            self->config_.autosave_interval_seconds * int64_t{1000000});
        if (save) { self->last_checkpoint_us_ = now; }
        if (reset) { self->last_reset_attempt_us_ = now; }
    }
    // Rate-limit attempts, including failures: PetLogic checkpoints themselves
    // post state events, which must not form a recursive flash-write retry loop.
    if (reset || save) {
        const esp_err_t error = reset ? self->erase_all_memory() : self->save_progress();
        Lock lock(self->mutex_);
        self->last_storage_error_ = error;
    }
}

void CloudSync::task_entry(void* argument) {
    static_cast<CloudSync*>(argument)->worker_loop();
    vTaskDelete(nullptr);
}

void CloudSync::worker_loop() {
    for (;;) {
        bool pending;
        {
            Lock lock(mutex_);
            pending = progress_.pending_sync && !progress_.reset_pending;
        }
        const bool online = (xEventGroupGetBits(signals_) & kOnline) != 0;
        const TickType_t wait = online && pending ? pdMS_TO_TICKS(config_.retry_interval_ms) : portMAX_DELAY;
        const EventBits_t bits = xEventGroupWaitBits(signals_, kRequest | kStop, pdTRUE, pdFALSE, wait);
        if (bits & kStop) { break; }
        if (!(xEventGroupGetBits(signals_) & kOnline)) { continue; }
        const esp_err_t error = sync_to_firebase();
        Lock lock(mutex_);
        last_sync_error_ = error;
    }
    xEventGroupSetBits(signals_, kStopped);
}

esp_err_t CloudSync::unregister_handlers() {
    if (pet_handler_) {
        const esp_err_t error = esp_event_handler_instance_unregister_with(
            pet_loop_, PET_LOGIC_EVENT, ESP_EVENT_ANY_ID, pet_handler_);
        if (error != ESP_OK) { return error; }
        pet_handler_ = nullptr;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!wifi_handlers_[i]) { continue; }
        const esp_err_t error = esp_event_handler_instance_unregister(kWifiBases[i], kWifiIds[i], wifi_handlers_[i]);
        if (error != ESP_OK) { return error; }
        wifi_handlers_[i] = nullptr;
    }
    return ESP_OK;
}

void CloudSync::release_storage() {
    if (nvs_) { nvs_close(nvs_); nvs_ = 0; }
    if (signals_) { vEventGroupDelete(signals_); signals_ = nullptr; }
    worker_ = nullptr;
    pet_ = nullptr;
    pet_loop_ = nullptr;
    initialized_ = stopping_ = false;
}

esp_err_t CloudSync::stop(TickType_t wait) {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    {
        Lock lock(mutex_);
        if (!signals_ || (worker_ && xTaskGetCurrentTaskHandle() == worker_)) { return ESP_ERR_INVALID_STATE; }
        stopping_ = true;
        if (worker_) { xEventGroupSetBits(signals_, kStop); }
    }
    // Unregister without holding mutex_: an in-flight event callback may need it.
    const esp_err_t error = unregister_handlers();
    if (error != ESP_OK) { return error; }
    if (worker_ && !(xEventGroupWaitBits(signals_, kStopped, pdFALSE, pdTRUE, wait) & kStopped)) {
        return ESP_ERR_TIMEOUT;
    }
    Lock lock(mutex_);
    release_storage();
    return ESP_OK;
}
