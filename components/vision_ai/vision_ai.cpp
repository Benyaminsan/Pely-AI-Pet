#include "vision_ai.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#if __has_include("fomo_model.h")
#include "fomo_model.h"
#else
#error "Supply assets/model/fomo_model.h with a genuine ESP-DL fomo_model_int8[] export before building."
#endif

ESP_EVENT_DEFINE_BASE(VISION_AI_EVENT);
static_assert(std::is_trivially_copyable<VisionResult>::value, "Event payload must be copyable");

namespace {
constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

uint32_t little_u32(const unsigned char* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

esp_err_t validate_model_blob(const unsigned char* data, std::size_t size) {
    // Accept a single, unencrypted .espdl container. TFLite/Edge Impulse arrays
    // and raw tensor weights cannot be passed to dl::Model as an ESP-DL model.
    if (size < 12) { return ESP_ERR_INVALID_SIZE; }
    const bool edl1 = std::memcmp(data, "EDL1", 4) == 0;
    const bool edl2 = std::memcmp(data, "EDL2", 4) == 0;
    if (!edl1 && !edl2) { return ESP_ERR_NOT_SUPPORTED; }
    const std::size_t header_size = edl1 ? 12 : 16;
    if (size < header_size || little_u32(data + 8) < 8 ||
        little_u32(data + 8) > size - header_size) { return ESP_ERR_INVALID_SIZE; }
    if (little_u32(data + 4) != 0) { return ESP_ERR_NOT_SUPPORTED; }
    return ESP_OK;
}

bool valid_config(const VisionAiConfig& config) {
    if (!config.labels || config.label_count < 2 || config.label_count > 64 ||
        config.background_channel >= config.label_count || !config.input_width || !config.input_height ||
        config.input_width > 512 || config.input_height > 512 || !config.minimum_cluster_cells ||
        !std::isfinite(config.confidence_threshold) || config.confidence_threshold <= 0.0F ||
        config.confidence_threshold > 1.0F ||
        (config.resize_mode != FomoResizeMode::CENTER_CROP && config.resize_mode != FomoResizeMode::STRETCH) ||
        (config.output_mode != FomoOutputMode::PROBABILITIES &&
         config.output_mode != FomoOutputMode::SOFTMAX_LOGITS)) { return false; }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(config.input_scale[i]) || !std::isfinite(config.input_offset[i])) { return false; }
    }
    for (std::size_t i = 0; i < config.label_count; ++i) {
        const auto& label = config.labels[i];
        if (!label.food_id || !label.food_name || std::strlen(label.food_id) >= 32 ||
            std::strlen(label.food_name) >= 48) { return false; }
        if (i == config.background_channel) { continue; }
        if (!label.food_id[0] || !label.food_name[0]) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (j != config.background_channel && std::strcmp(label.food_id, config.labels[j].food_id) == 0) {
                return false;
            }
        }
    }
    return true;
}

struct Rgb { float channel[3]; };
Rgb read_rgb(const Rgb565FrameView& frame, std::size_t stride, unsigned x, unsigned y) {
    const uint8_t* bytes = frame.data + y * stride + x * 2;
    const uint16_t pixel = frame.byte_order == Rgb565ByteOrder::HIGH_BYTE_FIRST ?
        static_cast<uint16_t>((bytes[0] << 8) | bytes[1]) :
        static_cast<uint16_t>((bytes[1] << 8) | bytes[0]);
    const unsigned r = (pixel >> 11) & 31;
    const unsigned g = (pixel >> 5) & 63;
    const unsigned b = pixel & 31;
    return {{static_cast<float>((r << 3) | (r >> 2)),
             static_cast<float>((g << 2) | (g >> 4)),
             static_cast<float>((b << 3) | (b >> 2))}};
}
} // namespace

bool VisionAI::on_owner_task() const {
    return xPortGetCoreID() == 1 && owner_task_ == xTaskGetCurrentTaskHandle();
}

VisionAI::~VisionAI() {
    if (model_) { configASSERT(on_owner_task()); }
    release();
}

void VisionAI::release() {
    delete model_;
    model_ = nullptr;
    input_ = output_ = nullptr;
    heap_caps_free(cell_class_); cell_class_ = nullptr;
    heap_caps_free(cell_confidence_); cell_confidence_ = nullptr;
    heap_caps_free(queue_); queue_ = nullptr;
    heap_caps_free(aligned_model_copy_); aligned_model_copy_ = nullptr;
    grid_width_ = grid_height_ = grid_cells_ = 0;
    owner_task_ = nullptr;
}

