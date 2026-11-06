#include "led_matrix.hpp"
#include "eye_animation.hpp"
#include "touch_sensor.hpp"
#include "driver/gpio.h"
#include "driver/touch_pad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <set>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(false)
struct FakeQueue { unsigned capacity; std::size_t size; std::deque<std::vector<uint8_t>> values; };
struct FakeMutex { bool held=false; };
struct led_strip_t { std::vector<EyeRgb> pixels; };

namespace {
int core=0;
TaskHandle_t task=reinterpret_cast<void*>(1);
int64_t now_us=0;
bool in_isr=false, internal_object=true, fail_queue=false, fail_mutex=false, gpio_service=false;
bool gpio_enabled=false, pad_live=false, fsm=false, pad_enabled=false;
gpio_isr_t gpio_handler=nullptr, pad_handler=nullptr;
void* gpio_arg=nullptr; void* pad_arg=nullptr;
uint32_t pad_status=0, benchmark=1000, threshold=0;
touch_pad_t current_pad=7;
gpio_int_type_t gpio_edge=GPIO_INTR_DISABLE;
TickType_t last_wait=0;
int queues=0, mutexes=0, strips=0, benchmark_reads=0, resumed=0, yields=0, refreshes=0, delays=0;
int allocation_budget=-1;
esp_err_t post_error=ESP_OK, remove_error=ESP_OK, pad_deinit_error=ESP_OK;
esp_err_t strip_new_error=ESP_OK, strip_pixel_error=ESP_OK, strip_refresh_error=ESP_OK;
esp_err_t strip_clear_error=ESP_OK, strip_delete_error=ESP_OK;
std::set<void*> allocated;
std::vector<TouchSensorEvent> touches;
std::vector<EyeRgb> shown;
led_strip_config_t last_strip_config{};
led_strip_rmt_config_t last_rmt_config{};

esp_err_t consume(esp_err_t& value) { const auto result=value; value=ESP_OK; return result; }
void reset() {
    CHECK(allocated.empty() && !queues && !mutexes && !strips && !gpio_handler && !pad_handler && !pad_live);
    core=0; task=reinterpret_cast<void*>(1); now_us=0; in_isr=false; internal_object=true;
    fail_queue=fail_mutex=gpio_service=gpio_enabled=fsm=pad_enabled=false;
    benchmark=1000; threshold=pad_status=0; current_pad=7;
    last_wait=0; benchmark_reads=resumed=yields=refreshes=delays=0; allocation_budget=-1;
    post_error=remove_error=pad_deinit_error=strip_new_error=strip_pixel_error=strip_refresh_error=ESP_OK;
    strip_clear_error=strip_delete_error=ESP_OK; touches.clear(); shown.clear();
}
void milliseconds(int64_t ms) { now_us=ms*1000; }
void fire_gpio(int64_t ms) {
    milliseconds(ms); CHECK(gpio_handler && gpio_enabled);
    in_isr=true; gpio_handler(gpio_arg); in_isr=false;
}
void fire_pad(int64_t ms,uint32_t status=TOUCH_PAD_INTR_MASK_ACTIVE,int channel=7) {
    milliseconds(ms); CHECK(pad_handler && pad_enabled);
    pad_status=status; current_pad=channel;
    in_isr=true; pad_handler(pad_arg); in_isr=false;
}
TouchSensorConfig touch_config(bool internal=false) {
    TouchSensorConfig cfg; cfg.gpio_num=4; cfg.pad_channel=7;
    cfg.backend=internal ? TouchBackend::INTERNAL_PAD : TouchBackend::TTP223_GPIO;
    cfg.event_loop=reinterpret_cast<void*>(2); return cfg;
}
LedMatrixConfig matrix_config() {
    LedMatrixConfig cfg; cfg.gpio_num=18; cfg.auto_blink=false; cfg.transition_ms=0;
    cfg.brightness=255; cfg.serpentine=false; cfg.happy_bounce_pixels=0; return cfg;
}
bool equal(const std::vector<EyeRgb>& a,const std::vector<EyeRgb>& b) {
    return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(EyeRgb))==0;
}
bool lit(EyeRgb c) { return c.red || c.green || c.blue; }
std::vector<EyeRgb> render(Emotion state,LedMatrixConfig cfg=matrix_config()) {
    LedMatrix matrix; CHECK(matrix.init(cfg)==ESP_OK); CHECK(matrix.show_emotion(state)==ESP_OK); return shown;
}
void near(double value,double expected) { CHECK(std::fabs(value-expected)<0.0001); }

