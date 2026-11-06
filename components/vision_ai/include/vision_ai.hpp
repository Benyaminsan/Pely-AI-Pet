#pragma once

#include <cstddef>
#include <cstdint>

#include "camera_hal.hpp"
#include "esp_err.h"
#include "esp_event.h"

namespace dl { class Model; class TensorBase; }

ESP_EVENT_DECLARE_BASE(VISION_AI_EVENT);
enum VisionAiEventId : int32_t { VISION_EVENT_RESULT = 1 };
enum class FomoResizeMode : uint8_t { CENTER_CROP, STRETCH };
enum class FomoOutputMode : uint8_t { PROBABILITIES, SOFTMAX_LOGITS };

// Include the background channel at its actual exported index (normally 0).
// The array and strings must outlive VisionAI. Never infer class order from a JSON catalog.
struct VisionLabel { const char* food_id; const char* food_name; };

struct VisionAiConfig {
    const VisionLabel* labels = nullptr;
    std::size_t label_count = 0;
    uint16_t background_channel = 0;
    uint16_t input_width = 96;
    uint16_t input_height = 96;
    float confidence_threshold = 0.70F;
    FomoResizeMode resize_mode = FomoResizeMode::CENTER_CROP;
    FomoOutputMode output_mode = FomoOutputMode::PROBABILITIES;
    // Real model input = pixel * scale + offset; match preprocessing used to export.
    float input_scale[3] = {1.0F / 255.0F, 1.0F / 255.0F, 1.0F / 255.0F};
    float input_offset[3] = {0.0F, 0.0F, 0.0F};
    bool bgr_input = false;
    uint16_t minimum_cluster_cells = 1;
    // Optional caller-owned Core-0 event loop. Null enables return-value-only use.
    esp_event_loop_handle_t event_loop = nullptr;
};

struct FoodDetection {
    uint16_t class_id = 0; // Model output channel, including background in indexing.
    char food_id[32] = {};
    char food_name[48] = {};
    float confidence = 0.0F;
    // Coarse active-cell region in source-frame pixels, x2/y2 exclusive.
    // FOMO predicts centroids, not an object's exact bounding box.
    uint16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float center_x = 0.0F, center_y = 0.0F;
    uint16_t cell_count = 0;
};

struct VisionResult {
    static constexpr std::size_t kMaxDetections = 8;
    FoodDetection detections[kMaxDetections] = {};
    uint16_t count = 0;            // Sorted by confidence, strongest first.
    bool truncated = false;       // More valid clusters than output capacity.
    uint16_t frame_width = 0, frame_height = 0;
    uint16_t crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;
    float inference_time_ms = 0.0F; // Preprocess + model + postprocess.
};

/** Single-owner Core-1 inference. The caller owns task creation and scheduling. */
class VisionAI final {
public:
    VisionAI() = default;
    ~VisionAI();
    VisionAI(const VisionAI&) = delete;
    VisionAI& operator=(const VisionAI&) = delete;
    VisionAI(VisionAI&&) = delete;
    VisionAI& operator=(VisionAI&&) = delete;

    esp_err_t init(const VisionAiConfig& config);
    // Keeps the camera frame borrowed only for this call; never retains its pixels.
    // Returns all clusters (up to capacity); ESP_OK with count=0 means no detection.
    // Event posting errors are returned separately; a valid result is still usable.
    esp_err_t infer_rgb565(const Rgb565FrameView& frame, VisionResult& result,
                           esp_err_t* event_error = nullptr);
    esp_err_t deinit();
private:
    bool on_owner_task() const;
    void release();
    esp_err_t validate_tensors();
    esp_err_t preprocess(const Rgb565FrameView& frame, VisionResult& result);
    esp_err_t postprocess(VisionResult& result);
    VisionAiConfig config_;
    TaskHandle_t owner_task_ = nullptr;
    dl::Model* model_ = nullptr;
    dl::TensorBase* input_ = nullptr;
    dl::TensorBase* output_ = nullptr;
    void* aligned_model_copy_ = nullptr;
    int16_t* cell_class_ = nullptr;
    float* cell_confidence_ = nullptr;
    uint32_t* queue_ = nullptr;
    std::size_t grid_width_ = 0, grid_height_ = 0, grid_cells_ = 0;
    float inverse_input_quant_scale_ = 0.0F;
    float output_quant_scale_ = 0.0F;
};
