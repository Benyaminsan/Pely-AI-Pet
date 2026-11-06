#include "vision_ai.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#value); std::exit(1); } } while(false)

namespace {
struct ModelSpec {
    std::vector<int> input_shape{1,96,96,3}, output_shape{1,2,2,3};
    int input_exponent=-7, output_exponent=-7;
    bool empty_io=false, per_channel=false, invalid_exponent=false, undersized=false;
    dl::dtype_t input_dtype=dl::DATA_TYPE_INT8;
    std::vector<int8_t> values;
} spec;
int core=1, models=0, runs=0, returned=0, deinitialized=0;
TaskHandle_t task=reinterpret_cast<void*>(1);
int64_t now_us=0;
bool psram=true, camera_active=false, frame_leased=false, no_frame=false;
int fail_allocation_after=-1, orientation_error=0;
esp_err_t camera_error=ESP_OK, event_error=ESP_OK;
std::map<void*,void*> allocations;
std::vector<int8_t> last_input;
std::vector<VisionResult> events;
std::vector<uint8_t> camera_bytes(8);
camera_fb_t camera_frame;
camera_config_t driver_config{};
sensor_t sensor;
const VisionLabel labels[]={{"","Background"},{"apple","Apple"},{"milk","Milk"}};
const uint8_t red[]={0xf8,0x00};

int orientation(sensor_t*,int) { return orientation_error; }
void heatmap(unsigned width=2,unsigned height=2,unsigned background=0) {
    spec.output_shape={1,static_cast<int>(height),static_cast<int>(width),3};
    spec.values.assign(width*height*3,0);
    for (unsigned i=0;i<width*height;++i) { spec.values[i*3+background]=100; }
}
void reset() {
    CHECK(allocations.empty() && models==0 && !camera_active && !frame_leased);
    spec=ModelSpec{}; core=1; task=reinterpret_cast<void*>(1);
    now_us=0; runs=returned=deinitialized=0; psram=true;
    fail_allocation_after=-1; orientation_error=0; no_frame=false;
    camera_error=event_error=ESP_OK; last_input.clear(); events.clear();
    camera_frame={camera_bytes.data(),8,2,2,PIXFORMAT_RGB565};
    sensor.id.PID=OV5640_PID; sensor.set_hmirror=orientation; sensor.set_vflip=orientation;
    heatmap();
}
VisionAiConfig config() { VisionAiConfig result; result.labels=labels; result.label_count=3; return result; }
Rgb565FrameView red_frame() { return {red,sizeof(red),1,1,2,Rgb565ByteOrder::HIGH_BYTE_FIRST}; }
void near(double actual,double expected,double tolerance=0.0001) {
    if (std::fabs(actual-expected)>tolerance) {
        std::fprintf(stderr,"Expected %.9f, got %.9f\n",expected,actual); std::exit(1);
    }
}
void foreground(unsigned x,unsigned y,int8_t score=120,unsigned channel=1) {
    const auto index=(y*spec.output_shape[2]+x)*3;
    for (unsigned c=0;c<3;++c) { spec.values[index+c]=c==channel ? score : 0; }
}
std::vector<uint8_t> solid_frame(unsigned width,unsigned height) {
    std::vector<uint8_t> bytes(width*height*2);
    for (std::size_t i=0;i<bytes.size();i+=2) { bytes[i]=0xf8; }
    return bytes;
}
CameraHalConfig camera_config() {
    CameraHalConfig result;
    result.pins={-1,-1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    return result;
}

void test_rgb565_quantization_and_refill() {
    reset(); VisionAI ai; CHECK(ai.init(config())==ESP_OK);
    VisionResult result;
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK);
    CHECK(result.count==0 && runs==1 && last_input.size()==96*96*3);
    for (unsigned i=0;i<last_input.size();i+=3) {
        CHECK(last_input[i]==127 && last_input[i+1]==0 && last_input[i+2]==0);
    }
    near(result.inference_time_ms,5);
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK);
    CHECK(last_input.back()==0 && last_input.front()==127); // Input was overwritten by fake run.
}

