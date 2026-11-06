// Compile the production entry point in this TU so its bounded runtime helpers
// can be exercised without simulating an infinite FreeRTOS scheduler.
#include "../../main.cpp"

#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(false)

struct MockQueue { std::size_t size; std::vector<uint8_t> value; bool pending=false; };
struct MockGroup { EventBits_t bits=0; };
struct MockPost { esp_event_base_t base; int32_t id; std::vector<uint8_t> bytes; };
struct MockHandler { esp_event_base_t base; int32_t id; esp_event_handler_t fn; void* arg; };
struct MockLoop {
    esp_event_loop_args_t args{};
    std::deque<MockPost> pending;
    std::vector<MockHandler> handlers;
};
struct MockTask { TaskFunction_t fn; std::string name; uint32_t stack; UBaseType_t priority; BaseType_t core; void* arg; };

ESP_EVENT_DEFINE_BASE(PET_LOGIC_EVENT);
ESP_EVENT_DEFINE_BASE(TOUCH_SENSOR_EVENT);
ESP_EVENT_DEFINE_BASE(VISION_AI_EVENT);
ESP_EVENT_DEFINE_BASE(WIFI_EVENT);
ESP_EVENT_DEFINE_BASE(IP_EVENT);

namespace {
int g_core=0;
const char* g_role="main";
int64_t g_now=0;
std::vector<MockTask> g_tasks;
std::vector<std::unique_ptr<MockQueue>> g_queues;
std::vector<std::unique_ptr<MockGroup>> g_groups;
std::vector<std::unique_ptr<MockLoop>> g_loops;
std::vector<MockHandler> g_default_handlers;
PetSnapshot g_pet;
BehaviorOutcome g_forced_outcome=BehaviorOutcome::CORRECT;
bool g_force_outcome=false,g_frame_held=false;
int g_food_calls=0,g_affection_calls=0,g_cloud_init=0,g_matrix_calls=0,g_ui_tick_calls=0;
int g_frame_returns=0,g_camera_live=0,g_model_live=0,g_deleted_tasks=0;
int g_touch_remaining=1,g_station_creations=0,g_wifi_connects=0,g_gpio_service=0;
esp_err_t g_pet_init_error=ESP_OK,g_cloud_error=ESP_OK,g_action_error=ESP_OK;
esp_err_t g_camera_error=ESP_OK,g_vision_error=ESP_OK,g_capture_error=ESP_OK,g_infer_error=ESP_OK,g_matrix_error=ESP_OK;
esp_err_t g_post_error=ESP_OK;
Emotion g_displayed=Emotion::IDLE;
wifi_config_t g_wifi_config{};
uint8_t g_pixels[2]={0xf8,0};
camera_fb_t g_frame{g_pixels,2,1,1};
VisionResult g_detection;

MockLoop& loop(AppRuntime& app) { return *static_cast<MockLoop*>(app.pet_loop); }
void role(const char* name,int core=0) { g_role=name; g_core=core; }
void actor(AppRuntime& app) { role("pet"); CHECK(esp_event_loop_run(app.pet_loop,0)==ESP_OK); }
void reset_actor(AppRuntime& app) {
    role("pet"); g_now=0; g_pet=PetSnapshot{};
    g_food_calls=g_affection_calls=g_matrix_calls=g_ui_tick_calls=0;
    g_force_outcome=false; g_action_error=g_matrix_error=g_post_error=ESP_OK;
    app.vision_gate.reset(); app.ui=pely::UiState{};
    app.ui_mailbox->pending=false; app.state->bits=kPetReady|kPetAlive;
    loop(app).pending.clear();
    CHECK(app.pet.init(app.pet_loop,app.catalog.foods(),app.catalog.food_count())==ESP_OK);
}
pely::VisionInput sample(uint32_t sequence,const char* id="apple",int64_t observed=0) {
    pely::VisionInput input; input.sequence=sequence; input.observed_us=observed;
    input.confidence=0.9F; input.frame_valid=true;
    std::snprintf(input.food_id,sizeof(input.food_id),"%s",id); return input;
}
void send_frame(AppRuntime& app,uint32_t sequence,const char* id="apple") {
    const auto input=sample(sequence,id,g_now);
    CHECK(esp_event_post_to(app.pet_loop,PELY_INPUT_EVENT,kVisionFrameEvent,&input,sizeof(input),0)==ESP_OK);
}

void test_boot_task_placement(AppRuntime& app) {
    CHECK(g_tasks.size()==4 && g_cloud_init==0);
    for (const auto& task:g_tasks) {
        CHECK(task.arg==&app);
        if (task.name=="task_vision_ai") { CHECK(task.core==1 && task.stack>=8192); }
        else { CHECK(task.core==0); }
    }
    CHECK(loop(app).args.task_name==nullptr && loop(app).args.queue_size==32);
    CHECK(app.catalog.food_count()==2 && app.catalog.label_count()==3);
    CHECK(std::strcmp(app.catalog.foods()[0].id,"apple")==0 && std::strcmp(app.catalog.foods()[1].id,"milk")==0);
    role("pet"); CHECK(initialize_pet(app));
    CHECK(g_cloud_init==1 && (app.state->bits&kPetReady) && (app.state->bits&kPetAlive));
    actor(app);
}
void test_touch_dispatch_is_asynchronous(AppRuntime& app) {
    reset_actor(app); g_touch_remaining=1;
    role("touch"); task_touch_sensor(&app);
    CHECK(g_affection_calls==0 && g_matrix_calls==0 && loop(app).pending.size()==1);
    actor(app);
    CHECK(g_affection_calls==1 && app.ui.effective(g_now)==Emotion::HAPPY && g_matrix_calls==0);
}
void test_vision_capture_releases_before_post(AppRuntime& app) {
    reset_actor(app); role("vision",1);
    CameraHal camera; VisionAI vision;
    CHECK(camera.init(pely::camera_config())==ESP_OK);
    CHECK(vision.init(pely::vision_config(app.catalog))==ESP_OK);
    g_detection=VisionResult{}; g_detection.count=1;
    std::snprintf(g_detection.detections[0].food_id,32,"apple"); g_detection.detections[0].confidence=0.9F;
    const int before=g_frame_returns;
    for (uint32_t i=1;i<=3;++i) {
        pely::VisionInput input;
        CHECK(capture_vision_input(camera,vision,i,input)==ESP_OK && input.frame_valid && !g_frame_held);
        CHECK(esp_event_post_to(app.pet_loop,PELY_INPUT_EVENT,kVisionFrameEvent,&input,sizeof(input),0)==ESP_OK);
    }
    CHECK(g_frame_returns==before+3 && g_food_calls==0 && sizeof(pely::VisionInput)<=64);
    actor(app); CHECK(g_food_calls==1 && g_matrix_calls==0);
    role("vision",1); // RAII destructors run on their owning task/core.
}
void test_held_card_is_consumed_once(AppRuntime& app) {
    reset_actor(app);
    for (uint32_t i=1;i<=12;++i) { send_frame(app,i); actor(app); }
    CHECK(g_food_calls==1); // Request now changed to milk; held apple must not become repeated wrong food.
    for (uint32_t i=13;i<=15;++i) { send_frame(app,i,""); actor(app); }
    for (uint32_t i=16;i<=18;++i) { send_frame(app,i); actor(app); }
    CHECK(g_food_calls==2);
}
void test_cooldown_and_output_backpressure(AppRuntime& app) {
    reset_actor(app); g_force_outcome=true; g_forced_outcome=BehaviorOutcome::IGNORED_COOLDOWN;
    for (uint32_t i=1;i<=3;++i) { send_frame(app,i); actor(app); }
    CHECK(g_food_calls==1);
    g_forced_outcome=BehaviorOutcome::CORRECT; g_action_error=ESP_ERR_TIMEOUT;
    send_frame(app,4); actor(app); CHECK(g_food_calls==2 && app.ui.effective(g_now)==Emotion::HAPPY);
    send_frame(app,5); actor(app); CHECK(g_food_calls==2); // Applied action isn't replayed because posting failed.
    g_action_error=ESP_OK;
}
void test_confirmation_gaps_and_wrap(AppRuntime&) {
    pely::VisionGate gate;
    CHECK(!gate.observe(sample(1),0,3,0.7F,1000));
    CHECK(!gate.observe(sample(3),0,3,0.7F,1000));
    CHECK(!gate.observe(sample(4),0,3,0.7F,1000));
    CHECK(gate.observe(sample(5),0,3,0.7F,1000));
    gate.reset();
    CHECK(!gate.observe(sample(UINT32_MAX-1),0,3,0.7F,1000));
    CHECK(!gate.observe(sample(UINT32_MAX),0,3,0.7F,1000));
    CHECK(gate.observe(sample(0),0,3,0.7F,1000));
}
void test_stale_and_failed_frames_do_not_rearm(AppRuntime&) {
    pely::VisionGate gate;
    CHECK(gate.observe(sample(1),0,1,0.7F,1000)); gate.consume("apple");
    for (uint32_t i=2;i<=4;++i) { CHECK(!gate.observe(sample(i),5000,1,0.7F,1000)); }
    CHECK(!gate.observe(sample(5,"apple",5000),5000,1,0.7F,1000));
    for (uint32_t i=6;i<=8;++i) {
        auto bad=sample(i,"",5000); bad.frame_valid=false;
        CHECK(!gate.observe(bad,5000,1,0.7F,1000));
    }
    CHECK(!gate.observe(sample(9,"apple",5000),5000,1,0.7F,1000));
    for (uint32_t i=10;i<=12;++i) { CHECK(!gate.observe(sample(i,"",5000),5000,1,0.7F,1000)); }
    CHECK(gate.observe(sample(13,"apple",5000),5000,1,0.7F,1000));
}
void test_bad_confidence_and_missing_frame_reset(AppRuntime&) {
    pely::VisionGate gate;
    CHECK(!gate.observe(sample(1),0,3,0.7F,1000));
    CHECK(!gate.observe(sample(2,""),0,3,0.7F,1000));
    CHECK(!gate.observe(sample(3),0,3,0.7F,1000));
    CHECK(!gate.observe(sample(4),0,3,0.7F,1000));
    CHECK(gate.observe(sample(5),0,3,0.7F,1000));
    gate.reset(); auto bad=sample(1); bad.confidence=std::nanf("");
    CHECK(!gate.observe(bad,0,1,0.7F,1000));
    bad=sample(2); std::memset(bad.food_id,'x',sizeof(bad.food_id));
    CHECK(!gate.observe(bad,0,1,0.7F,1000));
    CHECK(!gate.observe(sample(3,"apple",100),99,1,0.7F,1000));
}
void test_ui_reaction_survives_state_coalescing(AppRuntime& app) {
    reset_actor(app); react(app,Emotion::HAPPY,95,2800); publish_ui(app);
    g_pet.mood=PetMood::SLEEPY;
    PetLogicEventData event{}; on_pet_event(&app,PET_LOGIC_EVENT,PET_EVENT_STATE_CHANGED,&event);
    CHECK(app.ui.effective(1000000)==Emotion::HAPPY && app.ui.effective(2800000)==Emotion::SLEEPY);
    react(app,Emotion::ANGRY,90,2200); CHECK(app.ui.effective(1)==Emotion::HAPPY);
    react(app,Emotion::HAPPY,100,3000); CHECK(app.ui.priority==100);
}
void test_ui_frame_never_reads_pet_or_waits(AppRuntime& app) {
    reset_actor(app); app.ui.base=Emotion::SAD; publish_ui(app);
    role("ui"); LedMatrix eyes; CHECK(eyes.init(pely::matrix_config())==ESP_OK);
    pely::UiState current; Emotion displayed=Emotion::IDLE;
    CHECK(render_ui_frame(app,eyes,current,displayed)==ESP_OK && displayed==Emotion::SAD);
    const int before=g_matrix_calls;
    CHECK(render_ui_frame(app,eyes,current,displayed)==ESP_OK && g_matrix_calls==before+1 && g_ui_tick_calls==1);
    CHECK(g_affection_calls==0 && g_food_calls==0);
    role("pet"); app.ui.base=Emotion::HAPPY; publish_ui(app);
    role("ui"); g_matrix_error=ESP_FAIL;
    CHECK(render_ui_frame(app,eyes,current,displayed)==ESP_FAIL && displayed==Emotion::SAD);
    CHECK(render_ui_frame(app,eyes,current,displayed)==ESP_OK && displayed==Emotion::HAPPY);
}
void test_death_overrides_stale_reactions(AppRuntime& app) {
    reset_actor(app); g_pet.alive=false; g_pet.mood=PetMood::DEAD;
    PetLogicEventData old; old.emotion=PetEmotion::HAPPY; old.priority=100;
    on_pet_event(&app,PET_LOGIC_EVENT,PET_EVENT_SPEECH_REQUESTED,&old);
    CHECK(app.ui.effective(0)==Emotion::DEAD && !(app.state->bits&kPetAlive));
    TouchSensorEvent touch; on_touch(&app,TOUCH_SENSOR_EVENT,TOUCH_EVENT_TOUCHED,&touch);
    CHECK(app.ui.effective(0)==Emotion::DEAD);
    g_pet=PetSnapshot{};
    on_pet_event(&app,PET_LOGIC_EVENT,PET_EVENT_MEMORY_ERASED,&old);
    CHECK(app.ui.effective(0)==Emotion::HUNGRY && (app.state->bits&kPetAlive));
}
void test_capture_failure_clears_payload(AppRuntime& app) {
    reset_actor(app); role("vision",1); CameraHal camera; VisionAI vision;
    CHECK(camera.init(pely::camera_config())==ESP_OK && vision.init(pely::vision_config(app.catalog))==ESP_OK);
    auto input=sample(99); g_capture_error=ESP_FAIL;
    CHECK(capture_vision_input(camera,vision,1,input)==ESP_FAIL && !input.frame_valid && !input.food_id[0]);
    g_capture_error=ESP_OK; g_infer_error=ESP_FAIL;
    CHECK(capture_vision_input(camera,vision,2,input)==ESP_FAIL && !g_frame_held && !input.frame_valid);
    g_infer_error=ESP_OK;
}
void test_catalog_validation_and_training_order(AppRuntime&) {
    const char* json=R"({"foods":[{"id":"apple","name":"Apple","aliases":["fruit"]},{"id":"milk","name":"Milk"}]})";
    pely::FoodCatalog catalog; CHECK(catalog.load(json)==ESP_OK);
    CHECK(catalog.select_model_labels(" milk, apple ",1)==ESP_OK);
    CHECK(catalog.food_count()==2 && std::strcmp(catalog.foods()[0].id,"milk")==0);
    CHECK(std::strcmp(catalog.labels()[0].food_id,"milk")==0 && !catalog.labels()[1].food_id[0]);
    CHECK(std::strcmp(catalog.labels()[2].food_id,"apple")==0);
    CHECK(catalog.select_model_labels("unknown",0)==ESP_ERR_INVALID_ARG);
    CHECK(catalog.select_model_labels("apple,apple",0)==ESP_ERR_INVALID_ARG);
    CHECK(catalog.select_model_labels("apple,",0)==ESP_ERR_INVALID_ARG);
    CHECK(catalog.select_model_labels("apple",2)==ESP_ERR_INVALID_ARG);
    CHECK(catalog.select_model_labels("",0)==ESP_OK && catalog.label_count()==0 && catalog.food_count()==2);
}
void test_startup_failure_releases_waiters(AppRuntime& app) {
    reset_actor(app); app.state->bits=0; g_cloud_error=ESP_FAIL;
    CHECK(!initialize_pet(app)); CHECK((app.state->bits&kPetFailed) && !(app.state->bits&kPetReady));
    CHECK(app.ui.effective(g_now)==Emotion::ANGRY);
    g_cloud_error=ESP_OK; reset_actor(app);
}
void test_optional_wifi_and_boundary_lengths(AppRuntime& app) {
#ifdef CONFIG_PELY_WIFI_ENABLED
    CHECK(g_station_creations==1 && g_wifi_connects==1);
    CHECK(std::memcmp(g_wifi_config.sta.ssid,CONFIG_PELY_WIFI_SSID,32)==0);
    CHECK(std::memcmp(g_wifi_config.sta.password,CONFIG_PELY_WIFI_PASSWORD,64)==0);
    role("wifi"); const int before=g_wifi_connects;
    for (int i=0;i<CONFIG_PELY_WIFI_MAX_RETRIES+1;++i) { on_wifi(&app,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,nullptr); }
    CHECK(g_wifi_connects==before+CONFIG_PELY_WIFI_MAX_RETRIES);
    on_wifi(&app,IP_EVENT,IP_EVENT_STA_GOT_IP,nullptr); CHECK(app.wifi_retries==0);
#else
    (void)app; CHECK(g_station_creations==0 && g_wifi_connects==0);
#endif
}
} // namespace

