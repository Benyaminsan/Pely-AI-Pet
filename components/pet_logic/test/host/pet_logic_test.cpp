#include "pet_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
    std::exit(1); } } while (false)

struct FakeMutex { bool held = false; };
namespace {
using Values = std::map<std::string, std::vector<uint8_t>>;
struct Handle { std::string name; Values staged; };
struct Event { int32_t id; PetLogicEventData data; };
std::map<std::string, Values> flash;
std::map<nvs_handle_t, Handle> handles;
std::vector<Event> events;
int64_t now_us = 0;
int core = 0;
int commits = 0;
int erases = 0;
int rejected_event_id = -1;
int rejected_event_count = 0;
bool fail_mutex = false;
esp_err_t open_error = ESP_OK;
esp_err_t set_error = ESP_OK;
esp_err_t commit_error = ESP_OK;
esp_err_t erase_error = ESP_OK;
uint32_t random_state = 123;
auto* const loop = reinterpret_cast<esp_event_loop_handle_t>(1);
const char* const aliases[] = {"egg rice"};
const PetFood foods[] = {{"rice_egg", "Rice and Egg", aliases, 1}, {"apple", "Apple"}};

esp_err_t consume(esp_err_t& value) {
    const auto result = value;
    value = ESP_OK;
    return result;
}
void reset() {
    CHECK(handles.empty());
    flash.clear(); events.clear();
    now_us = 0; core = commits = erases = rejected_event_count = 0;
    rejected_event_id = -1;
    open_error = set_error = commit_error = erase_error = ESP_OK;
    fail_mutex = false;
    random_state = 123;
}
void seconds(double value) { now_us = static_cast<int64_t>(value * 1000000); }
void near(double actual, double expected, double tolerance = 0.0001) {
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "Expected %.9f, got %.9f\n", expected, actual);
        std::exit(1);
    }
}
PetSnapshot snapshot(PetLogic& pet) {
    PetSnapshot state;
    CHECK(pet.get_state(state) == ESP_OK);
    return state;
}
std::size_t count_events(int32_t id) {
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
        [id](const Event& event) { return event.id == id; }));
}
const PetLogicEventData& last_event(int32_t id) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->id == id) { return it->data; }
    }
    CHECK(false);
    std::abort();
}
PetLogicConfig still_config() {
    PetLogicConfig config;
    config.hunger_per_second = config.energy_loss_per_second = 0.0F;
    return config;
}

void test_defaults_and_passive_decay() {
    reset();
    PetLogic pet;
    PetSnapshot before;
    CHECK(pet.get_state(before) == ESP_ERR_INVALID_STATE);
    CHECK(pet.update_tick() == ESP_ERR_INVALID_STATE);
    CHECK(pet.init(loop) == ESP_OK);
    CHECK(pet.init(loop) == ESP_ERR_INVALID_STATE);
    auto state = snapshot(pet);
    near(state.hunger, 72); near(state.happiness, 65); near(state.energy, 80);
    near(state.health, 100); near(state.care_age_seconds, 0);
    CHECK(state.mood == PetMood::HUNGRY && state.desire == PetDesire::FOOD);
    seconds(1); CHECK(pet.update_tick() == ESP_OK);
    state = snapshot(pet);
    near(state.hunger, 72.15); near(state.energy, 79.95);
    seconds(21); CHECK(pet.update_tick() == ESP_OK);
    state = snapshot(pet);
    near(state.happiness, 64.75); near(state.care_age_seconds, 21);
    CHECK(state.desire == PetDesire::FOOD); // Hunger beats stale affection.
    CHECK(commits == 1);                  // No per-tick flash writes.
}

