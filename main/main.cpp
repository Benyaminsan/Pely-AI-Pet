#include <algorithm>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "app_config.hpp"
#include "runtime_state.hpp"
#include "cloud_sync.hpp"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

ESP_EVENT_DEFINE_BASE(PELY_INPUT_EVENT);
extern const char food_catalog_json_start[] asm("_binary_food_catalog_json_start");

namespace {
constexpr char kTag[] = "pely";
constexpr int32_t kVisionFrameEvent = 1;
constexpr EventBits_t kPetReady = 1, kPetFailed = 2, kPetAlive = 4;
constexpr int64_t kPetTickUs = 1000000;
constexpr uint32_t kVisionStackBytes = 8192, kLogicStackBytes = 8192;
constexpr uint32_t kUiStackBytes = 4096, kTouchStackBytes = 4096;

struct AppRuntime {
    pely::FoodCatalog catalog;
    PetLogic pet;
    CloudSync cloud; // Destruction order keeps the borrowed pet alive until cloud stops.
    esp_event_loop_handle_t pet_loop = nullptr;
    QueueHandle_t ui_mailbox = nullptr;
    EventGroupHandle_t state = nullptr;
    pely::UiState ui;
    pely::VisionGate vision_gate;
    TaskHandle_t vision_task = nullptr, pet_task = nullptr, ui_task = nullptr, touch_task = nullptr;
    uint32_t wifi_retries = 0;
};
static_assert(std::is_trivially_copyable<pely::VisionInput>::value, "Input must be copied by esp_event");
static_assert(std::is_trivially_copyable<pely::UiState>::value, "UI mailbox must contain values");

void warn_limited(const char* operation, esp_err_t error, int64_t& last_log_us) {
    const int64_t now = esp_timer_get_time();
    if (last_log_us < 0 || now - last_log_us >= 1000000) {
        ESP_LOGW(kTag, "%s: %s", operation, esp_err_to_name(error));
        last_log_us = now;
    }
}

void publish_ui(AppRuntime& app) {
    // Exactly one slot: a newer composite snapshot replaces an older one without waiting.
    const BaseType_t sent = xQueueOverwrite(app.ui_mailbox, &app.ui);
    configASSERT(sent == pdTRUE);
    (void)sent;
}

void update_base(AppRuntime& app, const PetSnapshot& pet, bool new_game = false) {
    if (new_game) { app.ui = pely::UiState{}; app.vision_gate.reset(); }
    app.ui.set_pet(pet);
    if (pet.alive) { xEventGroupSetBits(app.state, kPetAlive); }
    else { xEventGroupClearBits(app.state, kPetAlive); }
}

void react(AppRuntime& app, Emotion emotion, uint8_t priority, uint32_t duration_ms) {
    app.ui.react(emotion, priority, duration_ms, esp_timer_get_time());
}

void food_reaction(AppRuntime& app, BehaviorOutcome outcome) {
    if (outcome == BehaviorOutcome::CORRECT) { react(app, Emotion::HAPPY, 100, 2800); }
    else if (outcome == BehaviorOutcome::WRONG) { react(app, Emotion::ANGRY, 90, 2200); }
}

void on_vision(void* argument, esp_event_base_t, int32_t, void* data) {
    if (!data) { return; }
    auto& app = *static_cast<AppRuntime*>(argument);
    const auto& frame = *static_cast<const pely::VisionInput*>(data);
    if (!app.vision_gate.observe(frame, esp_timer_get_time(), CONFIG_PELY_CONFIRM_FRAMES,
        CONFIG_PELY_CONFIDENCE_PERCENT / 100.0F, CONFIG_PELY_VISION_MAX_AGE_MS * int64_t{1000})) { return; }
    PetSnapshot snapshot;
    if (app.pet.get_state(snapshot) != ESP_OK || !snapshot.alive) { return; }
    BehaviorOutcome outcome;
    const esp_err_t error = app.pet.handle_confirmed_food(frame.food_id, outcome);
    if (outcome == BehaviorOutcome::CORRECT || outcome == BehaviorOutcome::WRONG) {
        app.vision_gate.consume(frame.food_id); // Accepted even if its output event queue was full.
    }
    if (error != ESP_OK) { ESP_LOGW(kTag, "Food action: %s", esp_err_to_name(error)); }
    if (app.pet.get_state(snapshot) == ESP_OK) {
        update_base(app, snapshot);
        food_reaction(app, outcome);
        publish_ui(app);
    }
}

void on_touch(void* argument, esp_event_base_t, int32_t, void* data) {
    if (!data) { return; }
    auto& app = *static_cast<AppRuntime*>(argument);
    const esp_err_t error = app.pet.give_affection();
    PetSnapshot snapshot;
    if (app.pet.get_state(snapshot) != ESP_OK) { return; }
    update_base(app, snapshot);
    if (snapshot.alive) { react(app, Emotion::HAPPY, 95, 2800); }
    if (error != ESP_OK && snapshot.alive) { ESP_LOGW(kTag, "Affection: %s", esp_err_to_name(error)); }
    publish_ui(app);
}

void on_pet_event(void* argument, esp_event_base_t, int32_t id, void* data) {
    if (!data) { return; }
    auto& app = *static_cast<AppRuntime*>(argument);
    const auto& event = *static_cast<const PetLogicEventData*>(data);
    // Older queued snapshots must not restore happy eyes after a newer death/reset.
    PetSnapshot current;
    if (app.pet.get_state(current) != ESP_OK) { return; }
    update_base(app, current, id == PET_EVENT_MEMORY_ERASED && current.alive);
    if (id == PET_EVENT_FOOD_EVALUATED) { food_reaction(app, event.outcome); }
    else if (id == PET_EVENT_SPEECH_REQUESTED && current.alive) {
        Emotion emotion = Emotion::IDLE;
        switch (event.emotion) {
        case PetEmotion::HAPPY: case PetEmotion::EXCITED: emotion = Emotion::HAPPY; break;
        case PetEmotion::CONFUSED: emotion = Emotion::CONFUSED; break;
        case PetEmotion::HUNGRY: emotion = Emotion::HUNGRY; break;
        case PetEmotion::LONELY: emotion = Emotion::LONELY; break;
        case PetEmotion::SLEEPY: emotion = Emotion::SLEEPY; break;
        case PetEmotion::SAD: emotion = Emotion::SAD; break;
        case PetEmotion::NEUTRAL: break;
        }
        react(app, emotion, event.priority, 2500);
        ESP_LOGI(kTag, "Pet: %s", event.speech); // A future audio component can consume this intent.
    }
    publish_ui(app);
}

bool wait_for_pet(AppRuntime& app) {
    return (xEventGroupWaitBits(app.state, kPetReady | kPetFailed, pdFALSE, pdFALSE, portMAX_DELAY) & kPetReady) != 0;
}

esp_err_t capture_vision_input(CameraHal& camera, VisionAI& vision, uint32_t sequence, pely::VisionInput& input) {
    input = pely::VisionInput{};
    input.sequence = sequence;
    input.observed_us = esp_timer_get_time();
    CameraHal::Frame frame;
    esp_err_t error = camera.capture(frame);
    if (error != ESP_OK) { return error; }
    VisionResult result;
    error = vision.infer_rgb565(frame.view(), result);
    input.frame_valid = error == ESP_OK;
    if (error == ESP_OK && result.count) {
        std::memcpy(input.food_id, result.detections[0].food_id, sizeof(input.food_id));
        input.food_id[sizeof(input.food_id) - 1] = '\0';
        input.confidence = result.detections[0].confidence;
    }
    return error; // The borrowed camera frame is released before this returns.
}

void run_vision(AppRuntime& app) {
    if (app.catalog.label_count() == 0) {
        ESP_LOGW(kTag, "Vision idle: set the exported FOMO label order in menuconfig > Pely application");
        return;
    }
    CameraHal camera;
    VisionAI vision;
    esp_err_t error = camera.init(pely::camera_config());
    if (error == ESP_OK) { error = vision.init(pely::vision_config(app.catalog)); }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "Vision init: %s; verify OV5640 pins and model metadata", esp_err_to_name(error));
        return;
    }
    uint32_t sequence = 0;
    int64_t last_log_us = -1;
    for (;;) {
        xEventGroupWaitBits(app.state, kPetAlive, pdFALSE, pdTRUE, portMAX_DELAY);
        pely::VisionInput input;
        error = capture_vision_input(camera, vision, ++sequence, input);
        if (error != ESP_OK) { warn_limited("Vision frame", error, last_log_us); }
        const esp_err_t posted = esp_event_post_to(app.pet_loop, PELY_INPUT_EVENT,
            kVisionFrameEvent, &input, sizeof(input), 0);
        if (posted != ESP_OK) { warn_limited("Vision input queue", posted, last_log_us); }
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(80)));
    }
}

