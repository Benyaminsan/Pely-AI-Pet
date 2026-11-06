#include "cloud_sync.hpp"
#include "cJSON.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(false)
struct FakeMutex { bool held=false; };
struct FakeEventGroup { EventBits_t bits=0; };
ESP_EVENT_DEFINE_BASE(IP_EVENT);
ESP_EVENT_DEFINE_BASE(WIFI_EVENT);

namespace {
using Values=std::map<std::string,std::vector<uint8_t>>;
struct Handle { std::string name; Values values; };
struct Registration { esp_event_loop_handle_t loop; esp_event_base_t base; int32_t id; esp_event_handler_t fn; void* arg; };
struct Posted { int32_t id; PetLogicEventData data; };
std::map<std::string,Values> flash;
std::map<nvs_handle_t,Handle> handles;
std::map<uintptr_t,Registration> registrations;
std::deque<Posted> posted;
uintptr_t next_registration=1;
int core=0,mutexes=0,groups=0,commits=0,uploads=0,task_creations=0;
int64_t now_us=0;
TaskHandle_t current_task=reinterpret_cast<void*>(1);
TaskHandle_t worker_handle=reinterpret_cast<void*>(88);
void (*worker_entry)(void*)=nullptr;
void* worker_arg=nullptr;
bool worker_live=false,auto_stop_worker=false,withhold_worker=false,in_wifi_callback=false;
bool network_up=false,fail_task=false;
esp_netif_t station;
esp_event_loop_handle_t pet_loop=reinterpret_cast<void*>(2);
esp_err_t flash_init_error=ESP_OK,upload_result=ESP_OK,unregister_error=ESP_OK;
std::string failed_set_namespace,failed_commit_namespace;
int commits_before_failure=0;
std::string payload;
enum class DuringUpload { NONE,SAVE,ERASE };
DuringUpload during_upload=DuringUpload::NONE;
CloudSync* active_cloud=nullptr;
PetLogic* active_pet=nullptr;

esp_err_t consume(esp_err_t& value) { const auto result=value; value=ESP_OK; return result; }
void reset() {
    CHECK(handles.empty() && registrations.empty() && !mutexes && !groups && !worker_live);
    flash.clear(); posted.clear(); now_us=0; core=0; current_task=reinterpret_cast<void*>(1);
    worker_entry=nullptr; worker_arg=nullptr; auto_stop_worker=withhold_worker=in_wifi_callback=false;
    network_up=fail_task=false; commits=uploads=task_creations=0; next_registration=1;
    flash_init_error=upload_result=unregister_error=ESP_OK;
    failed_set_namespace.clear(); failed_commit_namespace.clear(); commits_before_failure=0;
    payload.clear(); during_upload=DuringUpload::NONE; active_cloud=nullptr; active_pet=nullptr;
    cJSON_InitHooks(nullptr);
}
void seconds(int64_t value) { now_us=value*1000000; }
PetLogicConfig pet_config(bool dying=false) {
    PetLogicConfig cfg;
    cfg.hunger_per_second=cfg.energy_loss_per_second=cfg.happiness_loss_per_second=0;
    cfg.starvation_health_loss_per_second=cfg.exhaustion_health_loss_per_second=0;
    if (dying) { cfg.initial_hunger=100; cfg.initial_health=10; cfg.starvation_health_loss_per_second=2; }
    return cfg;
}
CloudSyncConfig cloud_config() { CloudSyncConfig cfg; cfg.device_id="pely \"buddy\"\\\n"; return cfg; }
SavedProgress progress(CloudSync& cloud) { SavedProgress p; CHECK(cloud.load_progress(p)==ESP_OK); return p; }
PetSnapshot pet_state(PetLogic& pet) { PetSnapshot s; CHECK(pet.get_state(s)==ESP_OK); return s; }
void emit(esp_event_base_t base,int32_t id,void* data=nullptr) {
    const auto snapshot=registrations;
    for (const auto& item:snapshot) {
        const auto& r=item.second;
        if (r.base==base && (r.id==id || r.id==ESP_EVENT_ANY_ID)) {
            in_wifi_callback=base==IP_EVENT || base==WIFI_EVENT;
            r.fn(r.arg,base,id,data);
            in_wifi_callback=false;
        }
    }
}
void connect() {
    network_up=true; ip_event_got_ip_t got{&station,{}}; got.ip_info.ip.addr=1;
    emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&got);
}
void drain_pet_events() {
    unsigned count=0;
    while (!posted.empty()) {
        CHECK(++count<100); // Detect accidental checkpoint -> event -> checkpoint feedback loops.
        const Posted event=posted.front(); posted.pop_front();
        auto data=event.data; emit(PET_LOGIC_EVENT,event.id,&data);
    }
}
esp_err_t transport(const char* json,std::size_t size,void*) {
    CHECK(!in_wifi_callback && core==0); ++uploads; payload.assign(json,size);
    if (during_upload!=DuringUpload::NONE) {
        const auto action=during_upload; during_upload=DuringUpload::NONE;
        if (action==DuringUpload::SAVE) {
            now_us+=10000000; CHECK(active_pet->update_tick()==ESP_OK); CHECK(active_cloud->save_progress()==ESP_OK);
        } else { CHECK(active_cloud->erase_all_memory()==ESP_OK); }
        CHECK(active_cloud->sync_to_firebase()==ESP_ERR_INVALID_STATE); // One transport call at a time.
    }
    return upload_result;
}
void run_worker() {
    CHECK(worker_entry); auto fn=worker_entry; worker_entry=nullptr;
    const auto previous=current_task; current_task=worker_handle;
    auto_stop_worker=true; fn(worker_arg); auto_stop_worker=false; current_task=previous;
}