void test_direct_actions_and_clamping() {
    reset();
    auto config = still_config();
    config.initial_hunger = 70; config.initial_happiness = 50;
    PetLogic pet(config);
    CHECK(pet.init(loop) == ESP_OK);
    CHECK(pet.feed_success() == ESP_OK);
    auto state = snapshot(pet);
    near(state.hunger, 40); near(state.happiness, 70);
    CHECK(state.mood == PetMood::HAPPY && state.desire == PetDesire::PLAY);
    CHECK(pet.feed_wrong() == ESP_OK);
    state = snapshot(pet);
    near(state.hunger, 40); near(state.happiness, 65);
    CHECK(state.mood == PetMood::HUNGRY && state.desire == PetDesire::FOOD);
    CHECK(pet.feed_success(1000, 1000) == ESP_OK);
    state = snapshot(pet);
    near(state.hunger, 0); near(state.happiness, 100);
    CHECK(state.successful_feeds == 2 && state.wrong_feeds == 1);
    CHECK(pet.feed_success(-1) == ESP_ERR_INVALID_ARG);
    CHECK(pet.give_affection(std::numeric_limits<float>::quiet_NaN()) == ESP_ERR_INVALID_ARG);
    CHECK(pet.rest(std::numeric_limits<float>::infinity()) == ESP_ERR_INVALID_ARG);
    CHECK(pet.schedule_initial_desire_announcement(-1) == ESP_ERR_INVALID_ARG);
}

void test_affection_and_sleep() {
    reset();
    auto config = still_config();
    config.initial_mood = PetMood::LONELY;
    config.initial_hunger = config.initial_happiness = 30;
    PetLogic pet(config);
    CHECK(pet.init(loop) == ESP_OK);
    CHECK(snapshot(pet).desire == PetDesire::AFFECTION);
    CHECK(pet.give_affection(25) == ESP_OK);
    auto state = snapshot(pet);
    near(state.happiness, 55);
    CHECK(state.mood == PetMood::HAPPY && state.affection_count == 1);
    CHECK(pet.give_affection(25) == ESP_OK);
    CHECK(count_events(PET_EVENT_SPEECH_REQUESTED) == 1); // Touch speech cooldown.
    CHECK(pet.feed_wrong(100) == ESP_OK);
    seconds(21); CHECK(pet.update_tick() == ESP_OK);
    near(snapshot(pet).happiness, 0); // Passive floor must not magically heal sadness.
}

void test_sleep_priority_and_recovery() {
    reset();
    auto config = still_config();
    config.initial_mood = PetMood::SLEEPY; config.initial_energy = 15;
    PetLogic pet(config);
    CHECK(pet.init(loop) == ESP_OK);
    CHECK(snapshot(pet).desire == PetDesire::SLEEP);
    CHECK(pet.rest(100) == ESP_OK);
    near(snapshot(pet).energy, 100);
    CHECK(snapshot(pet).desire == PetDesire::FOOD);
}

void test_food_behavior_and_rotation() {
    reset();
    PetLogic pet(still_config());
    CHECK(pet.init(loop, foods, 2) == ESP_OK);
    const auto before = snapshot(pet);
    const std::string requested = before.requested_food_id;
    const char* query = requested == "rice_egg" ? "  EGG RICE  " : "  aPpLe  ";
    BehaviorOutcome outcome;
    CHECK(pet.handle_confirmed_food(query, outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::CORRECT); // First feed is allowed even at time zero.
    auto state = snapshot(pet);
    CHECK(requested != state.requested_food_id);
    near(state.hunger, 42); near(state.happiness, 85);
    CHECK(std::strcmp(last_event(PET_EVENT_FOOD_EVALUATED).requested_food, requested.c_str()) == 0);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).priority == 100);
    CHECK(pet.handle_confirmed_food(state.requested_food_id, outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::IGNORED_COOLDOWN);
    CHECK(snapshot(pet).successful_feeds == 1);
    seconds(3);
    CHECK(pet.handle_confirmed_food("unknown", outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::WRONG);
    near(snapshot(pet).happiness, 82);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).emotion == PetEmotion::CONFUSED);
    CHECK(pet.handle_confirmed_food("  ", outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::IGNORED_NO_FOOD);
    CHECK(pet.handle_confirmed_food(nullptr, outcome) == ESP_ERR_INVALID_ARG);
}