void mock_error(esp_err_t error,const char* expression) { if (error!=ESP_OK) { std::fprintf(stderr,"ESP_ERROR_CHECK: %s\n",expression); std::exit(1); } }
void mock_log(const char*,const char*,...) {}
const char* esp_err_to_name(esp_err_t) { return "host-status"; }
int xPortGetCoreID() { return g_core; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return const_cast<char*>(g_role); }
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(g_now/10000); }
int64_t esp_timer_get_time() { return g_now; }
void vTaskDelay(TickType_t ticks) { g_now+=ticks*10000; }
void vTaskDelayUntil(TickType_t* prior,TickType_t ticks) { *prior+=ticks; g_now=*prior*10000; }
void vTaskDelete(TaskHandle_t task) { CHECK(!task && !g_frame_held); ++g_deleted_tasks; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn,const char* name,uint32_t stack,void* arg,UBaseType_t priority,TaskHandle_t* out,BaseType_t core) {
    g_tasks.push_back({fn,name,stack,priority,core,arg}); *out=reinterpret_cast<void*>(g_tasks.size()); return pdPASS;
}
QueueHandle_t xQueueCreate(unsigned count,std::size_t size) {
    CHECK(count==1); auto q=std::make_unique<MockQueue>(); q->size=size; q->value.resize(size);
    auto* ptr=q.get(); g_queues.push_back(std::move(q)); return ptr;
}
BaseType_t xQueueOverwrite(QueueHandle_t q,const void* data) { CHECK(g_core==0); std::memcpy(q->value.data(),data,q->size); q->pending=true; return pdTRUE; }
BaseType_t xQueueReceive(QueueHandle_t q,void* data,TickType_t wait) {
    CHECK(wait==0); if (!q->pending) { return pdFALSE; }
    std::memcpy(data,q->value.data(),q->size); q->pending=false; return pdTRUE;
}
EventGroupHandle_t xEventGroupCreate() { auto p=std::make_unique<MockGroup>(); auto* out=p.get(); g_groups.push_back(std::move(p)); return out; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g,EventBits_t bits) { g->bits|=bits; return g->bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g,EventBits_t bits) { const auto old=g->bits; g->bits&=~bits; return old; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g,EventBits_t,BaseType_t,BaseType_t,TickType_t) { return g->bits; }
esp_err_t nvs_flash_init() { CHECK(g_core==0); return ESP_OK; }
esp_err_t esp_netif_init() { return ESP_OK; }
esp_err_t esp_event_loop_create_default() { return ESP_OK; }
esp_err_t esp_event_loop_create(const esp_event_loop_args_t* args,esp_event_loop_handle_t* out) {
    auto p=std::make_unique<MockLoop>(); p->args=*args; *out=p.get(); g_loops.push_back(std::move(p)); return ESP_OK;
}
esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t handle,esp_event_base_t base,int32_t id,esp_event_handler_t fn,void* arg) {
    static_cast<MockLoop*>(handle)->handlers.push_back({base,id,fn,arg}); return ESP_OK;
}
esp_err_t esp_event_handler_register(esp_event_base_t base,int32_t id,esp_event_handler_t fn,void* arg) {
    g_default_handlers.push_back({base,id,fn,arg}); return ESP_OK;
}
esp_err_t esp_event_post_to(esp_event_loop_handle_t handle,esp_event_base_t base,int32_t id,const void* data,std::size_t size,TickType_t wait) {
    CHECK(wait==0 && !g_frame_held); if (g_post_error!=ESP_OK) { return g_post_error; }
    const auto* bytes=static_cast<const uint8_t*>(data);
    static_cast<MockLoop*>(handle)->pending.push_back({base,id,{bytes,bytes+size}}); return ESP_OK;
}
esp_err_t esp_event_loop_run(esp_event_loop_handle_t handle,TickType_t) {
    CHECK(g_core==0 && std::strcmp(g_role,"pet")==0); auto& l=*static_cast<MockLoop*>(handle); unsigned count=0;
    while (!l.pending.empty()) {
        CHECK(++count<200); auto event=std::move(l.pending.front()); l.pending.pop_front();
        for (const auto& h:l.handlers) { if (h.base==event.base && (h.id==event.id || h.id==ESP_EVENT_ANY_ID)) { h.fn(h.arg,event.base,event.id,event.bytes.data()); } }
    }
    return ESP_OK;
}
esp_err_t gpio_install_isr_service(int) { CHECK(g_core==0); ++g_gpio_service; return ESP_OK; }
esp_netif_t* esp_netif_create_default_wifi_sta() { static esp_netif_t station; ++g_station_creations; return &station; }
esp_err_t esp_wifi_init(const wifi_init_config_t*) { return ESP_OK; }
esp_err_t esp_wifi_set_storage(wifi_storage_t) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t) { return ESP_OK; }
esp_err_t esp_wifi_set_config(wifi_interface_t,const wifi_config_t* cfg) { g_wifi_config=*cfg; return ESP_OK; }
esp_err_t esp_wifi_start() {
    for (const auto& h:g_default_handlers) { if (h.base==WIFI_EVENT && h.id==WIFI_EVENT_STA_START) { h.fn(h.arg,h.base,h.id,nullptr); } }
    return ESP_OK;
}
esp_err_t esp_wifi_connect() { ++g_wifi_connects; return ESP_OK; }