void test_byte_order_bgr_and_normalization() {
    reset();
    auto cfg=config(); cfg.bgr_input=true;
    for (unsigned i=0;i<3;++i) { cfg.input_scale[i]=1.0F/127.5F; cfg.input_offset[i]=-1; }
    VisionAI ai; CHECK(ai.init(cfg)==ESP_OK);
    const uint8_t low_first[]={0,0xf8};
    Rgb565FrameView frame{low_first,2,1,1,2,Rgb565ByteOrder::LOW_BYTE_FIRST};
    VisionResult result; CHECK(ai.infer_rgb565(frame,result)==ESP_OK);
    CHECK(last_input[0]==-128 && last_input[1]==-128 && last_input[2]==127);
}

void test_bilinear_and_stride() {
    reset(); spec.input_shape={1,1,1,3};
    auto cfg=config(); cfg.input_width=cfg.input_height=1;
    // Red, green / blue, white, with two bytes of padding per row.
    const uint8_t pixels[]={0xf8,0,0x07,0xe0,99,99,0,0x1f,0xff,0xff};
    Rgb565FrameView frame{pixels,sizeof(pixels),2,2,6};
    VisionAI ai; CHECK(ai.init(cfg)==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(frame,result)==ESP_OK);
    CHECK(last_input[0]==64 && last_input[1]==64 && last_input[2]==64);
}

void test_frame_validation() {
    reset(); VisionAI ai; CHECK(ai.init(config())==ESP_OK);
    VisionResult result; result.count=8;
    auto frame=red_frame(); frame.size_bytes=1;
    CHECK(ai.infer_rgb565(frame,result)==ESP_ERR_INVALID_SIZE && result.count==0);
    frame=red_frame(); frame.data=nullptr;
    CHECK(ai.infer_rgb565(frame,result)==ESP_ERR_INVALID_ARG);
    frame=red_frame(); frame.height=2; frame.stride_bytes=std::numeric_limits<std::size_t>::max();
    CHECK(ai.infer_rgb565(frame,result)==ESP_ERR_INVALID_SIZE);
    frame=red_frame(); frame.byte_order=static_cast<Rgb565ByteOrder>(99);
    CHECK(ai.infer_rgb565(frame,result)==ESP_ERR_INVALID_ARG);
    CHECK(runs==0);
}

void test_crop_coordinate_mapping() {
    reset(); foreground(0,0);
    auto bytes=solid_frame(200,100);
    Rgb565FrameView frame{bytes.data(),bytes.size(),200,100,400};
    VisionAI ai; CHECK(ai.init(config())==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(frame,result)==ESP_OK && result.count==1);
    CHECK(result.crop_x==50 && result.crop_width==100 && result.crop_height==100);
    const auto& detection=result.detections[0];
    CHECK(detection.x1==50 && detection.x2==100 && detection.y1==0 && detection.y2==50);
    near(detection.center_x,75); near(detection.center_y,25);
    CHECK(std::strcmp(detection.food_id,"apple")==0);
    near(detection.confidence,120.0/128);
}

void test_stretch_and_portrait_mapping() {
    reset(); foreground(1,1);
    auto cfg=config(); cfg.resize_mode=FomoResizeMode::STRETCH;
    auto bytes=solid_frame(100,200);
    Rgb565FrameView frame{bytes.data(),bytes.size(),100,200,200};
    VisionAI ai; CHECK(ai.init(cfg)==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(frame,result)==ESP_OK);
    CHECK(result.crop_width==100 && result.crop_height==200);
    const auto& detection=result.detections[0];
    CHECK(detection.x1==50 && detection.x2==100 && detection.y1==100 && detection.y2==200);
    near(detection.center_x,75); near(detection.center_y,150);
}