void test_empty_and_single_food_catalog() {
    reset();
    {
        PetLogic pet;
        CHECK(pet.init(loop) == ESP_OK);
        BehaviorOutcome outcome;
        CHECK(pet.handle_confirmed_food("apple", outcome) == ESP_OK);
        CHECK(outcome == BehaviorOutcome::IGNORED_NO_FOOD);
        CHECK(snapshot(pet).wrong_feeds == 0);
    }
    reset();
    PetLogic pet(still_config());
    CHECK(pet.init(loop, foods, 1) == ESP_OK);
    BehaviorOutcome outcome;
    CHECK(pet.handle_confirmed_food("Rice and Egg", outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::CORRECT);
    CHECK(std::strcmp(snapshot(pet).requested_food_id, "rice_egg") == 0);
}

void test_announcements_and_reminders() {
    reset();
    PetLogic pet(still_config());
    CHECK(pet.init(loop, foods, 2) == ESP_OK);
    CHECK(pet.announce_startup() == ESP_OK);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).priority == 90);
    seconds(3.4); CHECK(pet.update_tick() == ESP_OK);
    CHECK(count_events(PET_EVENT_SPEECH_REQUESTED) == 1);
    seconds(3.5); CHECK(pet.update_tick() == ESP_OK);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).priority == 75);
    seconds(7);
    BehaviorOutcome outcome;
    const auto requested = snapshot(pet);
    CHECK(pet.handle_confirmed_food(requested.requested_food_id, outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::CORRECT);
    seconds(10.9); CHECK(pet.update_tick() == ESP_OK);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).priority == 100);
    seconds(11); CHECK(pet.update_tick() == ESP_OK);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).priority == 75);
}

void test_escalation_and_mood_transitions() {
    reset();
    {
        PetLogic pet(still_config());
        CHECK(pet.init(loop, foods, 2) == ESP_OK);
        seconds(25); CHECK(pet.update_tick() == ESP_OK);
        const auto first = last_event(PET_EVENT_SPEECH_REQUESTED);
        CHECK(first.priority == 70 && first.emotion == PetEmotion::HUNGRY);
        seconds(50); CHECK(pet.update_tick() == ESP_OK);
        CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).priority == 70);
        CHECK(std::strcmp(first.speech, last_event(PET_EVENT_SPEECH_REQUESTED).speech) != 0);
    }
    reset();
    {
        auto config = still_config();
        config.initial_mood = PetMood::HAPPY; config.initial_hunger = 0;
        PetLogic pet(config);
        CHECK(pet.init(loop) == ESP_OK);
        seconds(21); CHECK(pet.update_tick() == ESP_OK);
        CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).emotion == PetEmotion::LONELY);
    }
    reset();
    auto config = still_config();
    config.initial_energy = 26; config.energy_loss_per_second = 1;
    PetLogic pet(config);
    CHECK(pet.init(loop) == ESP_OK);
    seconds(2); CHECK(pet.update_tick() == ESP_OK);
    CHECK(last_event(PET_EVENT_SPEECH_REQUESTED).emotion == PetEmotion::SLEEPY);
}

void test_health_thresholds_and_delayed_ticks() {
    reset();
    auto config = still_config();
    config.initial_hunger = 80; config.hunger_per_second = 1;
    config.starvation_health_loss_per_second = 2;
    {
        PetLogic pet(config);
        CHECK(pet.init(loop) == ESP_OK);
        seconds(15); CHECK(pet.update_tick() == ESP_OK);
        near(snapshot(pet).health, 90); // Only 5 seconds above hunger 90.
    }
    reset();
    {
        PetLogic pet(config);
        CHECK(pet.init(loop) == ESP_OK);
        for (int i = 1; i <= 15; ++i) { seconds(i); CHECK(pet.update_tick() == ESP_OK); }
        near(snapshot(pet).health, 90);
    }
    reset();
    config = still_config();
    config.initial_hunger = 0; config.initial_energy = 5;
    config.energy_loss_per_second = 1; config.exhaustion_health_loss_per_second = 2;
    PetLogic pet(config);
    CHECK(pet.init(loop) == ESP_OK);
    seconds(10); CHECK(pet.update_tick() == ESP_OK);
    near(snapshot(pet).health, 90); // Only 5 seconds at energy zero.
}

PetLogicConfig dying_config() {
    auto config = still_config();
    config.initial_hunger = 100; config.initial_health = 10;
    config.starvation_health_loss_per_second = 2;
    return config;
}