PetLogic::PetLogic() {}
PetLogic::~PetLogic() {}
esp_err_t PetLogic::init(esp_event_loop_handle_t loop,const PetFood* foods,std::size_t count) {
    CHECK(g_core==0 && std::strcmp(g_role,"pet")==0);
    if (g_pet_init_error!=ESP_OK) { return g_pet_init_error; }
    initialized_=true; event_loop_=loop; foods_=foods; food_count_=count; food_index_=0;
    std::snprintf(g_pet.requested_food_id,sizeof(g_pet.requested_food_id),"%s",foods[0].id); return ESP_OK;
}
esp_err_t PetLogic::get_state(PetSnapshot& out) const { CHECK(g_core==0 && std::strcmp(g_role,"pet")==0); out=g_pet; return ESP_OK; }
esp_err_t PetLogic::handle_confirmed_food(const char* id,BehaviorOutcome& outcome) {
    CHECK(g_core==0 && std::strcmp(g_role,"pet")==0); ++g_food_calls;
    outcome=g_force_outcome ? g_forced_outcome : (std::strcmp(id,g_pet.requested_food_id)==0 ? BehaviorOutcome::CORRECT : BehaviorOutcome::WRONG);
    if (outcome==BehaviorOutcome::CORRECT) { food_index_=(food_index_+1)%food_count_; std::snprintf(g_pet.requested_food_id,32,"%s",foods_[food_index_].id); }
    return g_action_error;
}
esp_err_t PetLogic::give_affection(float) { CHECK(g_core==0 && std::strcmp(g_role,"pet")==0); ++g_affection_calls; return g_pet.alive ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t PetLogic::update_tick() { return ESP_OK; }
esp_err_t PetLogic::announce_startup() { return ESP_OK; }
CloudSync::CloudSync() {}
CloudSync::~CloudSync() {}
esp_err_t CloudSync::init(PetLogic&,esp_event_loop_handle_t loop,const CloudSyncConfig&) {
    CHECK(g_core==0 && std::strcmp(g_role,"pet")==0 && !static_cast<MockLoop*>(loop)->args.task_name); ++g_cloud_init; return g_cloud_error;
}
LedMatrix::LedMatrix() {}
LedMatrix::~LedMatrix() {}
esp_err_t LedMatrix::init(const LedMatrixConfig&) { CHECK(g_core==0 && std::strcmp(g_role,"ui")==0); return ESP_OK; }
esp_err_t LedMatrix::show_emotion(Emotion e) {
    CHECK(g_core==0 && std::strcmp(g_role,"ui")==0); ++g_matrix_calls;
    const auto error=g_matrix_error; g_matrix_error=ESP_OK; if (error==ESP_OK) { g_displayed=e; } return error;
}
esp_err_t LedMatrix::update_tick() { CHECK(std::strcmp(g_role,"ui")==0); ++g_matrix_calls; ++g_ui_tick_calls; return ESP_OK; }
TouchSensor::~TouchSensor() {}
esp_err_t TouchSensor::init(const TouchSensorConfig& cfg) { CHECK(g_core==0 && std::strcmp(g_role,"touch")==0); config_=cfg; return ESP_OK; }
esp_err_t TouchSensor::wait_for_touch(TouchSensorEvent& out,TickType_t wait,esp_err_t* error) {
    CHECK(wait==portMAX_DELAY); if (g_touch_remaining--<=0) { return ESP_ERR_INVALID_STATE; }
    out=TouchSensorEvent{}; out.timestamp_us=g_now; out.sequence=1;
    *error=esp_event_post_to(config_.event_loop,TOUCH_SENSOR_EVENT,TOUCH_EVENT_TOUCHED,&out,sizeof(out),0); return ESP_OK;
}
esp_err_t TouchSensor::deinit() { CHECK(std::strcmp(g_role,"touch")==0); return ESP_OK; }
CameraHal::~CameraHal() { if (initialized_) { CHECK(deinit()==ESP_OK); } }
esp_err_t CameraHal::init(const CameraHalConfig&) {
    CHECK(g_core==1 && std::strcmp(g_role,"vision")==0);
    if (g_camera_error!=ESP_OK) { return g_camera_error; } initialized_=true; ++g_camera_live; return ESP_OK;
}
esp_err_t CameraHal::deinit() { CHECK(g_core==1 && !g_frame_held); initialized_=false; --g_camera_live; return ESP_OK; }
esp_err_t CameraHal::capture(Frame& frame) {
    CHECK(g_core==1 && !g_frame_held); if (g_capture_error!=ESP_OK) { return g_capture_error; }
    frame.buffer_=&g_frame; frame.owner_=this; g_frame_held=true; return ESP_OK;
}
CameraHal::Frame::~Frame() { reset(); }
void CameraHal::Frame::reset() { if (buffer_) { CHECK(g_core==1 && g_frame_held); g_frame_held=false; ++g_frame_returns; buffer_=nullptr; owner_=nullptr; } }
Rgb565FrameView CameraHal::Frame::view() const { return {buffer_->buf,buffer_->len,1,1,2}; }
VisionAI::~VisionAI() { if (model_) { CHECK(g_core==1); --g_model_live; } }
esp_err_t VisionAI::init(const VisionAiConfig& cfg) {
    CHECK(g_core==1 && cfg.event_loop==nullptr && cfg.label_count>=2);
    if (g_vision_error!=ESP_OK) { return g_vision_error; } model_=reinterpret_cast<dl::Model*>(1); ++g_model_live; return ESP_OK;
}
esp_err_t VisionAI::infer_rgb565(const Rgb565FrameView&,VisionResult& out,esp_err_t*) {
    CHECK(g_core==1 && g_frame_held); out=g_detection; return g_infer_error;
}

int main() {
    role("main"); app_main();
    auto& app=*static_cast<AppRuntime*>(g_tasks[0].arg);
    const struct { const char* name; void(*run)(AppRuntime&); } tests[]={
        {"boot task cores, AI stack, and manual pet dispatcher",test_boot_task_placement},
        {"touch task defers affection to pet task",test_touch_dispatch_is_asynchronous},
        {"vision frame lease released before asynchronous post",test_vision_capture_releases_before_post},
        {"one feeding action per presented card",test_held_card_is_consumed_once},
        {"cooldown retries and accepted-action backpressure",test_cooldown_and_output_backpressure},
        {"confirmation sequence gaps and counter wrap",test_confirmation_gaps_and_wrap},
        {"stale or failed frames cannot rearm a held card",test_stale_and_failed_frames_do_not_rearm},
        {"invalid confidence and consecutive-frame confirmation",test_bad_confidence_and_missing_frame_reset},
        {"latest UI snapshot preserves temporary reaction",test_ui_reaction_survives_state_coalescing},
        {"UI uses nonblocking mailbox and retries render errors",test_ui_frame_never_reads_pet_or_waits},
        {"death overrides queued happy reactions",test_death_overrides_stale_reactions},
        {"capture/inference errors clear old detection payload",test_capture_failure_clears_payload},
        {"catalog aliases and explicit model channel order",test_catalog_validation_and_training_order},
        {"storage startup failure releases waiting tasks",test_startup_failure_releases_waiters},
        {"optional Wi-Fi and maximum credential lengths",test_optional_wifi_and_boundary_lengths},
    };
    for (const auto& test:tests) { test.run(app); std::printf("PASS %s\n",test.name); }
    CHECK(!g_frame_held && !g_camera_live && !g_model_live);
    std::printf("All %zu runtime integration tests passed.\n",sizeof(tests)/sizeof(tests[0]));
}