void test_connected_components_and_sorting() {
    reset(); heatmap(4,4);
    foreground(0,0,120); foreground(1,1,115); // Diagonal neighbours merge.
    foreground(3,3,100); foreground(1,0,125,2); // Another class never merges.
    VisionAI ai; CHECK(ai.init(config())==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK);
    CHECK(result.count==3 && result.detections[0].class_id==2);
    CHECK(result.detections[1].cell_count==2 && result.detections[2].cell_count==1);
    const auto& merged=result.detections[1];
    near(merged.center_x,(0.5*120+1.5*115)/235/4);
    CHECK(merged.x1==0 && merged.y1==0 && merged.x2==1 && merged.y2==1);
}

void test_threshold_background_and_no_stale_results() {
    reset(); foreground(0,0,89); // 0.695 < 0.70.
    spec.values[3]=120; spec.values[4]=120; // Background wins a tie.
    spec.values[6]=120; spec.values[7]=110; // Foreground loses to background.
    VisionAI ai; CHECK(ai.init(config())==ESP_OK);
    VisionResult result; result.count=8;
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK && result.count==0);
    foreground(1,1,120);
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK && result.count==1);
    heatmap();
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK && result.count==0);
}

void test_logits_and_probability_validation() {
    reset(); heatmap(1,1); spec.values={0,16,8}; spec.output_exponent=-3;
    auto cfg=config(); cfg.output_mode=FomoOutputMode::SOFTMAX_LOGITS; cfg.confidence_threshold=0.6F;
    VisionAI ai; CHECK(ai.init(cfg)==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK && result.count==1);
    near(result.detections[0].confidence,1.0/(std::exp(-2.0)+1+std::exp(-1.0)));
    CHECK(ai.deinit()==ESP_OK);
    cfg.output_mode=FomoOutputMode::PROBABILITIES;
    CHECK(ai.init(cfg)==ESP_OK);
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_ERR_INVALID_RESPONSE && result.count==0);
}

void test_background_channel_and_cluster_filter() {
    reset(); heatmap(2,2,2); foreground(0,0,120,0);
    const VisionLabel other_labels[]={{"apple","Apple"},{"milk","Milk"},{"","Background"}};
    auto cfg=config(); cfg.background_channel=2; cfg.labels=other_labels;
    VisionAI ai; CHECK(ai.init(cfg)==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK && result.count==1);
    CHECK(result.detections[0].class_id==0 && std::strcmp(result.detections[0].food_id,"apple")==0);
    CHECK(ai.deinit()==ESP_OK);
    cfg.minimum_cluster_cells=2; CHECK(ai.init(cfg)==ESP_OK);
    CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK && result.count==0);
}

void test_result_capacity() {
    reset(); heatmap(7,7); int8_t score=96;
    for (unsigned y=0;y<7;y+=2) {
        for (unsigned x=0;x<7;x+=2) { foreground(x,y,score++); }
    }
    VisionAI ai; CHECK(ai.init(config())==ESP_OK);
    VisionResult result; CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK);
    CHECK(result.count==8 && result.truncated);
    near(result.detections[0].confidence,111.0/128);
    near(result.detections[7].confidence,104.0/128);
}

void test_tensor_validation() {
    reset();
    auto rejects=[]() { VisionAI ai; CHECK(ai.init(config())!=ESP_OK); CHECK(models==0 && allocations.empty()); };
    spec.input_shape={1,96,96,1}; rejects();
    spec.input_shape={1,96,96,3}; spec.output_shape={1,2,2,4}; rejects();
    spec.output_shape={1,2,2,3}; spec.input_dtype=dl::DATA_TYPE_FLOAT; rejects();
    spec.input_dtype=dl::DATA_TYPE_INT8; spec.per_channel=true; rejects();
    spec.per_channel=false; spec.input_exponent=40; rejects();
    spec.input_exponent=-7; spec.invalid_exponent=true; rejects();
    spec.invalid_exponent=false; spec.empty_io=true; rejects();
    spec.empty_io=false; spec.undersized=true; rejects();
}