void test_death_reboot_and_isolated_erase() {
    reset();
    flash["wifi"]["credentials"] = {1, 2, 3};
    const auto config = dying_config();
    {
        PetLogic pet(config);
        CHECK(pet.init(loop, foods, 2) == ESP_OK);
        seconds(8); CHECK(pet.update_tick() == ESP_OK);
        auto state = snapshot(pet);
        CHECK(!state.alive && state.mood == PetMood::DEAD && state.desire == PetDesire::NONE);
        near(state.health, 0); near(state.care_age_seconds, 5);
        CHECK(commits == 2 && count_events(PET_EVENT_DIED) == 1);
        seconds(9); CHECK(pet.update_tick() == ESP_OK);
        CHECK(pet.feed_success() == ESP_ERR_INVALID_STATE);
        CHECK(pet.give_affection() == ESP_ERR_INVALID_STATE);
        CHECK(pet.rest() == ESP_ERR_INVALID_STATE);
        BehaviorOutcome outcome;
        CHECK(pet.handle_confirmed_food("apple", outcome) == ESP_OK);
        CHECK(outcome == BehaviorOutcome::IGNORED_DEAD);
        near(snapshot(pet).care_age_seconds, 5);
        CHECK(count_events(PET_EVENT_DIED) == 1 && count_events(PET_EVENT_SPEECH_REQUESTED) == 0);
    }
    seconds(1000); events.clear();
    PetLogic rebooted(config);
    CHECK(rebooted.init(loop, foods, 2) == ESP_OK);
    CHECK(!snapshot(rebooted).alive);
    CHECK(rebooted.announce_startup() == ESP_ERR_INVALID_STATE);
    CHECK(rebooted.update_tick() == ESP_OK);
    near(snapshot(rebooted).care_age_seconds, 5);
    CHECK(count_events(PET_EVENT_DIED) == 1);
    CHECK(rebooted.erase_memory() == ESP_OK);
    const auto fresh = snapshot(rebooted);
    CHECK(fresh.alive && fresh.mood == PetMood::HUNGRY);
    near(fresh.health, 10); near(fresh.care_age_seconds, 0);
    CHECK(flash["pet_logic"].empty());
    CHECK(flash["wifi"]["credentials"] == std::vector<uint8_t>({1, 2, 3}));
    CHECK(count_events(PET_EVENT_MEMORY_ERASED) == 1 && erases == 1);
}

void test_combined_health_drain() {
    reset();
    auto config = dying_config();
    config.initial_energy = 2; config.energy_loss_per_second = 1;
    config.exhaustion_health_loss_per_second = 1;
    PetLogic pet(config);
    CHECK(pet.init(loop) == ESP_OK);
    seconds(10); CHECK(pet.update_tick() == ESP_OK);
    // 2s starvation (4 points), then 2s combined loss (6 points).
    near(snapshot(pet).care_age_seconds, 4);
    near(snapshot(pet).health, 0);
}

void test_checkpoint_and_active_age() {
    reset();
    {
        PetLogic pet(still_config());
        CHECK(pet.init(loop, foods, 2) == ESP_OK);
        for (int i = 1; i < 60; ++i) { seconds(i); CHECK(pet.update_tick() == ESP_OK); }
        CHECK(commits == 1);
        seconds(60); CHECK(pet.update_tick() == ESP_OK);
        CHECK(commits == 2);
        seconds(65); CHECK(pet.save_memory() == ESP_OK);
        CHECK(commits == 3);
    }
    seconds(1000);
    PetLogic rebooted(still_config());
    const PetFood reversed[] = {foods[1], foods[0]};
    CHECK(rebooted.init(loop, reversed, 2) == ESP_OK);
    near(snapshot(rebooted).care_age_seconds, 65);
    CHECK(rebooted.update_tick() == ESP_OK);
    near(snapshot(rebooted).care_age_seconds, 65); // Power-off duration excluded.
}

void test_death_write_retry_and_erase_failure() {
    reset();
    PetLogic pet(dying_config());
    CHECK(pet.init(loop) == ESP_OK);
    set_error = ESP_FAIL;
    seconds(8); CHECK(pet.save_memory() == ESP_FAIL);
    CHECK(commits == 1); // A failed explicit checkpoint must not write twice in one call.
    CHECK(!snapshot(pet).alive);
    seconds(9); CHECK(pet.update_tick() == ESP_OK);
    CHECK(commits == 2 && count_events(PET_EVENT_DIED) == 1);
    commit_error = ESP_FAIL;
    CHECK(pet.erase_memory() == ESP_FAIL);
    CHECK(!snapshot(pet).alive);
    near(snapshot(pet).care_age_seconds, 5);
    CHECK(pet.erase_memory() == ESP_OK);
    CHECK(snapshot(pet).alive);
}