void test_offline_save_days_and_real_json() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK);
    CloudSync cloud; CHECK(cloud.init(pet,pet_loop,cloud_config())==ESP_OK);
    seconds(2*86400+42); CHECK(cloud.save_progress()==ESP_OK);
    const auto p=progress(cloud); CHECK(p.care_days==2 && p.care_seconds==172842 && p.pending_sync && uploads==0);
    CHECK(cloud.sync_to_firebase()==ESP_ERR_INVALID_STATE);
    char json[512]; CHECK(cloud.get_progress_json(json,sizeof(json))==ESP_OK);
    cJSON* parsed=cJSON_Parse(json); CHECK(parsed);
    CHECK(cJSON_GetObjectItem(parsed,"care_days")->valuedouble==2);
    CHECK(cJSON_GetObjectItem(parsed,"care_seconds")->valuedouble==172842);
    CHECK(std::strcmp(cJSON_GetObjectItem(parsed,"device_id")->valuestring,cloud_config().device_id)==0);
    CHECK(cJSON_IsString(cJSON_GetObjectItem(parsed,"generation")) && cJSON_IsString(cJSON_GetObjectItem(parsed,"revision")));
    cJSON_Delete(parsed);
    char small[2]; CHECK(cloud.get_progress_json(small,sizeof(small))==ESP_ERR_INVALID_SIZE && small[0]=='\0');
}
void test_reboot_restores_offline_progress() {
    reset();
    { PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
      CHECK(cloud.init(pet,pet_loop)==ESP_OK); seconds(86401); CHECK(cloud.save_progress()==ESP_OK); }
    seconds(200000);
    PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop)==ESP_OK);
    CHECK(progress(cloud).care_days==1 && progress(cloud).care_seconds==86401 && progress(cloud).pending_sync);
    CHECK(pet_state(pet).care_age_seconds==86401); // Powered-off time is not added.
}
void test_placeholder_keeps_dirty_state() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop)==ESP_OK); connect();
    CHECK(cloud.sync_to_firebase()==ESP_ERR_NOT_SUPPORTED && progress(cloud).pending_sync && uploads==0);
    emit(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED);
    CHECK(cloud.sync_to_firebase()==ESP_ERR_INVALID_STATE && progress(cloud).pending_sync);
}
void test_background_wifi_worker_and_ack() {
    reset(); network_up=true; PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK);
    auto cfg=cloud_config(); cfg.transport=transport; CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop,cfg)==ESP_OK && task_creations==1);
    run_worker(); CHECK(uploads==1 && !progress(cloud).pending_sync && cloud.last_sync_error()==ESP_OK);
    cJSON* parsed=cJSON_Parse(payload.c_str()); CHECK(parsed); cJSON_Delete(parsed);
}
void test_upload_error_and_json_oom() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK);
    auto cfg=cloud_config(); cfg.transport=transport; CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop,cfg)==ESP_OK); connect(); upload_result=ESP_FAIL;
    CHECK(cloud.sync_to_firebase()==ESP_FAIL && progress(cloud).pending_sync);
    cJSON_Hooks hooks{}; hooks.malloc_fn=+[](std::size_t)->void* { return nullptr; }; hooks.free_fn=std::free;
    cJSON_InitHooks(&hooks); CHECK(cloud.sync_to_firebase()==ESP_ERR_NO_MEM && progress(cloud).pending_sync);
    cJSON_InitHooks(nullptr); upload_result=ESP_OK;
    CHECK(cloud.sync_to_firebase()==ESP_OK && !progress(cloud).pending_sync);
}
void test_newer_save_survives_old_ack() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK);
    auto cfg=cloud_config(); cfg.transport=transport; CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop,cfg)==ESP_OK); active_cloud=&cloud; active_pet=&pet; connect();
    during_upload=DuringUpload::SAVE; CHECK(cloud.sync_to_firebase()==ESP_OK);
    CHECK(progress(cloud).care_seconds==10 && progress(cloud).pending_sync);
    CHECK(cloud.sync_to_firebase()==ESP_OK && !progress(cloud).pending_sync && uploads==2);
}
void test_reset_survives_old_ack_and_preserves_wifi() {
    reset(); flash["wifi"]["credentials"]={1,2,3};
    PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK);
    auto cfg=cloud_config(); cfg.transport=transport; CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop,cfg)==ESP_OK); active_cloud=&cloud; active_pet=&pet; connect();
    during_upload=DuringUpload::ERASE; CHECK(cloud.sync_to_firebase()==ESP_OK);
    const auto p=progress(cloud); CHECK(p.generation==2 && p.pending_sync && p.care_days==0 && !p.reset_pending);
    CHECK(flash["wifi"]["credentials"]==std::vector<uint8_t>({1,2,3}));
    CHECK(cloud.sync_to_firebase()==ESP_OK && !progress(cloud).pending_sync);
    cJSON* parsed=cJSON_Parse(payload.c_str()); CHECK(parsed);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(parsed,"reset"))); cJSON_Delete(parsed);
}
void test_death_erases_save_but_keeps_dead_display() {
    reset();
    { PetLogic pet(pet_config(true)); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
      CHECK(cloud.init(pet,pet_loop)==ESP_OK); drain_pet_events();
      seconds(8); CHECK(pet.update_tick()==ESP_OK); drain_pet_events();
      CHECK(!pet_state(pet).alive && progress(cloud).care_days==0 && progress(cloud).generation==2);
      CHECK(flash["pet_logic"].empty());
      seconds(1000); CHECK(pet.update_tick()==ESP_OK); drain_pet_events();
      CHECK(flash["pet_logic"].empty() && pet.save_memory()==ESP_ERR_INVALID_STATE);
      CHECK(progress(cloud).generation==2); }
    PetLogic fresh(pet_config(true)); CHECK(fresh.init(pet_loop)==ESP_OK);
    CHECK(pet_state(fresh).alive && pet_state(fresh).care_age_seconds==0);
}
void test_pending_reset_recovers_after_restart() {
    reset();
    { PetLogic pet(pet_config(true)); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
      CHECK(cloud.init(pet,pet_loop)==ESP_OK); seconds(8); CHECK(pet.update_tick()==ESP_OK);
      failed_commit_namespace="pet_logic"; commits_before_failure=0;
      CHECK(cloud.erase_all_memory()==ESP_FAIL && progress(cloud).reset_pending); }
    seconds(20); PetLogic pet(pet_config(true)); CHECK(pet.init(pet_loop)==ESP_OK); CHECK(!pet_state(pet).alive);
    CloudSync cloud; CHECK(cloud.init(pet,pet_loop)==ESP_OK);
    CHECK(!progress(cloud).reset_pending && progress(cloud).generation==2 && flash["pet_logic"].empty());
}
void test_final_reset_commit_retry() {
    reset(); PetLogic pet(pet_config(true)); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop)==ESP_OK); seconds(8); CHECK(pet.update_tick()==ESP_OK);
    failed_commit_namespace="cloud_sync"; commits_before_failure=1;
    CHECK(cloud.erase_all_memory()==ESP_FAIL && progress(cloud).reset_pending && flash["pet_logic"].empty());
    CHECK(cloud.erase_all_memory()==ESP_OK && !progress(cloud).reset_pending && progress(cloud).generation==2);
}
void test_nvs_failures_and_automatic_retry_limits() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop)==ESP_OK); drain_pet_events();
    seconds(60); CHECK(pet.update_tick()==ESP_OK); failed_set_namespace="cloud_sync";
    drain_pet_events(); CHECK(cloud.last_storage_error()==ESP_FAIL && progress(cloud).care_seconds==0);
    CHECK(posted.empty()); // A failed checkpoint must not recursively retrigger itself.
    seconds(120); CHECK(pet.update_tick()==ESP_OK); drain_pet_events();
    CHECK(cloud.last_storage_error()==ESP_OK && progress(cloud).care_seconds==120);
}
void test_stale_death_and_new_game() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
    CHECK(cloud.init(pet,pet_loop)==ESP_OK); seconds(10); CHECK(cloud.save_progress()==ESP_OK);
    CHECK(pet.erase_memory()==ESP_OK); drain_pet_events();
    const auto p=progress(cloud); CHECK(p.care_seconds==0 && p.generation==2);
    emit(PET_LOGIC_EVENT,PET_EVENT_DIED); CHECK(progress(cloud).generation==2 && pet_state(pet).alive);
}
void test_corrupt_nvs_and_flash_init_error() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK);
    { CloudSync cloud; flash_init_error=ESP_FAIL; CHECK(cloud.init(pet,pet_loop)==ESP_FAIL); }
    flash["cloud_sync"]["progress_v1"]={1};
    CloudSync cloud; CHECK(cloud.init(pet,pet_loop)==ESP_ERR_INVALID_SIZE);
    CHECK(flash["cloud_sync"]["progress_v1"]==std::vector<uint8_t>({1}));
}
void test_core_config_and_stop_retry() {
    reset(); PetLogic pet(pet_config()); CHECK(pet.init(pet_loop)==ESP_OK); CloudSync cloud;
    auto cfg=cloud_config(); cfg.retry_interval_ms=0; CHECK(cloud.init(pet,pet_loop,cfg)==ESP_ERR_INVALID_ARG);
    core=1; CHECK(cloud.init(pet,pet_loop)==ESP_ERR_INVALID_STATE); core=0;
    CHECK(cloud.init(pet,pet_loop)==ESP_OK); core=1;
    CHECK(cloud.save_progress()==ESP_ERR_INVALID_STATE && cloud.erase_all_memory()==ESP_ERR_INVALID_STATE);
    CHECK(progress(cloud).generation==1); core=0;
    unregister_error=ESP_FAIL; CHECK(cloud.stop(0)==ESP_FAIL && groups==1);
    withhold_worker=true; CHECK(cloud.stop(0)==ESP_ERR_TIMEOUT && groups==1);
    withhold_worker=false; CHECK(cloud.stop()==ESP_OK && !groups && registrations.empty());
}
} // namespace

