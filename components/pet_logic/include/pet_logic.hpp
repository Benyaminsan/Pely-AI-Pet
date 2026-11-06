#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

ESP_EVENT_DECLARE_BASE(PET_LOGIC_EVENT);

enum class PetMood : uint8_t { HUNGRY, HAPPY, SAD, LONELY, SLEEPY, DEAD };
enum class PetDesire : uint8_t { FOOD, AFFECTION, PLAY, SLEEP, NONE };
enum class PetEmotion : uint8_t {
    NEUTRAL, HAPPY, EXCITED, CONFUSED, HUNGRY, LONELY, SLEEPY, SAD
};
enum class BehaviorOutcome : uint8_t {
    CORRECT, WRONG, IGNORED_COOLDOWN, IGNORED_NO_FOOD, IGNORED_DEAD
};

enum PetLogicEventId : int32_t {
    PET_EVENT_STATE_CHANGED = 1,
    PET_EVENT_FOOD_EVALUATED,
    PET_EVENT_SPEECH_REQUESTED,
    PET_EVENT_DIED,
    PET_EVENT_MEMORY_ERASED,
};

// The array, strings, and aliases must outlive PetLogic. IDs must match vision labels.
struct PetFood {
    const char* id;
    const char* name;
    const char* const* aliases = nullptr;
    std::size_t alias_count = 0;
};

struct PetLogicConfig {
    PetMood initial_mood = PetMood::HUNGRY;
    float initial_hunger = 72.0F;       // 0 = full, 100 = starving (Python semantics).
    float initial_happiness = 65.0F;
    float initial_energy = 80.0F;
    float initial_health = 100.0F;
    float response_cooldown_seconds = 3.0F;
    float hunger_per_second = 0.15F;
    float energy_loss_per_second = 0.05F;
    float affection_grace_seconds = 20.0F;
    float happiness_loss_per_second = 0.25F;
    float minimum_passive_happiness = 25.0F;

    // New gameplay rules: configurable because the Python prototype has no health.
    float starvation_hunger = 90.0F;
    float starvation_health_loss_per_second = 0.20F;
    float exhaustion_health_loss_per_second = 0.10F; // Only when energy reaches 0.
    float feeding_health_boost = 5.0F;
    uint32_t autosave_interval_seconds = 60;
};

struct PetSnapshot {
    PetMood mood = PetMood::HUNGRY;
    PetDesire desire = PetDesire::FOOD;
    bool alive = true;
    float hunger = 72.0F;
    float happiness = 65.0F;
    float energy = 80.0F;
    float health = 100.0F;
    double care_age_seconds = 0.0;      // Powered-on lifetime; frozen after death.
    double time_since_interaction_seconds = 0.0;
    double time_since_affection_seconds = 0.0;
    uint32_t successful_feeds = 0;
    uint32_t wrong_feeds = 0;
    uint32_t affection_count = 0;
    char requested_food_id[32] = {};
    char requested_food_name[48] = {};
};

// esp_event copies this POD payload. Handlers must copy it if retaining it.
struct PetLogicEventData {
    PetSnapshot state;
    BehaviorOutcome outcome = BehaviorOutcome::IGNORED_NO_FOOD;
    PetEmotion emotion = PetEmotion::NEUTRAL;
    uint8_t priority = 0;
    char detected_food[48] = {};
    char requested_food[32] = {};       // Request BEFORE a successful feed rotates it.
    char speech[192] = {};
};

/**
 * Call init and all mutating methods from tasks pinned to Core 0, never an ISR.
 * A mutex serializes Core-0 callers; get_state may be called from either core.
 * The caller owns the periodic FreeRTOS task and event loop (see README).
 * Stop/join all callers before destruction. Only one instance may own the NVS namespace.
 * The small state stays in normal RAM; camera/model PSRAM belongs to other components.
 */