void test_event_backpressure_and_snapshot_copy() {
    reset();
    {
        PetLogic pet(dying_config());
        CHECK(pet.init(loop) == ESP_OK);
        rejected_event_id = PET_EVENT_DIED; rejected_event_count = 1;
        seconds(8); CHECK(pet.update_tick() == ESP_ERR_TIMEOUT);
        CHECK(count_events(PET_EVENT_DIED) == 0);
        seconds(9); CHECK(pet.update_tick() == ESP_OK);
        CHECK(count_events(PET_EVENT_DIED) == 1);
    }
    reset();
    PetLogic pet(still_config());
    CHECK(pet.init(loop, foods, 2) == ESP_OK);
    const auto requested = snapshot(pet);
    rejected_event_id = PET_EVENT_SPEECH_REQUESTED; rejected_event_count = 1;
    BehaviorOutcome outcome;
    CHECK(pet.handle_confirmed_food(requested.requested_food_id, outcome) == ESP_ERR_TIMEOUT);
    CHECK(outcome == BehaviorOutcome::CORRECT && snapshot(pet).successful_feeds == 1);
    const auto saved_event = last_event(PET_EVENT_FOOD_EVALUATED);
    CHECK(pet.handle_confirmed_food(snapshot(pet).requested_food_id, outcome) == ESP_OK);
    CHECK(outcome == BehaviorOutcome::IGNORED_COOLDOWN);
    CHECK(saved_event.state.successful_feeds == 1);
}

void test_invalid_init_and_corrupt_persistence() {
    reset();
    {
        fail_mutex = true;
        PetLogic pet;
        CHECK(pet.init(loop) == ESP_ERR_NO_MEM);
        fail_mutex = false;
    }
    {
        auto config = still_config(); config.hunger_per_second = -1;
        PetLogic pet(config);
        CHECK(pet.init(loop) == ESP_ERR_INVALID_ARG);
    }
    {
        PetLogic pet;
        CHECK(pet.init(nullptr) == ESP_ERR_INVALID_ARG);
        CHECK(pet.init(loop, nullptr, 1) == ESP_ERR_INVALID_ARG);
        open_error = ESP_FAIL;
        CHECK(pet.init(loop) == ESP_FAIL);
        CHECK(pet.init(loop) == ESP_OK);
    }
    const auto valid = flash["pet_logic"]["state_v1"];
    {
        flash["pet_logic"]["state_v1"] = {1};
        PetLogic pet;
        CHECK(pet.init(loop) == ESP_ERR_INVALID_SIZE);
    }
    {
        flash["pet_logic"]["state_v1"] = valid;
        flash["pet_logic"]["state_v1"][0] = 99;
        PetLogic pet;
        CHECK(pet.init(loop) == ESP_ERR_NOT_SUPPORTED);
    }
    {
        flash["pet_logic"]["state_v1"] = valid;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(flash["pet_logic"]["state_v1"].data() + 32, &nan, sizeof(nan));
        PetLogic pet;
        CHECK(pet.init(loop) == ESP_ERR_INVALID_STATE);
    }
    CHECK(erases == 0 && commits == 1);
}

void test_core_affinity() {
    reset();
    PetLogic pet;
    core = 1; CHECK(pet.init(loop) == ESP_ERR_INVALID_STATE);
    core = 0; CHECK(pet.init(loop) == ESP_OK);
    core = 1;
    CHECK(pet.update_tick() == ESP_ERR_INVALID_STATE);
    CHECK(pet.feed_success() == ESP_ERR_INVALID_STATE);
    CHECK(pet.erase_memory() == ESP_ERR_INVALID_STATE);
    near(snapshot(pet).hunger, 72); // Cross-core snapshot remains available.
    core = 0;
}

void test_erased_dead_save_stays_erased() {
    reset();
    {
        PetLogic pet(dying_config());
        CHECK(pet.init(loop) == ESP_OK);
        CHECK(pet.erase_memory(false) == ESP_ERR_INVALID_STATE);
        seconds(8); CHECK(pet.update_tick() == ESP_OK);
        CHECK(pet.erase_memory(false) == ESP_OK && !snapshot(pet).alive);
        CHECK(flash["pet_logic"].empty());
        seconds(1000); CHECK(pet.update_tick() == ESP_OK);
        CHECK(pet.save_memory() == ESP_ERR_INVALID_STATE && flash["pet_logic"].empty());
    }
    PetLogic fresh(dying_config());
    CHECK(fresh.init(loop) == ESP_OK && snapshot(fresh).alive);
    near(snapshot(fresh).care_age_seconds, 0);
}
} // namespace