void test_config_psram_and_allocation_errors() {
    reset(); VisionAI ai;
    auto cfg=config(); cfg.labels=nullptr; CHECK(ai.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=config(); cfg.confidence_threshold=std::numeric_limits<float>::quiet_NaN();
    CHECK(ai.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=config(); cfg.input_width=0; CHECK(ai.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=config(); psram=false; CHECK(ai.init(cfg)==ESP_ERR_NO_MEM); psram=true;
    fail_allocation_after=1; CHECK(ai.init(cfg)==ESP_ERR_NO_MEM);
    CHECK(models==0 && allocations.empty());
    fail_allocation_after=-1; CHECK(ai.init(cfg)==ESP_OK);
    CHECK(ai.init(cfg)==ESP_ERR_INVALID_STATE);
}

void test_events_and_owner_task() {
    reset(); foreground(0,0); auto cfg=config(); cfg.event_loop=reinterpret_cast<void*>(2);
    VisionAI ai;
    core=0; CHECK(ai.init(cfg)==ESP_ERR_INVALID_STATE); core=1;
    CHECK(ai.init(cfg)==ESP_OK);
    VisionResult result; esp_err_t posted=ESP_FAIL;
    CHECK(ai.infer_rgb565(red_frame(),result,&posted)==ESP_OK && posted==ESP_OK);
    CHECK(events.size()==1 && events[0].count==1);
    event_error=ESP_ERR_TIMEOUT;
    CHECK(ai.infer_rgb565(red_frame(),result,&posted)==ESP_OK && posted==ESP_ERR_TIMEOUT && result.count==1);
    core=0; CHECK(ai.infer_rgb565(red_frame(),result)==ESP_ERR_INVALID_STATE && result.count==0);
    CHECK(ai.deinit()==ESP_ERR_INVALID_STATE); core=1;
    task=reinterpret_cast<void*>(3); CHECK(ai.infer_rgb565(red_frame(),result)==ESP_ERR_INVALID_STATE);
    task=reinterpret_cast<void*>(1);
    CHECK(ai.deinit()==ESP_OK);
}

void test_camera_initialization() {
    reset(); CameraHal camera;
    CHECK(camera.init(CameraHalConfig{})==ESP_ERR_INVALID_ARG);
    auto cfg=camera_config(); cfg.pins.d1=cfg.pins.d0;
    CHECK(camera.init(cfg)==ESP_ERR_INVALID_ARG);
    cfg=camera_config(); core=0; CHECK(camera.init(cfg)==ESP_ERR_INVALID_STATE); core=1;
    psram=false; CHECK(camera.init(cfg)==ESP_ERR_NO_MEM); psram=true;
    CHECK(camera.init(cfg)==ESP_OK);
    CHECK(driver_config.pixel_format==PIXFORMAT_RGB565 && driver_config.fb_count==1);
    CHECK(driver_config.fb_location==CAMERA_FB_IN_PSRAM && driver_config.pin_d7==12);
    CHECK(camera.init(cfg)==ESP_ERR_INVALID_STATE);
}

void test_camera_sensor_failure_cleanup() {
    reset(); CameraHal camera; const auto cfg=camera_config();
    camera_error=ESP_FAIL; CHECK(camera.init(cfg)==ESP_FAIL && !camera_active);
    camera_error=ESP_OK; sensor.id.PID=0x2640;
    CHECK(camera.init(cfg)==ESP_ERR_NOT_SUPPORTED && !camera_active && deinitialized==1);
    sensor.id.PID=OV5640_PID; orientation_error=1;
    CHECK(camera.init(cfg)==ESP_FAIL && !camera_active && deinitialized==2);
    orientation_error=0; CHECK(camera.init(cfg)==ESP_OK);
}

void test_frame_lease_move_and_release() {
    reset(); CameraHal camera; CHECK(camera.init(camera_config())==ESP_OK);
    {
        CameraHal::Frame frame; CHECK(camera.capture(frame)==ESP_OK);
        CHECK(frame.view().data==camera_frame.buf && frame.view().stride_bytes==4);
        CameraHal::Frame extra;
        CHECK(camera.capture(extra)==ESP_ERR_INVALID_STATE);
        CHECK(camera.deinit()==ESP_ERR_INVALID_STATE);
        CameraHal::Frame moved(std::move(frame));
        CHECK(!frame && moved);
        moved.reset(); moved.reset();
        CHECK(returned==1 && !frame_leased);
        CHECK(camera.capture(extra)==ESP_OK);
    }
    CHECK(returned==2);
    CHECK(camera.deinit()==ESP_OK && deinitialized==1);
}

void test_capture_errors_and_thread_owner() {
    reset(); CameraHal camera; CHECK(camera.init(camera_config())==ESP_OK);
    CameraHal::Frame frame;
    no_frame=true; CHECK(camera.capture(frame)==ESP_ERR_TIMEOUT); no_frame=false;
    camera_frame.format=PIXFORMAT_JPEG; CHECK(camera.capture(frame)==ESP_ERR_INVALID_SIZE && returned==1);
    camera_frame.format=PIXFORMAT_RGB565; camera_frame.len=1;
    CHECK(camera.capture(frame)==ESP_ERR_INVALID_SIZE && returned==2);
    camera_frame.len=8; task=reinterpret_cast<void*>(3);
    CHECK(camera.capture(frame)==ESP_ERR_INVALID_STATE);
    task=reinterpret_cast<void*>(1); CHECK(camera.capture(frame)==ESP_OK); frame.reset();
}
} // namespace

int xPortGetCoreID() { return core; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return task; }
int64_t esp_timer_get_time() { return now_us; }
std::size_t heap_caps_get_total_size(uint32_t caps) { CHECK(caps==MALLOC_CAP_SPIRAM); return psram ? 8*1024*1024 : 0; }
void* heap_caps_aligned_alloc(std::size_t alignment,std::size_t size,uint32_t caps) {
    CHECK(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (fail_allocation_after==0) { return nullptr; }
    if (fail_allocation_after>0) { --fail_allocation_after; }
    void* base=std::malloc(size+alignment);
    CHECK(base);
    auto* aligned=reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(base)+alignment-1)&~(alignment-1));
    allocations[aligned]=base; return aligned;
}
void* heap_caps_malloc(std::size_t size,uint32_t caps) { return heap_caps_aligned_alloc(16,size,caps); }
void heap_caps_free(void* pointer) {
    if (!pointer) { return; }
    CHECK(allocations.count(pointer)==1); std::free(allocations.at(pointer)); allocations.erase(pointer);
}
esp_err_t esp_event_post_to(esp_event_loop_handle_t loop,esp_event_base_t base,int32_t id,
                            const void* data,std::size_t size,TickType_t wait) {
    CHECK(loop && base==VISION_AI_EVENT && id==VISION_EVENT_RESULT && size==sizeof(VisionResult) && wait==0);
    if (event_error!=ESP_OK) { return event_error; }
    events.push_back(*static_cast<const VisionResult*>(data)); return ESP_OK;
}
esp_err_t esp_camera_init(const camera_config_t* cfg) {
    CHECK(!camera_active);
    if (camera_error!=ESP_OK) { return camera_error; }
    driver_config=*cfg; camera_active=true; return ESP_OK;
}
esp_err_t esp_camera_deinit() {
    CHECK(camera_active && !frame_leased); camera_active=false; ++deinitialized; return ESP_OK;
}
sensor_t* esp_camera_sensor_get() { return &sensor; }
camera_fb_t* esp_camera_fb_get() {
    CHECK(camera_active && !frame_leased);
    if (no_frame) { return nullptr; }
    frame_leased=true; return &camera_frame;
}
void esp_camera_fb_return(camera_fb_t* frame) { CHECK(frame==&camera_frame && frame_leased); frame_leased=false; ++returned; }

namespace dl {
Model::Model(const char* data,fbs::model_location_type_t location,int internal,memory_manager_t mm,
              const uint8_t* key,bool param_copy) {
    CHECK(reinterpret_cast<uintptr_t>(data)%16==0 && location==fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    CHECK(internal==0 && mm==MEMORY_MANAGER_GREEDY && !key && param_copy); ++models;
    input.shape=spec.input_shape; input.dtype=spec.input_dtype;
    input.exponent.value=spec.input_exponent; input.exponent.per_channel=spec.per_channel;
    input.exponent.valid=!spec.invalid_exponent;
    output.shape=spec.output_shape; output.exponent.value=spec.output_exponent;
    auto size=[](const std::vector<int>& shape) { std::size_t n=1; for (int value:shape) { n*=std::max(1,value); } return n; };
    input.storage.resize(spec.undersized ? 1 : size(input.shape)); output.storage.resize(size(output.shape));
    if (!spec.empty_io) { inputs["input"]=&input; outputs["output"]=&output; }
}
Model::~Model() { --models; }
void Model::run(runtime_mode_t mode) {
    CHECK(mode==RUNTIME_MODE_SINGLE_CORE && core==1); ++runs;
    last_input=input.storage;
    CHECK(output.storage.size()==spec.values.size()); output.storage=spec.values;
    std::fill(input.storage.begin(),input.storage.end(),-42); // Model arenas may overwrite input.
    now_us+=5000;
}
} // namespace dl

int main(int argc,char** argv) {
    if (argc > 1) {
        reset();
        VisionAI ai;
        if (std::strcmp(argv[1],"format")==0) {
            CHECK(ai.init(config())==ESP_ERR_NOT_SUPPORTED);
        } else if (std::strcmp(argv[1],"size")==0) {
            CHECK(ai.init(config())==ESP_ERR_INVALID_SIZE);
        } else {
            CHECK(ai.init(config())==ESP_OK && allocations.size()==4);
            VisionResult result; CHECK(ai.infer_rgb565(red_frame(),result)==ESP_OK);
            CHECK(ai.deinit()==ESP_OK && allocations.empty());
        }
        CHECK(models==0 && allocations.empty());
        std::printf("PASS model container/alignment: %s\n",argv[1]);
        return 0;
    }
    const struct { const char* name; void (*run)(); } tests[]={
        {"RGB565 to 96x96 quantization and arena refill",test_rgb565_quantization_and_refill},
        {"byte order, BGR, and signed normalization",test_byte_order_bgr_and_normalization},
        {"bilinear resize and padded rows",test_bilinear_and_stride},
        {"invalid and truncated RGB565 frames",test_frame_validation},
        {"center-crop coordinates in original frame",test_crop_coordinate_mapping},
        {"stretch resize and portrait coordinates",test_stretch_and_portrait_mapping},
        {"FOMO same-class components and sorting",test_connected_components_and_sorting},
        {"confidence, background ties, and empty-result reset",test_threshold_background_and_no_stale_results},
        {"softmax logits and probability contract",test_logits_and_probability_validation},
        {"background index and minimum cluster size",test_background_channel_and_cluster_filter},
        {"result capacity retains strongest clusters",test_result_capacity},
        {"tensor shape, type, scale, and load rejection",test_tensor_validation},
        {"configuration, PSRAM, and allocation cleanup",test_config_psram_and_allocation_errors},
        {"event backpressure and Core-1 task ownership",test_events_and_owner_task},
        {"OV5640 pins and RGB565 PSRAM configuration",test_camera_initialization},
        {"sensor identity and initialization cleanup",test_camera_sensor_failure_cleanup},
        {"camera frame ownership, moves, and return",test_frame_lease_move_and_release},
        {"capture errors and camera task ownership",test_capture_errors_and_thread_owner},
    };
    for (const auto& test:tests) { test.run(); std::printf("PASS %s\n",test.name); }
    CHECK(allocations.empty() && models==0 && !camera_active && !frame_leased);
    std::printf("All %zu host tests passed.\n",sizeof(tests)/sizeof(tests[0]));
}