class PetLogic final {
public:
    PetLogic();
    explicit PetLogic(const PetLogicConfig& config);
    ~PetLogic();
    PetLogic(const PetLogic&) = delete;
    PetLogic& operator=(const PetLogic&) = delete;
    PetLogic(PetLogic&&) = delete;
    PetLogic& operator=(PetLogic&&) = delete;

    // Application must initialize NVS and create a Core-0 event loop first.
    // No automatic partition erase on initialization/read errors.
    esp_err_t init(esp_event_loop_handle_t event_loop,
                   const PetFood* foods = nullptr, std::size_t food_count = 0);
    esp_err_t update_tick();           // Uses actual monotonic elapsed time, not tick count.
    esp_err_t get_state(PetSnapshot& result) const;

    // Direct state actions mirror PetState. feed_wrong defaults to Python's 5 points.
    esp_err_t feed_success(float hunger_delta = 30.0F, float happiness_delta = 20.0F);
    esp_err_t feed_wrong(float happiness_delta = 5.0F);
    esp_err_t give_affection(float happiness_boost = 20.0F);
    esp_err_t rest(float energy_boost = 20.0F);

    // Behavior path: matching, 3-second cooldown, wrong-food penalty of 3, speech,
    // request rotation, and a new-food announcement after 4 seconds.
    esp_err_t handle_confirmed_food(const char* detected_food, BehaviorOutcome& outcome);
    esp_err_t announce_startup();
    esp_err_t schedule_initial_desire_announcement(float delay_seconds = 3.5F);

    // Explicit checkpoint before a planned restart/deep sleep; never called per frame.
    esp_err_t save_memory();
    // Erase/commit ONLY the pet_logic namespace. Default: create a fresh pet in RAM.
    // false is allowed only while dead: keep the dead display and prevent future
    // checkpoints from recreating its erased save (CloudSync death-reset path).
    esp_err_t erase_memory(bool restart_pet = true);

private:
    esp_err_t check_mutation_context() const;
    bool valid_config() const;
    void reset_state_locked(int64_t now);
    void advance_locked(int64_t now);
    void update_desire_locked();
    void select_food_locked(bool exclude_current);
    bool matches_food_locked(const char* detected) const;
    void feed_success_locked(float hunger_delta, float happiness_delta);
    void feed_wrong_locked(float happiness_delta);
    esp_err_t load_locked();
    esp_err_t save_locked(int64_t now);
    esp_err_t finish_locked(int64_t now, esp_err_t action_error = ESP_OK, bool force_save = false);
    esp_err_t publish_locked(PetLogicEventId id, const PetLogicEventData* event = nullptr);
    esp_err_t speak_locked(const char* const* phrases, std::size_t count,
                           PetEmotion emotion, uint8_t priority, int64_t now,
                           const char* food_name = nullptr);
    esp_err_t vocalize_locked(int64_t now);
    bool can_respond_locked(int64_t now) const;
    void mark_speech_locked(int64_t now, uint32_t maximum_interval_seconds = 24);

    PetLogicConfig config_;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    nvs_handle_t nvs_ = 0;
    esp_event_loop_handle_t event_loop_ = nullptr; // Borrowed, never deleted here.
    const PetFood* foods_ = nullptr;
    std::size_t food_count_ = 0;
    std::size_t food_index_ = 0;
    PetSnapshot state_;
    PetMood previous_mood_ = PetMood::HUNGRY;
    bool initialized_ = false;
    bool dirty_ = false;
    bool death_event_pending_ = false;
    bool death_save_pending_ = false;
    bool dead_memory_erased_ = false;
    int64_t last_tick_us_ = 0;
    int64_t last_save_us_ = 0;
    int64_t last_response_us_ = -1;
    int64_t last_speech_us_ = 0;
    int64_t next_spontaneous_us_ = 0;
    int64_t pending_announcement_us_ = -1;
    uint32_t hunger_reminder_count_ = 0;
    const char* last_phrase_ = nullptr; // Points into immutable phrase tables.
};