SemaphoreHandle_t xSemaphoreCreateMutex() { return fail_mutex ? nullptr : new FakeMutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
    CHECK(!mutex->held); mutex->held = true; return 1;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
    CHECK(mutex->held); mutex->held = false; return 1;
}
void vSemaphoreDelete(SemaphoreHandle_t mutex) { CHECK(!mutex->held); delete mutex; }
BaseType_t xPortGetCoreID() { return core; }
int64_t esp_timer_get_time() { return now_us; }
uint32_t esp_random() { random_state = random_state * 1664525U + 1013904223U; return random_state; }
esp_err_t esp_event_post_to(esp_event_loop_handle_t event_loop, esp_event_base_t base, int32_t id,
                              const void* data, std::size_t size, TickType_t wait) {
    CHECK(event_loop == loop && base == PET_LOGIC_EVENT && wait == 0);
    CHECK(size == sizeof(PetLogicEventData));
    if (rejected_event_count && rejected_event_id == id) {
        --rejected_event_count; return ESP_ERR_TIMEOUT;
    }
    Event event{id, {}};
    std::memcpy(&event.data, data, size);
    events.push_back(event);
    return ESP_OK;
}
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* result) {
    CHECK(mode == NVS_READWRITE);
    if (open_error != ESP_OK) { return consume(open_error); }
    *result = handles.empty() ? 1 : handles.rbegin()->first + 1;
    handles[*result] = Handle{name, flash[name]};
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { CHECK(handles.erase(handle) == 1); }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* result, std::size_t* size) {
    const auto& values = handles.at(handle).staged;
    const auto it = values.find(key);
    if (it == values.end()) { return ESP_ERR_NVS_NOT_FOUND; }
    if (result && *size < it->second.size()) { return ESP_ERR_INVALID_SIZE; }
    if (result) { std::memcpy(result, it->second.data(), it->second.size()); }
    *size = it->second.size();
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* data, std::size_t size) {
    if (set_error != ESP_OK) { return consume(set_error); }
    const auto* begin = static_cast<const uint8_t*>(data);
    handles.at(handle).staged[key] = std::vector<uint8_t>(begin, begin + size);
    return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t handle) {
    if (erase_error != ESP_OK) { return consume(erase_error); }
    ++erases; handles.at(handle).staged.clear(); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    if (commit_error != ESP_OK) { return consume(commit_error); }
    ++commits;
    const auto& value = handles.at(handle);
    flash[value.name] = value.staged;
    return ESP_OK;
}

int main() {
    const struct { const char* name; void (*run)(); } tests[] = {
        {"defaults and passive decay", test_defaults_and_passive_decay},
        {"direct actions and clamping", test_direct_actions_and_clamping},
        {"affection recovery and speech cooldown", test_affection_and_sleep},
        {"sleep priority and recovery", test_sleep_priority_and_recovery},
        {"food behavior, aliases, cooldown, rotation", test_food_behavior_and_rotation},
        {"empty and single-item catalogs", test_empty_and_single_food_catalog},
        {"startup and delayed food announcements", test_announcements_and_reminders},
        {"escalated reminders and mood transitions", test_escalation_and_mood_transitions},
        {"health threshold integration and delayed ticks", test_health_thresholds_and_delayed_ticks},
        {"death, reboot, and namespace-only erase", test_death_reboot_and_isolated_erase},
        {"combined starvation and exhaustion", test_combined_health_drain},
        {"checkpoint interval and powered-on age", test_checkpoint_and_active_age},
        {"NVS write retry and erase failure", test_death_write_retry_and_erase_failure},
        {"event backpressure and payload copies", test_event_backpressure_and_snapshot_copy},
        {"invalid configuration and corrupt NVS", test_invalid_init_and_corrupt_persistence},
        {"Core-0 mutation and cross-core reads", test_core_affinity},
        {"erased dead state never recreates its checkpoint", test_erased_dead_save_stays_erased},
    };
    for (const auto& test : tests) { test.run(); std::printf("PASS %s\n", test.name); }
    CHECK(handles.empty());
    std::printf("All %zu host tests passed.\n", sizeof(tests) / sizeof(tests[0]));
}