void test_gpio_isr_defers_work() {
    reset(); TouchSensor sensor; CHECK(sensor.init(touch_config())==ESP_OK);
    CHECK(gpio_edge==GPIO_INTR_POSEDGE); fire_gpio(0);
    CHECK(touches.empty() && yields==1);
    TouchSensorEvent event; CHECK(sensor.wait_for_touch(event,0)==ESP_OK);
    CHECK(event.timestamp_us==0 && event.sequence==1 && event.gpio_num==4 && touches.size()==1);
}
void test_python_debounce_and_timestamp() {
    reset(); TouchSensor sensor; CHECK(sensor.init(touch_config())==ESP_OK);
    fire_gpio(1000); fire_gpio(1200); fire_gpio(1500);
    TouchSensorEvent event;
    CHECK(sensor.wait_for_touch(event,0)==ESP_OK && event.timestamp_us==1000000);
    CHECK(sensor.wait_for_touch(event,0)==ESP_OK && event.timestamp_us==1500000 && event.sequence==2);
    CHECK(sensor.wait_for_touch(event,0)==ESP_ERR_TIMEOUT && touches.size()==2);
}
void test_blocking_idle_and_stop() {
    reset(); TouchSensor sensor; CHECK(sensor.init(touch_config())==ESP_OK);
    TouchSensorEvent event; CHECK(sensor.wait_for_touch(event)==ESP_ERR_TIMEOUT && last_wait==portMAX_DELAY);
    CHECK(delays==0 && benchmark_reads==0); // GPIO runtime never samples levels or capacitance.
    task=reinterpret_cast<void*>(3); CHECK(sensor.request_stop()==ESP_OK);
    task=reinterpret_cast<void*>(1); CHECK(sensor.wait_for_touch(event)==ESP_ERR_INVALID_STATE);
    CHECK(touches.empty()); CHECK(sensor.deinit()==ESP_OK && !gpio_handler && !queues && gpio_service);
}
void test_active_low_and_service_reuse() {
    reset(); gpio_service=true; auto cfg=touch_config(); cfg.active_high=false;
    TouchSensor sensor; CHECK(sensor.init(cfg)==ESP_OK && gpio_edge==GPIO_INTR_NEGEDGE);
    fire_gpio(1); TouchSensorEvent event; CHECK(sensor.wait_for_touch(event,0)==ESP_OK);
}
void test_internal_pad_threshold_and_interrupt() {
    reset(); TouchSensor sensor; CHECK(sensor.init(touch_config(true))==ESP_OK);
    CHECK(threshold==200 && sensor.calibrated_threshold()==200 && benchmark_reads==1 && delays==1);
    fire_pad(200,TOUCH_PAD_INTR_MASK_INACTIVE); fire_pad(300,TOUCH_PAD_INTR_MASK_ACTIVE,8);
    TouchSensorEvent event; CHECK(sensor.wait_for_touch(event,0)==ESP_ERR_TIMEOUT);
    fire_pad(400); CHECK(sensor.wait_for_touch(event,0)==ESP_OK && event.pad_channel==7);
    CHECK(benchmark_reads==1 && touches.size()==1); // No runtime read loop.
}
void test_pad_timeout_and_full_queue_recovery() {
    reset(); auto cfg=touch_config(true); cfg.queue_length=1;
    TouchSensor sensor; CHECK(sensor.init(cfg)==ESP_OK);
    fire_pad(200); fire_pad(201,TOUCH_PAD_INTR_MASK_TIMEOUT);
    CHECK(sensor.dropped_interrupts()==1);
    TouchSensorEvent event;
    CHECK(sensor.wait_for_touch(event,0)==ESP_ERR_INVALID_RESPONSE && resumed==1 && touches.empty());
    CHECK(sensor.wait_for_touch(event,0)==ESP_OK && touches.size()==1);
    fire_pad(600,TOUCH_PAD_INTR_MASK_TIMEOUT);
    CHECK(sensor.wait_for_touch(event,0)==ESP_ERR_INVALID_RESPONSE && resumed==2 && touches.size()==1);
}
void test_touch_backpressure() {
    reset(); auto cfg=touch_config(); cfg.queue_length=1;
    TouchSensor sensor; CHECK(sensor.init(cfg)==ESP_OK);
    fire_gpio(0); fire_gpio(1); CHECK(sensor.dropped_interrupts()==1);
    post_error=ESP_ERR_TIMEOUT; TouchSensorEvent event; esp_err_t posted=ESP_OK;
    CHECK(sensor.wait_for_touch(event,0,&posted)==ESP_OK && posted==ESP_ERR_TIMEOUT && event.sequence==1);
    post_error=ESP_OK; fire_gpio(100);
    CHECK(sensor.wait_for_touch(event,0)==ESP_ERR_TIMEOUT && touches.empty());
}
void test_touch_init_validation_and_cleanup() {
    reset(); TouchSensor sensor;
    core=1; CHECK(sensor.init(touch_config())==ESP_ERR_INVALID_STATE); core=0;
    internal_object=false; CHECK(sensor.init(touch_config())==ESP_ERR_INVALID_ARG); internal_object=true;
    auto cfg=touch_config(); cfg.event_loop=nullptr; CHECK(sensor.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=touch_config(true); cfg.pad_channel=0; CHECK(sensor.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=touch_config(true); cfg.threshold_ratio=std::numeric_limits<float>::quiet_NaN();
    CHECK(sensor.init(cfg)==ESP_ERR_INVALID_ARG);
    fail_queue=true; CHECK(sensor.init(touch_config())==ESP_ERR_NO_MEM); fail_queue=false;
    benchmark=0; CHECK(sensor.init(touch_config(true))==ESP_ERR_INVALID_RESPONSE && !pad_live && !queues);
    benchmark=1000; CHECK(sensor.init(touch_config())==ESP_OK);
    remove_error=ESP_FAIL; CHECK(sensor.deinit()==ESP_FAIL && queues==1 && gpio_handler && !gpio_enabled);
    CHECK(sensor.deinit()==ESP_OK && !queues && !gpio_handler);
}
void test_pad_cleanup_retry_and_owner() {
    reset(); TouchSensor sensor; CHECK(sensor.init(touch_config(true))==ESP_OK);
    task=reinterpret_cast<void*>(3); TouchSensorEvent event;
    CHECK(sensor.wait_for_touch(event,0)==ESP_ERR_INVALID_STATE && sensor.deinit()==ESP_ERR_INVALID_STATE);
    task=reinterpret_cast<void*>(1); pad_deinit_error=ESP_FAIL;
    CHECK(sensor.deinit()==ESP_FAIL && queues==1 && !pad_handler && !fsm);
    CHECK(sensor.deinit()==ESP_OK && !queues && !pad_live);
}
void test_animation_math() {
    reset(); using namespace eye_animation;
    near(lerp(10,20,0.5F),15); near(lerp(10,20,-1),10); near(lerp(10,20,2),20);
    near(ease_in_out(0),0); near(ease_in_out(0.5F),0.5); near(ease_in_out(1),1);
    near(smoothstep(5,5,5),0); near(smoothstep(0,1,0.5F),0.5);
    near(ease_in_cubic(0.5F),0.125); near(ease_out_cubic(0.5F),0.875);
    near(ease_out_elastic(0),0); near(ease_out_elastic(1),1); CHECK(ease_out_elastic(0.15F)>1);
    near(calculate_blink_progress(0,0.2F),1); near(calculate_blink_progress(0.1F,0.2F),0);
    near(calculate_blink_progress(0.2F,0.2F),1); near(calculate_blink_progress(0,-1),1);
}
void test_rmt_configuration() {
    reset(); LedMatrix matrix; CHECK(matrix.init(matrix_config())==ESP_OK);
    CHECK(last_strip_config.led_model==LED_MODEL_WS2812 && last_strip_config.max_leds==128);
    const auto fmt=last_strip_config.color_component_format.format;
    CHECK(fmt.r_pos==1 && fmt.g_pos==0 && fmt.b_pos==2 && fmt.num_components==3 && fmt.bytes_per_color==1);
    CHECK(last_rmt_config.resolution_hz==10000000 && last_rmt_config.flags.with_dma);
    CHECK(last_rmt_config.mem_block_symbols==256 && allocated.size()==2 && refreshes==1);
}
void test_emotion_masks_and_palette() {
    reset(); const auto happy=render(Emotion::HAPPY), sad=render(Emotion::SAD), angry=render(Emotion::ANGRY);
    CHECK(!equal(happy,sad) && !equal(sad,angry) && !equal(happy,angry));
    CHECK(std::count_if(happy.begin(),happy.end(),lit)>0);
    CHECK(std::count_if(sad.begin(),sad.end(),lit)>0 && std::count_if(angry.begin(),angry.end(),lit)>0);
    for (const auto& p:happy) { CHECK(!lit(p) || (p.red==32 && p.green==255 && p.blue==96)); }
    for (const auto& p:sad) { CHECK(!lit(p) || (p.red==32 && p.green==96 && p.blue==255)); }
    for (const auto& p:angry) { CHECK(!lit(p) || (p.red==255 && p.green==40 && p.blue==16)); }
    for (unsigned state=0;state<=static_cast<unsigned>(Emotion::DEAD);++state) {
        const auto frame=render(static_cast<Emotion>(state));
        CHECK(std::count_if(frame.begin(),frame.end(),lit)>0);
    }
}
void test_smooth_transition_and_repeated_state() {
    reset(); const auto target=render(Emotion::HAPPY); auto cfg=matrix_config(); cfg.transition_ms=280;
    LedMatrix matrix; CHECK(matrix.init(cfg)==ESP_OK); const auto start=shown;
    CHECK(matrix.show_emotion(Emotion::HAPPY)==ESP_OK && equal(shown,start));
    milliseconds(140); CHECK(matrix.update_tick()==ESP_OK); const auto middle=shown;
    CHECK(!equal(middle,start) && !equal(middle,target));
    CHECK(matrix.show_emotion(Emotion::HAPPY)==ESP_OK);
    milliseconds(280); CHECK(matrix.update_tick()==ESP_OK && equal(shown,target));
}
void test_brightness_and_clear() {
    reset(); auto cfg=matrix_config(); cfg.brightness=128; cfg.idle_color={255,255,255};
    LedMatrix matrix; CHECK(matrix.init(cfg)==ESP_OK); const auto half=shown;
    CHECK(matrix.set_brightness(128)==ESP_OK && equal(shown,half));
    for (const auto& p:shown) { CHECK(!lit(p) || (p.red==128 && p.green==128 && p.blue==128)); }
    CHECK(matrix.set_brightness(0)==ESP_OK && std::none_of(shown.begin(),shown.end(),lit));
    CHECK(matrix.set_brightness(255)==ESP_OK && std::any_of(shown.begin(),shown.end(),lit));
    CHECK(matrix.clear()==ESP_OK); const int before=refreshes;
    CHECK(matrix.update_tick()==ESP_OK && refreshes==before && std::none_of(shown.begin(),shown.end(),lit));
    CHECK(matrix.show_emotion(Emotion::HAPPY)==ESP_OK && std::any_of(shown.begin(),shown.end(),lit));
}
void test_blink_and_dead_expression() {
    reset(); auto cfg=matrix_config(); cfg.auto_blink=true; cfg.blink_min_ms=cfg.blink_max_ms=100;
    LedMatrix matrix; CHECK(matrix.init(cfg)==ESP_OK); const auto open=shown;
    milliseconds(100); CHECK(matrix.update_tick()==ESP_OK);
    milliseconds(180); CHECK(matrix.update_tick()==ESP_OK && !equal(shown,open));
    milliseconds(260); CHECK(matrix.update_tick()==ESP_OK && equal(shown,open));
    CHECK(matrix.show_emotion(Emotion::DEAD)==ESP_OK); const auto dead=shown;
    milliseconds(10000); CHECK(matrix.update_tick()==ESP_OK && equal(shown,dead));
}
void test_matrix_wiring() {
    reset(); auto base=matrix_config(); base.height=16;
    const auto logical=render(Emotion::ANGRY,base);
    auto cfg=base; cfg.tile_width=cfg.tile_height=8; cfg.serpentine=true;
    cfg.serpentine_tiles=true; cfg.major=MatrixMajor::COLUMNS; cfg.mirror_x=cfg.mirror_y=true;
    const auto physical=render(Emotion::ANGRY,cfg);
    std::set<unsigned> indices;
    for (unsigned y=0;y<16;++y) { for (unsigned x=0;x<16;++x) {
        const unsigned mx=15-x,my=15-y,ty=my/8;
        const unsigned tx=(ty&1) ? 1-mx/8 : mx/8;
        const unsigned lx=mx%8,ly=(lx&1) ? 7-my%8 : my%8;
        const unsigned index=(ty*2+tx)*64+lx*8+ly;
        indices.insert(index);
        CHECK(std::memcmp(&logical[y*16+x],&physical[index],sizeof(EyeRgb))==0);
    }}
    CHECK(indices.size()==256 && *indices.rbegin()==255);
}
void test_matrix_errors_and_core() {
    reset(); LedMatrix matrix; CHECK(matrix.show_emotion(Emotion::HAPPY)==ESP_ERR_INVALID_STATE);
    auto cfg=matrix_config(); cfg.gpio_num=-1; CHECK(matrix.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=matrix_config(); cfg.tile_width=8; CHECK(matrix.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=matrix_config(); cfg.happy_bounce_pixels=std::numeric_limits<float>::quiet_NaN();
    CHECK(matrix.init(cfg)==ESP_ERR_INVALID_ARG); cfg=matrix_config();
    core=1; CHECK(matrix.init(cfg)==ESP_ERR_INVALID_STATE); core=0;
    allocation_budget=1; CHECK(matrix.init(cfg)==ESP_ERR_NO_MEM && allocated.empty()); allocation_budget=-1;
    strip_new_error=ESP_FAIL; CHECK(matrix.init(cfg)==ESP_FAIL && allocated.empty());
    CHECK(matrix.init(cfg)==ESP_OK);
    CHECK(matrix.show_emotion(static_cast<Emotion>(99))==ESP_ERR_INVALID_ARG);
    core=1; CHECK(matrix.update_tick()==ESP_ERR_INVALID_STATE); core=0;
    task=reinterpret_cast<void*>(3); CHECK(matrix.show_emotion(Emotion::SAD)==ESP_OK);
    task=reinterpret_cast<void*>(1); strip_pixel_error=ESP_FAIL; CHECK(matrix.update_tick()==ESP_FAIL);
    strip_refresh_error=ESP_FAIL; CHECK(matrix.update_tick()==ESP_FAIL); CHECK(matrix.update_tick()==ESP_OK);
    strip_delete_error=ESP_FAIL; CHECK(matrix.deinit()==ESP_FAIL && strips==1);
    CHECK(matrix.deinit()==ESP_OK && !strips && allocated.empty());
}

void write_preview(const char* path) {
    reset(); const std::vector<std::vector<EyeRgb>> frames={render(Emotion::HAPPY),render(Emotion::SAD),render(Emotion::ANGRY)};
    FILE* file=std::fopen(path,"wb"); CHECK(file);
    std::fprintf(file,"P6\n48 8\n255\n");
    for (unsigned y=0;y<8;++y) { for (const auto& frame:frames) { for (unsigned x=0;x<16;++x) {
        const auto p=frame[y*16+x]; std::fputc(p.red,file); std::fputc(p.green,file); std::fputc(p.blue,file);
    }}}
    CHECK(std::fclose(file)==0);
}
} // namespace

void fake_enter(portMUX_TYPE* p,bool isr) { CHECK(isr==in_isr && p->depth==0); ++p->depth; }
void fake_exit(portMUX_TYPE* p,bool isr) { CHECK(isr==in_isr && p->depth==1); --p->depth; }
void fake_yield() { CHECK(in_isr); ++yields; }
int xPortGetCoreID() { return core; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return task; }
TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(now_us/1000); }
void vTaskDelay(TickType_t value) { CHECK(!in_isr); ++delays; now_us+=value*1000; }
int64_t esp_timer_get_time() { return now_us; }
uint32_t esp_random() { CHECK(!in_isr); return 123; }
bool esp_ptr_internal(const void*) { return internal_object; }
QueueHandle_t xQueueCreate(unsigned capacity,std::size_t size) {
    CHECK(!in_isr); if (fail_queue) { return nullptr; } ++queues; return new FakeQueue{capacity,size,{}};
}
void vQueueDelete(QueueHandle_t queue) { CHECK(!in_isr); --queues; delete queue; }
BaseType_t xQueueSendFromISR(QueueHandle_t queue,const void* value,BaseType_t* awake) {
    CHECK(in_isr); if (queue->values.size()>=queue->capacity) { return pdFALSE; }
    const auto* first=static_cast<const uint8_t*>(value);
    queue->values.emplace_back(first,first+queue->size); *awake=pdTRUE; return pdTRUE;
}
BaseType_t xQueueSendToFront(QueueHandle_t queue,const void* value,TickType_t) {
    CHECK(!in_isr); if (queue->values.size()>=queue->capacity) { return pdFALSE; }
    const auto* first=static_cast<const uint8_t*>(value);
    queue->values.emplace_front(first,first+queue->size); return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue,void* value,TickType_t wait) {
    CHECK(!in_isr); last_wait=wait;
    if (queue->values.empty()) { if (wait!=portMAX_DELAY) { now_us+=wait*1000; } return pdFALSE; }
    std::memcpy(value,queue->values.front().data(),queue->size); queue->values.pop_front(); return pdTRUE;
}
SemaphoreHandle_t xSemaphoreCreateMutex() { if (fail_mutex) { return nullptr; } ++mutexes; return new FakeMutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t) { CHECK(!in_isr && !m->held); m->held=true; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { CHECK(!in_isr && m->held); m->held=false; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t m) { CHECK(!m->held); --mutexes; delete m; }
void* heap_caps_calloc(std::size_t count,std::size_t size,uint32_t caps) {
    CHECK(!in_isr && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if (allocation_budget==0) { return nullptr; } if (allocation_budget>0) { --allocation_budget; }
    void* p=std::calloc(count,size); CHECK(p); allocated.insert(p); return p;
}
void heap_caps_free(void* p) { CHECK(!in_isr); if (p) { CHECK(allocated.erase(p)==1); std::free(p); } }
esp_err_t esp_event_post_to(esp_event_loop_handle_t loop,esp_event_base_t base,int32_t id,
                            const void* data,std::size_t size,TickType_t wait) {
    CHECK(!in_isr && core==0 && loop && base==TOUCH_SENSOR_EVENT && id==TOUCH_EVENT_TOUCHED && wait==0);
    CHECK(size==sizeof(TouchSensorEvent)); if (post_error!=ESP_OK) { return post_error; }
    touches.push_back(*static_cast<const TouchSensorEvent*>(data)); return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t* c) { CHECK(!in_isr && c->mode==GPIO_MODE_INPUT && c->intr_type==GPIO_INTR_DISABLE); return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { CHECK(flags==ESP_INTR_FLAG_IRAM); if (gpio_service) { return ESP_ERR_INVALID_STATE; } gpio_service=true; return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t,gpio_isr_t fn,void* arg) { CHECK(!gpio_handler); gpio_handler=fn; gpio_arg=arg; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t) { if (remove_error!=ESP_OK) { return consume(remove_error); } gpio_handler=nullptr; return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t,gpio_int_type_t edge) { gpio_edge=edge; return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t) { gpio_enabled=true; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t) { gpio_enabled=false; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t) { CHECK(!gpio_handler); return ESP_OK; }
esp_err_t touch_pad_init() { CHECK(!pad_live); pad_live=true; return ESP_OK; }
esp_err_t touch_pad_deinit() { if (pad_deinit_error!=ESP_OK) { return consume(pad_deinit_error); } CHECK(!fsm && !pad_handler); pad_live=false; return ESP_OK; }
esp_err_t touch_pad_config(touch_pad_t pad) { CHECK(pad==7); return ESP_OK; }
esp_err_t touch_pad_set_thresh(touch_pad_t pad,uint32_t value) { CHECK(pad==7); threshold=value; return ESP_OK; }
esp_err_t touch_pad_filter_set_config(const touch_filter_config_t* f) { CHECK(f->mode==TOUCH_PAD_FILTER_IIR_16); return ESP_OK; }
esp_err_t touch_pad_filter_enable() { return ESP_OK; }
esp_err_t touch_pad_set_fsm_mode(touch_fsm_mode_t) { return ESP_OK; }
esp_err_t touch_pad_fsm_start() { fsm=true; return ESP_OK; }
esp_err_t touch_pad_fsm_stop() { fsm=false; return ESP_OK; }
esp_err_t touch_pad_read_benchmark(touch_pad_t,uint32_t* value) { CHECK(!in_isr && fsm); ++benchmark_reads; *value=benchmark; return ESP_OK; }
esp_err_t touch_pad_timeout_set(bool enabled,uint32_t value) { CHECK(enabled && value==TOUCH_PAD_THRESHOLD_MAX); return ESP_OK; }
esp_err_t touch_pad_isr_register(void(*fn)(void*),void* arg,touch_pad_intr_mask_t) { pad_handler=fn; pad_arg=arg; return ESP_OK; }
esp_err_t touch_pad_isr_deregister(void(*fn)(void*),void* arg) { CHECK(fn==pad_handler && arg==pad_arg); pad_handler=nullptr; return ESP_OK; }
esp_err_t touch_pad_intr_enable(touch_pad_intr_mask_t) { pad_enabled=true; return ESP_OK; }
esp_err_t touch_pad_intr_disable(touch_pad_intr_mask_t) { pad_enabled=false; return ESP_OK; }
esp_err_t touch_pad_intr_clear(touch_pad_intr_mask_t) { return ESP_OK; }
uint32_t touch_pad_read_intr_status_mask() { CHECK(in_isr); return pad_status; }
touch_pad_t touch_pad_get_current_meas_channel() { CHECK(in_isr); return current_pad; }
esp_err_t touch_pad_timeout_resume() { CHECK(!in_isr); ++resumed; return ESP_OK; }
esp_err_t led_strip_new_rmt_device(const led_strip_config_t* cfg,const led_strip_rmt_config_t* rmt,led_strip_handle_t* out) {
    CHECK(!in_isr && core==0); if (strip_new_error!=ESP_OK) { return consume(strip_new_error); }
    last_strip_config=*cfg; last_rmt_config=*rmt; *out=new led_strip_t{std::vector<EyeRgb>(cfg->max_leds)}; ++strips; return ESP_OK;
}
esp_err_t led_strip_set_pixel(led_strip_handle_t strip,uint32_t index,uint32_t r,uint32_t g,uint32_t b) {
    CHECK(!in_isr && index<strip->pixels.size() && r<=255 && g<=255 && b<=255);
    if (strip_pixel_error!=ESP_OK) { return consume(strip_pixel_error); }
    strip->pixels[index]={static_cast<uint8_t>(r),static_cast<uint8_t>(g),static_cast<uint8_t>(b)}; return ESP_OK;
}
esp_err_t led_strip_refresh(led_strip_handle_t strip) {
    CHECK(!in_isr); if (strip_refresh_error!=ESP_OK) { return consume(strip_refresh_error); }
    shown=strip->pixels; ++refreshes; return ESP_OK;
}
esp_err_t led_strip_clear(led_strip_handle_t strip) {
    CHECK(!in_isr); if (strip_clear_error!=ESP_OK) { return consume(strip_clear_error); }
    std::fill(strip->pixels.begin(),strip->pixels.end(),EyeRgb{}); return led_strip_refresh(strip);
}
esp_err_t led_strip_del(led_strip_handle_t strip) {
    CHECK(!in_isr); if (strip_delete_error!=ESP_OK) { return consume(strip_delete_error); }
    --strips; delete strip; return ESP_OK;
}

int main(int argc,char** argv) {
    const struct { const char* name; void(*run)(); } tests[]={
        {"GPIO interrupt defers event dispatch",test_gpio_isr_defers_work},
        {"Python debounce and interrupt timestamps",test_python_debounce_and_timestamp},
        {"blocking idle and cooperative stop",test_blocking_idle_and_stop},
        {"active-low input and shared GPIO service",test_active_low_and_service_reuse},
        {"S3 benchmark delta threshold and interrupt filter",test_internal_pad_threshold_and_interrupt},
        {"touch timeout recovery with a full queue",test_pad_timeout_and_full_queue_recovery},
        {"touch queue and event backpressure",test_touch_backpressure},
        {"touch configuration and cleanup retry",test_touch_init_validation_and_cleanup},
        {"pad cleanup and task ownership",test_pad_cleanup_retry_and_owner},
        {"Python easing and blink math",test_animation_math},
        {"WS2812 GRB and RMT DMA configuration",test_rmt_configuration},
        {"happy, sad, and angry expressions",test_emotion_masks_and_palette},
        {"smooth transition and repeated emotion",test_smooth_transition_and_repeated_state},
        {"brightness scaling and clear/pause",test_brightness_and_clear},
        {"blink timing and static dead eyes",test_blink_and_dead_expression},
        {"tile, column, serpentine, and mirror wiring",test_matrix_wiring},
        {"matrix errors, retries, and Core-0 access",test_matrix_errors_and_core},
    };
    for (const auto& test:tests) { test.run(); std::printf("PASS %s\n",test.name); }
    if (argc>1) { write_preview(argv[1]); }
    CHECK(!queues && !mutexes && !strips && allocated.empty());
    std::printf("All %zu host tests passed.\n",sizeof(tests)/sizeof(tests[0]));
}