void task_vision_ai(void* argument) {
    auto& app = *static_cast<AppRuntime*>(argument);
    if (wait_for_pet(app)) { run_vision(app); }
    vTaskDelete(nullptr); // run_vision's RAII owners were destroyed on Core 1 first.
}

void run_touch(AppRuntime& app) {
    TouchSensor sensor; // xTaskCreatePinnedToCore allocates this task's stack in internal RAM.
    const esp_err_t initialized = sensor.init(pely::touch_config(app.pet_loop));
    if (initialized != ESP_OK) {
        ESP_LOGE(kTag, "Touch init: %s; configure its GPIO/pad in menuconfig", esp_err_to_name(initialized));
        return;
    }
    int64_t last_log_us = -1;
    for (;;) {
        TouchSensorEvent event;
        esp_err_t posted = ESP_OK;
        const esp_err_t error = sensor.wait_for_touch(event, portMAX_DELAY, &posted);
        if (error == ESP_ERR_INVALID_STATE) { break; }
        if (error != ESP_OK) { warn_limited("Touch input", error, last_log_us); }
        if (posted != ESP_OK) { warn_limited("Touch input queue", posted, last_log_us); }
        // The module posts to pet_loop. Do not also apply affection here.
    }
    ESP_ERROR_CHECK(sensor.deinit());
}