SemaphoreHandle_t xSemaphoreCreateMutex() { ++mutexes; return new FakeMutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t) { CHECK(!m->held); m->held=true; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { CHECK(m->held); m->held=false; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t m) { CHECK(!m->held); --mutexes; delete m; }
BaseType_t xPortGetCoreID() { return core; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return current_task; }
int64_t esp_timer_get_time() { return now_us; }
uint32_t esp_random() { return 123; }
esp_err_t nvs_flash_init() { CHECK(!in_wifi_callback); return consume(flash_init_error); }
esp_err_t nvs_open(const char* name,nvs_open_mode_t,nvs_handle_t* out) {
    CHECK(!in_wifi_callback); *out=handles.empty() ? 1 : handles.rbegin()->first+1; handles[*out]={name,flash[name]}; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(handles.erase(handle)==1); }
esp_err_t nvs_get_blob(nvs_handle_t h,const char* key,void* out,std::size_t* size) {
    const auto& map=handles.at(h).values; const auto it=map.find(key); if (it==map.end()) { return ESP_ERR_NVS_NOT_FOUND; }
    if (out && *size<it->second.size()) { return ESP_ERR_INVALID_SIZE; }
    if (out) { std::memcpy(out,it->second.data(),it->second.size()); } *size=it->second.size(); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char* key,const void* data,std::size_t size) {
    CHECK(!in_wifi_callback); if (handles.at(h).name==failed_set_namespace) { failed_set_namespace.clear(); return ESP_FAIL; }
    const auto* bytes=static_cast<const uint8_t*>(data); handles.at(h).values[key]={bytes,bytes+size}; return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) { CHECK(!in_wifi_callback); handles.at(h).values.clear(); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) {
    CHECK(!in_wifi_callback); const auto& handle=handles.at(h);
    if (handle.name==failed_commit_namespace) {
        if (commits_before_failure--==0) { failed_commit_namespace.clear(); return ESP_FAIL; }
    }
    flash[handle.name]=handle.values; ++commits; return ESP_OK;
}
esp_err_t esp_event_post_to(esp_event_loop_handle_t loop,esp_event_base_t base,int32_t id,const void* data,std::size_t size,TickType_t wait) {
    CHECK(loop==pet_loop && base==PET_LOGIC_EVENT && size==sizeof(PetLogicEventData) && wait==0);
    posted.push_back({id,*static_cast<const PetLogicEventData*>(data)}); return ESP_OK;
}
esp_err_t esp_event_handler_instance_register_with(esp_event_loop_handle_t loop,esp_event_base_t base,int32_t id,
                                                  esp_event_handler_t fn,void* arg,esp_event_handler_instance_t* out) {
    const auto key=next_registration++; registrations[key]={loop,base,id,fn,arg}; *out=reinterpret_cast<void*>(key); return ESP_OK;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t b,int32_t id,esp_event_handler_t fn,void* a,esp_event_handler_instance_t* out) {
    return esp_event_handler_instance_register_with(nullptr,b,id,fn,a,out);
}
esp_err_t esp_event_handler_instance_unregister_with(esp_event_loop_handle_t,esp_event_base_t,int32_t,esp_event_handler_instance_t instance) {
    if (unregister_error!=ESP_OK) { return consume(unregister_error); }
    CHECK(registrations.erase(reinterpret_cast<uintptr_t>(instance))==1); return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b,int32_t id,esp_event_handler_instance_t i) {
    return esp_event_handler_instance_unregister_with(nullptr,b,id,i);
}
EventGroupHandle_t xEventGroupCreate() { ++groups; return new FakeEventGroup; }
void vEventGroupDelete(EventGroupHandle_t group) { CHECK(!worker_live); --groups; delete group; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group,EventBits_t bits) { group->bits|=bits; return group->bits; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { return group->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group,EventBits_t bits) { const auto before=group->bits; group->bits&=~bits; return before; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,EventBits_t bits,BaseType_t clear,BaseType_t,TickType_t) {
    if (bits==8 && !(group->bits&8) && worker_entry && !withhold_worker) { run_worker(); }
    if (current_task==worker_handle && bits==6 && !(group->bits&bits) && auto_stop_worker) { group->bits|=4; }
    const auto value=group->bits; if (clear) { group->bits&=~bits; } return value;
}
BaseType_t xTaskCreatePinnedToCore(void(*entry)(void*),const char*,uint32_t stack,void* arg,UBaseType_t priority,TaskHandle_t* out,BaseType_t affinity) {
    CHECK(stack==6144 && priority==2 && affinity==0); if (fail_task) { return pdFALSE; }
    ++task_creations; worker_entry=entry; worker_arg=arg; worker_live=true; *out=worker_handle; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { CHECK(!task && current_task==worker_handle); worker_live=false; }
esp_err_t esp_read_mac(uint8_t* mac,esp_mac_type_t) { const uint8_t fake[]={1,2,3,4,5,6}; std::memcpy(mac,fake,6); return ESP_OK; }
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key) { return std::strcmp(key,"WIFI_STA_DEF")==0 ? &station : nullptr; }
bool esp_netif_is_netif_up(esp_netif_t*) { return network_up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t*,esp_netif_ip_info_t* info) { info->ip.addr=network_up ? 1 : 0; return ESP_OK; }

int main() {
    const struct { const char* name; void(*run)(); } tests[]={
        {"offline days and real cJSON serialization",test_offline_save_days_and_real_json},
        {"reboot restores unsynced powered-on age",test_reboot_restores_offline_progress},
        {"Firebase placeholder retains dirty state",test_placeholder_keeps_dirty_state},
        {"Wi-Fi-triggered background task and acknowledgement",test_background_wifi_worker_and_ack},
        {"upload failure and JSON allocation failure",test_upload_error_and_json_oom},
        {"new local save survives an old acknowledgement",test_newer_save_survives_old_ack},
        {"reset survives old acknowledgement and preserves Wi-Fi",test_reset_survives_old_ack_and_preserves_wifi},
        {"death erases NVS and keeps the dead display",test_death_erases_save_but_keeps_dead_display},
        {"pending reset recovery after restart",test_pending_reset_recovers_after_restart},
        {"final reset commit retries without duplicate generation",test_final_reset_commit_retry},
        {"NVS errors and bounded automatic retries",test_nvs_failures_and_automatic_retry_limits},
        {"stale death events do not reset a new pet",test_stale_death_and_new_game},
        {"corrupt storage and initialization errors preserve data",test_corrupt_nvs_and_flash_init_error},
        {"Core-0 access, handler cleanup, and stop retry",test_core_config_and_stop_retry},
    };
    for (const auto& test:tests) { test.run(); std::printf("PASS %s\n",test.name); }
    CHECK(handles.empty() && registrations.empty() && !mutexes && !groups && !worker_live);
    std::printf("All %zu host tests passed.\n",sizeof(tests)/sizeof(tests[0]));
}
