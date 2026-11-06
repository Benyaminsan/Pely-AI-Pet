#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pet_logic.hpp"

// Optional future HTTPS/Firebase implementation. Return ESP_OK only after server ack.
// Called outside locks from the Core-0 sync worker; json is borrowed for this call.
using FirebaseJsonTransport = esp_err_t (*)(const char* json, std::size_t length, void* context);

struct CloudSyncConfig {
    const char* device_id = nullptr; // Null: derive a stable ID from factory Wi-Fi MAC.
    const char* station_interface_key = "WIFI_STA_DEF";
    uint32_t autosave_interval_seconds = 60;
    uint32_t retry_interval_ms = 30000;
    FirebaseJsonTransport transport = nullptr; // Placeholder by default: no network request.
    void* transport_context = nullptr;
};

struct SavedProgress {
    uint64_t care_seconds = 0;
    uint32_t care_days = 0; // Completed 24-hour periods, not calendar-date boundaries.
    uint64_t generation = 1;
    uint64_t revision = 0;
    bool pending_sync = false;
    bool reset_pending = false;
};

/** Offline-first save coordinator. Public mutation/lifecycle APIs run on Core 0. */
class CloudSync final {
public:
    CloudSync();
    ~CloudSync();
    CloudSync(const CloudSync&) = delete;
    CloudSync& operator=(const CloudSync&) = delete;
    CloudSync(CloudSync&&) = delete;
    CloudSync& operator=(CloudSync&&) = delete;

    // NVS and default Wi-Fi/IP event loop must be usable. Pet loop must run on Core 0.
    // Does not connect Wi-Fi, create a default loop, or erase NVS on init errors.
    esp_err_t init(PetLogic& pet, esp_event_loop_handle_t pet_loop,
                   const CloudSyncConfig& config = CloudSyncConfig{});
    esp_err_t save_progress();     // Checkpoint pet first, then persist care_days locally.
    esp_err_t erase_all_memory();  // Reset game, leaving Wi-Fi/other namespaces intact.
    esp_err_t load_progress(SavedProgress& result) const;
    esp_err_t get_progress_json(char* buffer, std::size_t capacity) const;

    esp_err_t request_sync();      // Wake background worker; suitable for UI callbacks.
    // Worker calls this. Null transport returns ESP_ERR_NOT_SUPPORTED and retains dirty state.
    esp_err_t sync_to_firebase();
    esp_err_t last_sync_error() const;
    esp_err_t last_storage_error() const; // Last automatic save/reset result.
    // Call outside callbacks/transport, with no other lifecycle call in flight.
    // On timeout/error retain the object and retry stop before destruction.
    esp_err_t stop(TickType_t wait = portMAX_DELAY);
private:
    static void wifi_event(void*, esp_event_base_t, int32_t, void*);
    static void pet_event(void*, esp_event_base_t, int32_t, void*);
    static void task_entry(void*);
    void worker_loop();
    esp_err_t check_context() const;
    esp_err_t read_locked();
    esp_err_t write_locked(const SavedProgress& progress);
    esp_err_t erase_locked();
    esp_err_t json_locked(char* buffer, std::size_t capacity) const;
    esp_err_t unregister_handlers();
    void release_storage();

    mutable SemaphoreHandle_t mutex_ = nullptr;
    EventGroupHandle_t signals_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    nvs_handle_t nvs_ = 0;
    PetLogic* pet_ = nullptr; // Borrowed; must outlive this class.
    esp_event_loop_handle_t pet_loop_ = nullptr;
    esp_event_handler_instance_t pet_handler_ = nullptr;
    esp_event_handler_instance_t wifi_handlers_[4] = {};
    CloudSyncConfig config_;
    char device_id_[65] = {};
    char station_key_[33] = {};
    SavedProgress progress_;
    int64_t last_checkpoint_us_ = 0;
    int64_t last_reset_attempt_us_ = -1;
    esp_err_t last_sync_error_ = ESP_OK;
    esp_err_t last_storage_error_ = ESP_OK;
    bool initialized_ = false;
    bool stopping_ = false;
    bool erased_current_dead_pet_ = false;
    bool sync_inflight_ = false;
};