void task_touch_sensor(void* argument) {
    auto& app = *static_cast<AppRuntime*>(argument);
    if (wait_for_pet(app)) { run_touch(app); }
    vTaskDelete(nullptr);
}

bool initialize_pet(AppRuntime& app) {
    esp_err_t error = app.pet.init(app.pet_loop, app.catalog.foods(), app.catalog.food_count());
    if (error == ESP_OK) { error = app.cloud.init(app.pet, app.pet_loop); }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "State storage init: %s; saved data was not automatically erased", esp_err_to_name(error));
        app.ui.base = Emotion::ANGRY;
        publish_ui(app);
        xEventGroupSetBits(app.state, kPetFailed);
        return false;
    }
    PetSnapshot snapshot;
    ESP_ERROR_CHECK(app.pet.get_state(snapshot));
    update_base(app, snapshot);
    publish_ui(app);
    xEventGroupSetBits(app.state, kPetReady);
    if (snapshot.alive) {
        error = app.pet.announce_startup();
        if (error != ESP_OK) { ESP_LOGW(kTag, "Startup intent: %s", esp_err_to_name(error)); }
    }
    return true;
}

void task_pet_logic(void* argument) {
    auto& app = *static_cast<AppRuntime*>(argument);
    if (!initialize_pet(app)) { vTaskDelete(nullptr); return; }
    PetSnapshot snapshot;
    esp_err_t error;
    int64_t next_tick_us = esp_timer_get_time() + kPetTickUs;
    int64_t last_log_us = -1;
    for (;;) {
        const int64_t remaining_us = next_tick_us - esp_timer_get_time();
        const TickType_t wait = remaining_us <= 0 ? 0 : std::max<TickType_t>(1,
            pdMS_TO_TICKS(static_cast<uint32_t>((remaining_us + 999) / 1000)));
        // No dedicated event-loop task: all input handlers and cloud's pet observer
        // execute HERE, on the sole state-owner task, never inside task_ui.
        error = esp_event_loop_run(app.pet_loop, wait);
        if (error != ESP_OK) {
            warn_limited("Pet event loop", error, last_log_us);
            vTaskDelay(1);
        }
        const int64_t now = esp_timer_get_time();
        if (now < next_tick_us) { continue; }
        error = app.pet.update_tick();
        if (error != ESP_OK) { warn_limited("Pet tick", error, last_log_us); }
        if (app.pet.get_state(snapshot) == ESP_OK) { update_base(app, snapshot); publish_ui(app); }
        next_tick_us = now + kPetTickUs; // PetLogic integrates actual elapsed time; no catch-up busy loop.
    }
}

esp_err_t render_ui_frame(AppRuntime& app, LedMatrix& eyes, pely::UiState& current, Emotion& displayed) {
    xQueueReceive(app.ui_mailbox, &current, 0);
    const Emotion selected = current.effective(esp_timer_get_time());
    if (selected != displayed) {
        const esp_err_t error = eyes.show_emotion(selected);
        if (error == ESP_OK) { displayed = selected; }
        return error;
    }
    return eyes.update_tick();
}

void run_ui(AppRuntime& app) {
    LedMatrix eyes;
    const esp_err_t initialized = eyes.init(pely::matrix_config());
    if (initialized != ESP_OK) {
        ESP_LOGE(kTag, "Matrix init: %s; configure WS2812 GPIO/layout in menuconfig", esp_err_to_name(initialized));
        return;
    }
    pely::UiState current;
    Emotion displayed = Emotion::IDLE;
    TickType_t last_wake = xTaskGetTickCount();
    int64_t last_log_us = -1;
    for (;;) {
        const esp_err_t error = render_ui_frame(app, eyes, current, displayed);
        if (error != ESP_OK) { warn_limited("Eye render", error, last_log_us); }
        vTaskDelayUntil(&last_wake, std::max<TickType_t>(1, pdMS_TO_TICKS(33)));
    }
}

void task_ui(void* argument) {
    run_ui(*static_cast<AppRuntime*>(argument));
    vTaskDelete(nullptr);
}