esp_err_t VisionAI::init(const VisionAiConfig& config) {
    if (model_ || xPortGetCoreID() != 1) { return ESP_ERR_INVALID_STATE; }
    if (!valid_config(config)) { return ESP_ERR_INVALID_ARG; }
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { return ESP_ERR_NO_MEM; }
    const unsigned char* bytes = fomo_model_int8;
    const std::size_t byte_count = sizeof(fomo_model_int8);
    esp_err_t error = validate_model_blob(bytes, byte_count);
    if (error != ESP_OK) { return error; }
    // A plain unsigned-char C-array does not guarantee FlatBuffers' alignment.
    if (reinterpret_cast<uintptr_t>(bytes) % 16 != 0) {
        aligned_model_copy_ = heap_caps_aligned_alloc(16, byte_count, kPsramCaps);
        if (!aligned_model_copy_) { return ESP_ERR_NO_MEM; }
        std::memcpy(aligned_model_copy_, bytes, byte_count);
        bytes = static_cast<const unsigned char*>(aligned_model_copy_);
    }
    config_ = config;
    owner_task_ = xTaskGetCurrentTaskHandle();
    // max_internal_size=0 requests a PSRAM arena. ESP-DL manages parameter-copy allocation.
    // Use the loading constructor: this ESP-DL version's default ctor does not
    // initialize the ModelContext needed by load(). Failed loads leave empty I/O maps.
    model_ = new (std::nothrow) dl::Model(reinterpret_cast<const char*>(bytes),
        fbs::MODEL_LOCATION_IN_FLASH_RODATA, 0, dl::MEMORY_MANAGER_GREEDY, nullptr, true);
    if (!model_) { release(); return ESP_ERR_NO_MEM; }
    error = validate_tensors();
    if (error != ESP_OK) { release(); return error; }
    cell_class_ = static_cast<int16_t*>(heap_caps_malloc(grid_cells_ * sizeof(int16_t), kPsramCaps));
    cell_confidence_ = static_cast<float*>(heap_caps_malloc(grid_cells_ * sizeof(float), kPsramCaps));
    queue_ = static_cast<uint32_t*>(heap_caps_malloc(grid_cells_ * sizeof(uint32_t), kPsramCaps));
    if (!cell_class_ || !cell_confidence_ || !queue_) { release(); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t VisionAI::validate_tensors() {
    auto& inputs = model_->get_inputs();
    auto& outputs = model_->get_outputs();
    if (inputs.size() != 1 || outputs.size() != 1) { return ESP_ERR_NOT_SUPPORTED; }
    input_ = inputs.begin()->second;
    output_ = outputs.begin()->second;
    if (!input_ || !output_ || input_->dtype != dl::DATA_TYPE_INT8 || output_->dtype != dl::DATA_TYPE_INT8 ||
        input_->shape.size() != 4 || output_->shape.size() != 4 ||
        input_->shape[0] != 1 || input_->shape[1] != config_.input_height ||
        input_->shape[2] != config_.input_width || input_->shape[3] != 3 ||
        output_->shape[0] != 1 || output_->shape[1] <= 0 || output_->shape[2] <= 0 ||
        output_->shape[1] > 128 || output_->shape[2] > 128 ||
        output_->shape[3] != static_cast<int>(config_.label_count) ||
        !input_->get_element_ptr() || !output_->get_element_ptr() ||
        !input_->exponent.is_valid() || !output_->exponent.is_valid() ||
        input_->exponent.is_per_channel() || output_->exponent.is_per_channel()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const int input_exponent = input_->exponent;
    const int output_exponent = output_->exponent;
    if (input_exponent < -30 || input_exponent > 30 || output_exponent < -30 || output_exponent > 30) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    grid_height_ = static_cast<std::size_t>(output_->shape[1]);
    grid_width_ = static_cast<std::size_t>(output_->shape[2]);
    grid_cells_ = grid_width_ * grid_height_;
    if (config_.minimum_cluster_cells > grid_cells_ ||
        input_->get_size() < config_.input_width * config_.input_height * 3 ||
        output_->get_size() < static_cast<int>(grid_cells_ * config_.label_count)) {
        return ESP_ERR_INVALID_SIZE;
    }
    inverse_input_quant_scale_ = std::ldexp(1.0F, -input_exponent);
    output_quant_scale_ = std::ldexp(1.0F, output_exponent);
    return ESP_OK;
}

esp_err_t VisionAI::preprocess(const Rgb565FrameView& frame, VisionResult& result) {
    if (!frame.data || !frame.width || !frame.height || frame.width > 4096 || frame.height > 4096 ||
        (frame.byte_order != Rgb565ByteOrder::HIGH_BYTE_FIRST &&
         frame.byte_order != Rgb565ByteOrder::LOW_BYTE_FIRST)) { return ESP_ERR_INVALID_ARG; }
    const std::size_t stride = frame.stride_bytes ? frame.stride_bytes : frame.width * 2;
    if (stride < frame.width * 2U || stride > std::numeric_limits<std::size_t>::max() / frame.height ||
        frame.size_bytes < stride * (frame.height - 1) + frame.width * 2U) { return ESP_ERR_INVALID_SIZE; }
    result.frame_width = frame.width; result.frame_height = frame.height;
    result.crop_width = frame.width; result.crop_height = frame.height;
    if (config_.resize_mode == FomoResizeMode::CENTER_CROP) {
        if (frame.width * config_.input_height > frame.height * config_.input_width) {
            result.crop_width = static_cast<uint16_t>(std::max(1, frame.height * config_.input_width / config_.input_height));
        } else {
            result.crop_height = static_cast<uint16_t>(std::max(1, frame.width * config_.input_height / config_.input_width));
        }
        result.crop_x = (frame.width - result.crop_width) / 2;
        result.crop_y = (frame.height - result.crop_height) / 2;
    }
    int8_t* destination = input_->get_element_ptr<int8_t>();
    for (unsigned y = 0; y < config_.input_height; ++y) {
        const float sy = std::clamp(result.crop_y + (y + 0.5F) * result.crop_height / config_.input_height - 0.5F,
            static_cast<float>(result.crop_y), static_cast<float>(result.crop_y + result.crop_height - 1));
        const unsigned y0 = static_cast<unsigned>(sy);
        const unsigned y1 = std::min<unsigned>(y0 + 1, result.crop_y + result.crop_height - 1);
        const float fy = sy - y0;
        for (unsigned x = 0; x < config_.input_width; ++x) {
            const float sx = std::clamp(result.crop_x + (x + 0.5F) * result.crop_width / config_.input_width - 0.5F,
                static_cast<float>(result.crop_x), static_cast<float>(result.crop_x + result.crop_width - 1));
            const unsigned x0 = static_cast<unsigned>(sx);
            const unsigned x1 = std::min<unsigned>(x0 + 1, result.crop_x + result.crop_width - 1);
            const float fx = sx - x0;
            const Rgb p00 = read_rgb(frame, stride, x0, y0), p10 = read_rgb(frame, stride, x1, y0);
            const Rgb p01 = read_rgb(frame, stride, x0, y1), p11 = read_rgb(frame, stride, x1, y1);
            for (unsigned channel = 0; channel < 3; ++channel) {
                const unsigned source_channel = config_.bgr_input ? 2 - channel : channel;
                const float top = p00.channel[source_channel] * (1 - fx) + p10.channel[source_channel] * fx;
                const float bottom = p01.channel[source_channel] * (1 - fx) + p11.channel[source_channel] * fx;
                const double pixel = top * (1 - fy) + bottom * fy;
                const double real = pixel * config_.input_scale[channel] + config_.input_offset[channel];
                const double quantized = std::round(real * inverse_input_quant_scale_);
                *destination++ = static_cast<int8_t>(std::clamp(quantized, -128.0, 127.0));
            }
        }
    }
    return ESP_OK;
}

esp_err_t VisionAI::postprocess(VisionResult& result) {
    const int8_t* output = output_->get_element_ptr<int8_t>();
    for (std::size_t cell = 0; cell < grid_cells_; ++cell) {
        const int8_t* channels = output + cell * config_.label_count;
        std::size_t winner = config_.background_channel;
        float best = channels[winner] * output_quant_scale_;
        for (std::size_t channel = 0; channel < config_.label_count; ++channel) {
            const float value = channels[channel] * output_quant_scale_;
            if (config_.output_mode == FomoOutputMode::PROBABILITIES && (value < 0.0F || value > 1.000001F)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (value > best) { best = value; winner = channel; }
        }
        float confidence = best;
        if (config_.output_mode == FomoOutputMode::SOFTMAX_LOGITS) {
            float denominator = 0;
            for (std::size_t channel = 0; channel < config_.label_count; ++channel) {
                denominator += std::exp(channels[channel] * output_quant_scale_ - best);
            }
            confidence = 1.0F / denominator;
        }
        cell_class_[cell] = winner != config_.background_channel && confidence >= config_.confidence_threshold ?
            static_cast<int16_t>(winner) : -1;
        cell_confidence_[cell] = confidence;
    }

    // Eight-neighbour connected components, restricted to the same winning class.
    for (std::size_t seed = 0; seed < grid_cells_; ++seed) {
        if (cell_class_[seed] < 0) { continue; }
        const auto class_id = static_cast<uint16_t>(cell_class_[seed]);
        std::size_t head = 0, tail = 1;
        queue_[0] = static_cast<uint32_t>(seed);
        cell_class_[seed] = -1;
        unsigned min_x = static_cast<unsigned>(grid_width_), min_y = static_cast<unsigned>(grid_height_);
        unsigned max_x = 0, max_y = 0;
        double sum_x = 0, sum_y = 0, sum_weight = 0;
        float confidence = 0;
        while (head < tail) {
            const uint32_t cell = queue_[head++];
            const unsigned x = cell % grid_width_, y = cell / grid_width_;
            const float weight = cell_confidence_[cell];
            min_x = std::min(min_x, x); min_y = std::min(min_y, y);
            max_x = std::max(max_x, x); max_y = std::max(max_y, y);
            sum_x += (x + 0.5) * weight; sum_y += (y + 0.5) * weight; sum_weight += weight;
            confidence = std::max(confidence, weight);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = static_cast<int>(x) + dx, ny = static_cast<int>(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= static_cast<int>(grid_width_) || ny >= static_cast<int>(grid_height_)) {
                        continue;
                    }
                    const std::size_t neighbour = ny * grid_width_ + nx;
                    if (cell_class_[neighbour] == class_id) {
                        cell_class_[neighbour] = -1; // Mark on enqueue to bound queue size.
                        queue_[tail++] = static_cast<uint32_t>(neighbour);
                    }
                }
            }
        }
        if (tail < config_.minimum_cluster_cells) { continue; }
        FoodDetection detection{};
        detection.class_id = class_id;
        std::snprintf(detection.food_id, sizeof(detection.food_id), "%s", config_.labels[class_id].food_id);
        std::snprintf(detection.food_name, sizeof(detection.food_name), "%s", config_.labels[class_id].food_name);
        detection.confidence = confidence;
        detection.cell_count = static_cast<uint16_t>(tail);
        detection.x1 = static_cast<uint16_t>(result.crop_x + min_x * result.crop_width / grid_width_);
        detection.y1 = static_cast<uint16_t>(result.crop_y + min_y * result.crop_height / grid_height_);
        detection.x2 = static_cast<uint16_t>(result.crop_x +
            ((max_x + 1) * result.crop_width + grid_width_ - 1) / grid_width_);
        detection.y2 = static_cast<uint16_t>(result.crop_y +
            ((max_y + 1) * result.crop_height + grid_height_ - 1) / grid_height_);
        detection.center_x = static_cast<float>(result.crop_x + sum_x / sum_weight * result.crop_width / grid_width_);
        detection.center_y = static_cast<float>(result.crop_y + sum_y / sum_weight * result.crop_height / grid_height_);
        std::size_t insertion;
        if (result.count < VisionResult::kMaxDetections) { insertion = result.count++; }
        else {
            result.truncated = true;
            insertion = VisionResult::kMaxDetections - 1;
            if (detection.confidence <= result.detections[insertion].confidence) { continue; }
        }
        result.detections[insertion] = detection;
        while (insertion > 0 && result.detections[insertion].confidence > result.detections[insertion - 1].confidence) {
            std::swap(result.detections[insertion], result.detections[insertion - 1]);
            --insertion;
        }
    }
    return ESP_OK;
}

esp_err_t VisionAI::infer_rgb565(const Rgb565FrameView& frame, VisionResult& result, esp_err_t* event_error) {
    result = VisionResult{};
    if (event_error) { *event_error = ESP_OK; }
    if (!model_ || !on_owner_task()) { return ESP_ERR_INVALID_STATE; }
    const int64_t start = esp_timer_get_time();
    esp_err_t error = preprocess(frame, result);
    if (error != ESP_OK) { return error; }
    // Never request DUAL_CORE/AUTO: those modes can run kernels on UI/logic Core 0.
    model_->run(dl::RUNTIME_MODE_SINGLE_CORE);
    error = postprocess(result);
    if (error != ESP_OK) { result = VisionResult{}; return error; }
    result.inference_time_ms = static_cast<float>(esp_timer_get_time() - start) / 1000.0F;
    if (config_.event_loop) {
        const esp_err_t posted = esp_event_post_to(config_.event_loop, VISION_AI_EVENT,
            VISION_EVENT_RESULT, &result, sizeof(result), 0);
        if (event_error) { *event_error = posted; }
    }
    return ESP_OK;
}

esp_err_t VisionAI::deinit() {
    if (!model_ || !on_owner_task()) { return ESP_ERR_INVALID_STATE; }
    release();
    return ESP_OK;
}