#ifdef CONFIG_PELY_WIFI_ENABLED
void on_wifi(void* argument, esp_event_base_t base, int32_t id, void*) {
    auto& app = *static_cast<AppRuntime*>(argument);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        app.wifi_retries = 0;
        ESP_LOGI(kTag, "Wi-Fi station ready");
    } else if (id == WIFI_EVENT_STA_START ||
               (id == WIFI_EVENT_STA_DISCONNECTED && app.wifi_retries++ < CONFIG_PELY_WIFI_MAX_RETRIES)) {
        const esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) { ESP_LOGW(kTag, "Wi-Fi connect: %s", esp_err_to_name(error)); }
    }
}
#endif

void start_optional_wifi(AppRuntime& app) {
#ifdef CONFIG_PELY_WIFI_ENABLED
    wifi_config_t wifi{};
    const std::size_t ssid_length = std::strlen(CONFIG_PELY_WIFI_SSID);
    const std::size_t password_length = std::strlen(CONFIG_PELY_WIFI_PASSWORD);
    if (!ssid_length || ssid_length > sizeof(wifi.sta.ssid) || password_length > sizeof(wifi.sta.password) ||
        (password_length && password_length < 8)) {
        ESP_LOGE(kTag, "Wi-Fi configuration is incomplete; continuing offline");
        return;
    }
    esp_netif_t* station = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(station ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t driver = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&driver));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, on_wifi, &app));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi, &app));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, &app));
    std::memcpy(wifi.sta.ssid, CONFIG_PELY_WIFI_SSID, ssid_length);
    std::memcpy(wifi.sta.password, CONFIG_PELY_WIFI_PASSWORD, password_length);
    wifi.sta.threshold.authmode = password_length ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
#else
    (void)app;
    ESP_LOGI(kTag, "Offline-first mode; Wi-Fi can be enabled in menuconfig");
#endif
}

void create_task(TaskFunction_t function, const char* name, uint32_t stack_bytes,
                 AppRuntime& app, UBaseType_t priority, TaskHandle_t& handle, BaseType_t core) {
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(function, name, stack_bytes, &app, priority, &handle, core) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM);
}
} // namespace

extern "C" void app_main() {
    configASSERT(xPortGetCoreID() == 0);
    static DRAM_ATTR AppRuntime app; // Outlives app_main; no task receives a pointer into its transient stack.
    ESP_ERROR_CHECK(nvs_flash_init()); // Preserve saved data on errors; never auto erase the NVS partition.
    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t default_loop = esp_event_loop_create_default();
    ESP_ERROR_CHECK(default_loop == ESP_ERR_INVALID_STATE ? ESP_OK : default_loop);
    ESP_ERROR_CHECK(app.catalog.load(food_catalog_json_start));
    ESP_ERROR_CHECK(app.catalog.select_model_labels(CONFIG_PELY_FOMO_LABEL_IDS, CONFIG_PELY_FOMO_BACKGROUND_CHANNEL));
    app.ui_mailbox = xQueueCreate(1, sizeof(pely::UiState));
    app.state = xEventGroupCreate();
    ESP_ERROR_CHECK(app.ui_mailbox && app.state ? ESP_OK : ESP_ERR_NO_MEM);

    esp_event_loop_args_t loop{};
    loop.queue_size = 32;
    loop.task_name = nullptr; // task_pet_logic is the sole dispatcher.
    ESP_ERROR_CHECK(esp_event_loop_create(&loop, &app.pet_loop));
    ESP_ERROR_CHECK(esp_event_handler_register_with(app.pet_loop, PELY_INPUT_EVENT, kVisionFrameEvent, on_vision, &app));
    ESP_ERROR_CHECK(esp_event_handler_register_with(app.pet_loop, TOUCH_SENSOR_EVENT, TOUCH_EVENT_TOUCHED, on_touch, &app));
    ESP_ERROR_CHECK(esp_event_handler_register_with(app.pet_loop, PET_LOGIC_EVENT, ESP_EVENT_ANY_ID, on_pet_event, &app));
#if defined(CONFIG_PELY_TOUCH_TTP223) && CONFIG_PELY_TOUCH_GPIO >= 0
    // Install the shared GPIO ISR service on Core 0 before any Core-1 driver can use it.
    const esp_err_t gpio_isr = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    ESP_ERROR_CHECK(gpio_isr == ESP_ERR_INVALID_STATE ? ESP_OK : gpio_isr);
#endif
    create_task(task_ui, "task_ui", kUiStackBytes, app, 5, app.ui_task, 0);
    create_task(task_pet_logic, "task_pet_logic", kLogicStackBytes, app, 4, app.pet_task, 0);
    create_task(task_vision_ai, "task_vision_ai", kVisionStackBytes, app, 4, app.vision_task, 1);
    create_task(task_touch_sensor, "task_touch_sensor", kTouchStackBytes, app, 5, app.touch_task, 0);
    start_optional_wifi(app);
}
