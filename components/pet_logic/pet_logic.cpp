#include "pet_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"

ESP_EVENT_DEFINE_BASE(PET_LOGIC_EVENT);

namespace {
constexpr char kNamespace[] = "pet_logic";
constexpr char kStateKey[] = "state_v1";
constexpr uint32_t kStateVersion = 1;
constexpr int64_t kSecond = 1000000;

// Fixed-width fields; never serialize pointers, the live class, or FreeRTOS handles.
struct StoredState {
    uint32_t version;
    uint32_t mood;
    double care_age_seconds;
    double interaction_seconds;
    double affection_seconds;
    float hunger;
    float happiness;
    float energy;
    float health;
    uint32_t successful_feeds;
    uint32_t wrong_feeds;
    uint32_t affection_count;
    uint32_t reserved;
    char requested_food_id[32];
};
static_assert(std::is_trivially_copyable<StoredState>::value, "NVS state must be POD");
static_assert(sizeof(StoredState) == 96, "Change the NVS version if the layout changes");
static_assert(std::is_trivially_copyable<PetLogicEventData>::value, "Events must be copyable");

class Lock final {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SemaphoreHandle_t mutex_;
};

template <std::size_t N>
void copy_text(char (&destination)[N], const char* source) {
    std::snprintf(destination, N, "%s", source ? source : "");
}

template <typename T, std::size_t N>
constexpr std::size_t count_of(const T (&)[N]) { return N; }

float bar(double value) {
    return static_cast<float>(std::clamp(value, 0.0, 100.0));
}

bool nonnegative(double value) { return std::isfinite(value) && value >= 0.0; }
bool valid_bar(double value) { return nonnegative(value) && value <= 100.0; }

void increment(uint32_t& value) {
    if (value != std::numeric_limits<uint32_t>::max()) { ++value; }
}

esp_err_t first_error(esp_err_t first, esp_err_t next) {
    return first != ESP_OK ? first : next;
}

bool equals_query(const char* query, const char* expected) {
    if (!query || !expected) { return false; }
    while (std::isspace(static_cast<unsigned char>(*query))) { ++query; }
    std::size_t length = std::strlen(query);
    while (length && std::isspace(static_cast<unsigned char>(query[length - 1]))) { --length; }
    if (length != std::strlen(expected)) { return false; }
    for (std::size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(query[i])) !=
            std::tolower(static_cast<unsigned char>(expected[i]))) { return false; }
    }
    return true;
}

const char* const kCorrect[] = {
    "Yay! That's exactly what I wanted! Thank you!",
    "Whoa! You found it! You are the best friend ever!",
    "Yummy yummy in my tummy! That was so delicious!",
    "Om-nom-nom! That was super tasty! High five!",
    "Yay, thank you! That was super duper yummy!",
};
const char* const kWrong[] = {
    "Hmm... that's not what I asked for. Let's try again!",
    "Oopsie-daisy! That looks yummy, but I really wanted %s.",
    "Tee-hee, almost! But my tummy is wishing for %s!",
    "Aww, good try! Could you find %s for me please?",
};
const char* const kGentle[] = {
    "Hmm... my tummy is getting hungry. Can we find %s?",
    "Hey buddy! Could you help me find %s?",
    "Ooh, my tummy says snack time! Pretty please, can I have %s?",
    "I'm dreaming of some yummy %s! Is it ready?",
    "I'm feeling a little hungry! Can we have %s together?",
};
const char* const kEscalated[] = {
    "Rumble rumble! My tummy is still craving %s! Did you find it?",
    "I'm still wishing for %s! Pretty please, bestie?",
    "My tummy is waiting so patiently for %s!",
    "Hey bestie! Don't forget my %s, I'm super hungry!",
    "Rumble rumble! My tummy needs some yummy %s to recharge!",
};
const char* const kFoodDesire[] = {
    "Ooh! Next, I would love some yummy %s!",
    "Hey bestie! My tummy is wishing for %s now!",
    "Mmm, I'm dreaming of %s! Can you find it for me?",
};
const char* const kStartup[] = {
    "Yay, you're here! Hi bestie! I'm ready to play!",
    "Yippee! Hello friend! Let's find some yummy food!",
    "Hi sunshine! I'm so happy to see you today!",
};
const char* const kPat[] = {
    "Tee-hee! That tickles! You give the best head pats!",
    "Purrrr, that feels so warm and cozy! More pats please!",
    "Aww, you're so sweet! I feel so happy and loved now!",
    "Yay! Soft head pats are my favorite thing in the whole world!",
    "Mmm, that's the spot! You're the best bestie ever!",
};
const char* const kLonely[] = {
    "I'm feeling a little lonely... Can you give me some gentle head pats?",
    "Could you stroke my head? Warm pats make me feel so happy and safe!",
    "Bestie, are you there? I really miss your soft cuddles!",
    "Pet me please! My head is waiting for your gentle pats, tee-hee!",
    "A warm hug and head pats would make my day! Don't leave me alone, okay?",
    "Can I have some pats, please? I love when you stroke my forehead!",
    "Where did you go, buddy? Come pet me and play!",
};
const char* const kPlay[] = {
    "I'm so happy today! What fun game should we play next?",
    "Yay, you're the best friend ever! Look at me bounce!",
    "Tee-hee, let's play together! Show me something fun!",
    "Having you here makes me so joyful! We make the best team!",
};
const char* const kSleepy[] = {
    "I'm getting sleepy... Maybe it's time for a little nap.",
    "Yaaawn... My eyes are heavy. Nap time feels nice.",
    "So sleepy... Could you stay close while I rest a bit?",
};
} // namespace

PetLogic::PetLogic() : PetLogic(PetLogicConfig{}) {}
PetLogic::PetLogic(const PetLogicConfig& config)
    : config_(config), mutex_(xSemaphoreCreateMutex()) {}

PetLogic::~PetLogic() {
    // Lifecycle is caller-owned: the periodic task must already have stopped.
    // Saving is explicit so destruction never silently writes flash.
    if (nvs_) { nvs_close(nvs_); }
    if (mutex_) { vSemaphoreDelete(mutex_); }
}

bool PetLogic::valid_config() const {
    return static_cast<unsigned>(config_.initial_mood) < static_cast<unsigned>(PetMood::DEAD) &&
        valid_bar(config_.initial_hunger) && valid_bar(config_.initial_happiness) &&
        valid_bar(config_.initial_energy) && valid_bar(config_.initial_health) &&
        config_.initial_health > 0.0F && nonnegative(config_.response_cooldown_seconds) &&
        nonnegative(config_.hunger_per_second) && nonnegative(config_.energy_loss_per_second) &&
        nonnegative(config_.affection_grace_seconds) &&
        nonnegative(config_.happiness_loss_per_second) && valid_bar(config_.minimum_passive_happiness) &&
        valid_bar(config_.starvation_hunger) &&
        nonnegative(config_.starvation_health_loss_per_second) &&
        nonnegative(config_.exhaustion_health_loss_per_second) &&
        nonnegative(config_.feeding_health_boost) && config_.autosave_interval_seconds != 0;
}

esp_err_t PetLogic::check_mutation_context() const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    return xPortGetCoreID() == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t PetLogic::init(esp_event_loop_handle_t event_loop,
                         const PetFood* foods, std::size_t food_count) {
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (initialized_) { return ESP_ERR_INVALID_STATE; }
    if (!event_loop || !valid_config() || (food_count && !foods) || food_count > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (std::size_t i = 0; i < food_count; ++i) {
        if (!foods[i].id || !foods[i].id[0] || std::strlen(foods[i].id) >= 32 ||
            !foods[i].name || !foods[i].name[0] || std::strlen(foods[i].name) >= 48 ||
            (foods[i].alias_count && !foods[i].aliases)) { return ESP_ERR_INVALID_ARG; }
        for (std::size_t j = 0; j < foods[i].alias_count; ++j) {
            if (!foods[i].aliases[j]) { return ESP_ERR_INVALID_ARG; }
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (equals_query(foods[i].id, foods[j].id)) { return ESP_ERR_INVALID_ARG; }
        }
    }
    esp_err_t error = nvs_open(kNamespace, NVS_READWRITE, &nvs_);
    if (error != ESP_OK) { return error; }
    event_loop_ = event_loop;
    foods_ = foods;
    food_count_ = food_count;
    const int64_t now = esp_timer_get_time();
    reset_state_locked(now);
    error = load_locked();
    if (error == ESP_ERR_NVS_NOT_FOUND) { error = save_locked(now); }
    if (error != ESP_OK) {
        nvs_close(nvs_);
        nvs_ = 0;
        return error;
    }
    initialized_ = true;
    previous_mood_ = state_.mood;
    // Notify subscribers once on the first tick, including after a dead-pet reboot.
    death_event_pending_ = !state_.alive;
    return ESP_OK;
}

void PetLogic::reset_state_locked(int64_t now) {
    state_ = PetSnapshot{};
    state_.mood = config_.initial_mood;
    state_.hunger = config_.initial_hunger;
    state_.happiness = config_.initial_happiness;
    state_.energy = config_.initial_energy;
    state_.health = config_.initial_health;
    select_food_locked(false);
    update_desire_locked();
    previous_mood_ = state_.mood;
    last_tick_us_ = last_save_us_ = last_speech_us_ = now;
    last_response_us_ = pending_announcement_us_ = -1;
    next_spontaneous_us_ = (18 + esp_random() % 7) * kSecond;
    hunger_reminder_count_ = 0;
    last_phrase_ = nullptr;
    death_event_pending_ = death_save_pending_ = false;
    dead_memory_erased_ = false;
    dirty_ = true;
}

void PetLogic::select_food_locked(bool exclude_current) {
    if (!food_count_) { return; }
    if (exclude_current && food_count_ > 1) {
        std::size_t next = esp_random() % (food_count_ - 1);
        if (next >= food_index_) { ++next; }
        food_index_ = next;
    } else {
        food_index_ = esp_random() % food_count_;
    }
    copy_text(state_.requested_food_id, foods_[food_index_].id);
    copy_text(state_.requested_food_name, foods_[food_index_].name);
}

bool PetLogic::matches_food_locked(const char* detected) const {
    if (!food_count_) { return false; }
    const PetFood& food = foods_[food_index_];
    if (equals_query(detected, food.id) || equals_query(detected, food.name)) { return true; }
    for (std::size_t i = 0; i < food.alias_count; ++i) {
        if (equals_query(detected, food.aliases[i])) { return true; }
    }
    return false;
}

void PetLogic::update_desire_locked() {
    if (!state_.alive) { state_.desire = PetDesire::NONE; }
    else if (state_.energy < 25.0F) { state_.desire = PetDesire::SLEEP; }
    else if (state_.hunger >= 50.0F || state_.mood == PetMood::HUNGRY) {
        state_.desire = PetDesire::FOOD;
    } else if (state_.mood == PetMood::LONELY ||
               state_.time_since_affection_seconds > config_.affection_grace_seconds ||
               state_.happiness < 50.0F) { state_.desire = PetDesire::AFFECTION; }
    else { state_.desire = PetDesire::PLAY; }
}

void PetLogic::advance_locked(int64_t now) {
    if (now <= last_tick_us_) { return; }
    double elapsed = static_cast<double>(now - last_tick_us_) / kSecond;
    last_tick_us_ = now;
    if (!state_.alive) { return; }

    // Integrate only the part of the interval AFTER each health threshold is crossed.
    // This also locates the exact death time in a delayed tick, freezing age there.
    const double infinity = std::numeric_limits<double>::infinity();
    double starts[2] = {
        state_.hunger >= config_.starvation_hunger ? 0.0 :
            (config_.hunger_per_second > 0.0F ?
             (config_.starvation_hunger - state_.hunger) / config_.hunger_per_second : infinity),
        state_.energy <= 0.0F ? 0.0 :
            (config_.energy_loss_per_second > 0.0F ?
             state_.energy / config_.energy_loss_per_second : infinity),
    };
    double rates[2] = {config_.starvation_health_loss_per_second,
                       config_.exhaustion_health_loss_per_second};
    if (starts[0] > starts[1]) { std::swap(starts[0], starts[1]); std::swap(rates[0], rates[1]); }
    double health = state_.health;
    double cursor = 0.0;
    double rate = 0.0;
    bool died = false;
    for (std::size_t i = 0; i < 3; ++i) {
        const double end = i < 2 ? std::min(elapsed, starts[i]) : elapsed;
        if (rate > 0.0 && health <= (end - cursor) * rate) {
            elapsed = cursor + health / rate;
            health = 0.0;
            died = true;
            break;
        }
        health -= (end - cursor) * rate;
        cursor = end;
        if (i < 2) { rate += rates[i]; }
    }

    const double lonely_seconds = elapsed - std::min(elapsed, std::max(0.0,
        config_.affection_grace_seconds - state_.time_since_affection_seconds));
    state_.hunger = bar(state_.hunger + elapsed * config_.hunger_per_second);
    state_.energy = bar(state_.energy - elapsed * config_.energy_loss_per_second);
    if (state_.happiness > config_.minimum_passive_happiness) {
        state_.happiness = static_cast<float>(std::max<double>(config_.minimum_passive_happiness,
            state_.happiness - lonely_seconds * config_.happiness_loss_per_second));
    }
    state_.health = bar(health);
    state_.care_age_seconds += elapsed;
    state_.time_since_interaction_seconds += elapsed;
    state_.time_since_affection_seconds += elapsed;
    if (died || state_.health <= 0.0F) {
        state_.health = 0.0F;
        state_.alive = false;
        state_.mood = PetMood::DEAD;
        pending_announcement_us_ = -1;
        death_event_pending_ = death_save_pending_ = true;
    } else if (state_.energy < 25.0F) { state_.mood = PetMood::SLEEPY; }
    else if (state_.hunger >= 50.0F) { state_.mood = PetMood::HUNGRY; }
    else if (state_.time_since_affection_seconds > config_.affection_grace_seconds) {
        state_.mood = PetMood::LONELY;
    }
    update_desire_locked();
    dirty_ = true;
}

esp_err_t PetLogic::load_locked() {
    StoredState stored{};
    std::size_t size = 0;
    esp_err_t error = nvs_get_blob(nvs_, kStateKey, nullptr, &size);
    if (error != ESP_OK) { return error; }
    if (size != sizeof(stored)) { return ESP_ERR_INVALID_SIZE; }
    error = nvs_get_blob(nvs_, kStateKey, &stored, &size);
    if (error != ESP_OK) { return error; }
    if (stored.version != kStateVersion) { return ESP_ERR_NOT_SUPPORTED; }
    if (stored.mood > static_cast<uint32_t>(PetMood::DEAD) ||
        !valid_bar(stored.hunger) || !valid_bar(stored.happiness) ||
        !valid_bar(stored.energy) || !valid_bar(stored.health) ||
        !nonnegative(stored.care_age_seconds) || !nonnegative(stored.interaction_seconds) ||
        !nonnegative(stored.affection_seconds) ||
        !std::memchr(stored.requested_food_id, '\0', sizeof(stored.requested_food_id)) ||
        ((stored.health == 0.0F) != (stored.mood == static_cast<uint32_t>(PetMood::DEAD)))) {
        return ESP_ERR_INVALID_STATE;
    }
    state_.mood = static_cast<PetMood>(stored.mood);
    state_.alive = stored.health > 0.0F;
    state_.hunger = stored.hunger;
    state_.happiness = stored.happiness;
    state_.energy = stored.energy;
    state_.health = stored.health;
    state_.care_age_seconds = stored.care_age_seconds;
    state_.time_since_interaction_seconds = stored.interaction_seconds;
    state_.time_since_affection_seconds = stored.affection_seconds;
    state_.successful_feeds = stored.successful_feeds;
    state_.wrong_feeds = stored.wrong_feeds;
    state_.affection_count = stored.affection_count;
    // Resolve by ID so reordering the caller's catalog cannot change the request.
    for (std::size_t i = 0; i < food_count_; ++i) {
        if (std::strcmp(foods_[i].id, stored.requested_food_id) == 0) {
            food_index_ = i;
            copy_text(state_.requested_food_id, foods_[i].id);
            copy_text(state_.requested_food_name, foods_[i].name);
            break;
        }
    }
    update_desire_locked();
    dirty_ = false;
    return ESP_OK;
}

esp_err_t PetLogic::save_locked(int64_t now) {
    if (dead_memory_erased_) { return ESP_ERR_INVALID_STATE; }
    StoredState stored{};
    stored.version = kStateVersion;
    stored.mood = static_cast<uint32_t>(state_.mood);
    stored.care_age_seconds = state_.care_age_seconds;
    stored.interaction_seconds = state_.time_since_interaction_seconds;
    stored.affection_seconds = state_.time_since_affection_seconds;
    stored.hunger = state_.hunger;
    stored.happiness = state_.happiness;
    stored.energy = state_.energy;
    stored.health = state_.health;
    stored.successful_feeds = state_.successful_feeds;
    stored.wrong_feeds = state_.wrong_feeds;
    stored.affection_count = state_.affection_count;
    copy_text(stored.requested_food_id, state_.requested_food_id);
    esp_err_t error = nvs_set_blob(nvs_, kStateKey, &stored, sizeof(stored));
    if (error == ESP_OK) { error = nvs_commit(nvs_); }
    if (error == ESP_OK) {
        dirty_ = death_save_pending_ = false;
        last_save_us_ = now;
    }
    return error;
}

esp_err_t PetLogic::publish_locked(PetLogicEventId id, const PetLogicEventData* event) {
    PetLogicEventData data = event ? *event : PetLogicEventData{};
    data.state = state_;
    // Never wait with a mutex held: subscribers may immediately call get_state().
    return esp_event_post_to(event_loop_, PET_LOGIC_EVENT, id, &data, sizeof(data), 0);
}

esp_err_t PetLogic::finish_locked(int64_t now, esp_err_t action_error, bool force_save) {
    esp_err_t error = ESP_OK;
    if (!dead_memory_erased_ && (force_save || death_save_pending_ ||
        (dirty_ && now - last_save_us_ >= config_.autosave_interval_seconds * kSecond))) {
        error = save_locked(now);
    }
    if (death_event_pending_) {
        const esp_err_t posted = publish_locked(PET_EVENT_DIED);
        if (posted == ESP_OK) { death_event_pending_ = false; }
        error = first_error(error, posted);
    }
    error = first_error(error, action_error);
    return first_error(error, publish_locked(PET_EVENT_STATE_CHANGED));
}

bool PetLogic::can_respond_locked(int64_t now) const {
    return last_response_us_ < 0 ||
        static_cast<double>(now - last_response_us_) >= config_.response_cooldown_seconds * kSecond;
}

void PetLogic::mark_speech_locked(int64_t now, uint32_t maximum_interval_seconds) {
    last_response_us_ = last_speech_us_ = now;
    next_spontaneous_us_ = (18 + esp_random() % (maximum_interval_seconds - 17)) * kSecond;
}

esp_err_t PetLogic::speak_locked(const char* const* phrases, std::size_t count,
                                 PetEmotion emotion, uint8_t priority, int64_t now,
                                 const char* food_name) {
    std::size_t index = esp_random() % count;
    if (count > 1 && phrases[index] == last_phrase_) {
        index = (index + 1 + esp_random() % (count - 1)) % count;
    }
    const char* phrase = phrases[index];
    PetLogicEventData event{};
    event.emotion = emotion;
    if (emotion == PetEmotion::HAPPY && std::strstr(phrase, "Whoa")) {
        event.emotion = PetEmotion::EXCITED;
    }
    event.priority = priority;
    copy_text(event.requested_food, state_.requested_food_id);
    char lowercase_name[48]{};
    copy_text(lowercase_name, food_name ? food_name : "food");
    for (char& c : lowercase_name) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const char* placeholder = std::strstr(phrase, "%s");
    if (placeholder) {
        std::snprintf(event.speech, sizeof(event.speech), "%.*s%s%s",
            static_cast<int>(placeholder - phrase), phrase, lowercase_name, placeholder + 2);
    } else { copy_text(event.speech, phrase); }
    const esp_err_t error = publish_locked(PET_EVENT_SPEECH_REQUESTED, &event);
    if (error == ESP_OK) { last_phrase_ = phrase; mark_speech_locked(now); }
    return error;
}

esp_err_t PetLogic::vocalize_locked(int64_t now) {
    if (!state_.alive || !can_respond_locked(now)) { return ESP_OK; }
    const char* name = state_.requested_food_name[0] ? state_.requested_food_name : "food";
    if (pending_announcement_us_ >= 0 && now >= pending_announcement_us_) {
        const esp_err_t error = speak_locked(kFoodDesire, count_of(kFoodDesire),
            PetEmotion::HUNGRY, 75, now, name);
        if (error == ESP_OK) { pending_announcement_us_ = -1; hunger_reminder_count_ = 0; }
        return error;
    }
    if (state_.mood != previous_mood_) {
        esp_err_t error = ESP_OK;
        if (state_.mood == PetMood::LONELY) {
            error = speak_locked(kLonely, count_of(kLonely), PetEmotion::LONELY, 75, now);
        } else if (state_.mood == PetMood::SLEEPY) {
            error = speak_locked(kSleepy, count_of(kSleepy), PetEmotion::SLEEPY, 60, now);
        }
        if (error == ESP_OK) { previous_mood_ = state_.mood; }
        if (error != ESP_OK || last_speech_us_ == now) { return error; }
    }
    if (now - last_speech_us_ < next_spontaneous_us_) { return ESP_OK; }
    esp_err_t error = ESP_OK;
    switch (state_.desire) {
    case PetDesire::FOOD:
        error = hunger_reminder_count_ == 0 ?
            speak_locked(kGentle, count_of(kGentle), PetEmotion::HUNGRY, 70, now, name) :
            speak_locked(kEscalated, count_of(kEscalated), PetEmotion::HUNGRY, 70, now, name);
        if (error == ESP_OK) { increment(hunger_reminder_count_); }
        break;
    case PetDesire::AFFECTION:
        error = speak_locked(kLonely, count_of(kLonely), PetEmotion::LONELY, 75, now);
        break;
    case PetDesire::PLAY:
        error = speak_locked(kPlay, count_of(kPlay), PetEmotion::HAPPY, 65, now);
        break;
    case PetDesire::SLEEP:
        error = speak_locked(kSleepy, count_of(kSleepy), PetEmotion::SLEEPY, 60, now);
        break;
    case PetDesire::NONE: break;
    }
    if (error == ESP_OK) { mark_speech_locked(now, 25); }
    return error;
}

esp_err_t PetLogic::update_tick() {
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    return finish_locked(now, vocalize_locked(now));
}

esp_err_t PetLogic::get_state(PetSnapshot& result) const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    result = state_;
    return ESP_OK;
}

void PetLogic::feed_success_locked(float hunger_delta, float happiness_delta) {
    state_.hunger = bar(static_cast<double>(state_.hunger) - hunger_delta);
    state_.happiness = bar(static_cast<double>(state_.happiness) + happiness_delta);
    state_.health = bar(static_cast<double>(state_.health) + config_.feeding_health_boost);
    state_.time_since_interaction_seconds = 0.0;
    state_.mood = PetMood::HAPPY;
    increment(state_.successful_feeds);
    update_desire_locked();
    dirty_ = true;
}

void PetLogic::feed_wrong_locked(float happiness_delta) {
    state_.happiness = bar(static_cast<double>(state_.happiness) - happiness_delta);
    state_.time_since_interaction_seconds = 0.0;
    state_.mood = PetMood::HUNGRY;
    increment(state_.wrong_feeds);
    update_desire_locked();
    dirty_ = true;
}

esp_err_t PetLogic::feed_success(float hunger_delta, float happiness_delta) {
    if (!nonnegative(hunger_delta) || !nonnegative(happiness_delta)) { return ESP_ERR_INVALID_ARG; }
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!state_.alive) { return finish_locked(now, ESP_ERR_INVALID_STATE); }
    feed_success_locked(hunger_delta, happiness_delta);
    return finish_locked(now);
}

esp_err_t PetLogic::feed_wrong(float happiness_delta) {
    if (!nonnegative(happiness_delta)) { return ESP_ERR_INVALID_ARG; }
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!state_.alive) { return finish_locked(now, ESP_ERR_INVALID_STATE); }
    feed_wrong_locked(happiness_delta);
    return finish_locked(now);
}

esp_err_t PetLogic::give_affection(float happiness_boost) {
    if (!nonnegative(happiness_boost)) { return ESP_ERR_INVALID_ARG; }
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!state_.alive) { return finish_locked(now, ESP_ERR_INVALID_STATE); }
    state_.happiness = bar(static_cast<double>(state_.happiness) + happiness_boost);
    state_.time_since_affection_seconds = state_.time_since_interaction_seconds = 0.0;
    if (state_.mood == PetMood::LONELY || state_.mood == PetMood::SAD) { state_.mood = PetMood::HAPPY; }
    increment(state_.affection_count);
    update_desire_locked();
    dirty_ = true;
    // Physical affection always counts, but repeated touches do not spam speech.
    const esp_err_t error = can_respond_locked(now) ?
        speak_locked(kPat, count_of(kPat), PetEmotion::HAPPY, 85, now) : ESP_OK;
    return finish_locked(now, error);
}

esp_err_t PetLogic::rest(float energy_boost) {
    if (!nonnegative(energy_boost)) { return ESP_ERR_INVALID_ARG; }
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!state_.alive) { return finish_locked(now, ESP_ERR_INVALID_STATE); }
    state_.energy = bar(static_cast<double>(state_.energy) + energy_boost);
    state_.time_since_interaction_seconds = 0.0;
    if (state_.mood == PetMood::SLEEPY && state_.energy >= 25.0F) {
        state_.mood = state_.hunger >= 50.0F ? PetMood::HUNGRY : PetMood::HAPPY;
    }
    update_desire_locked();
    dirty_ = true;
    return finish_locked(now);
}

esp_err_t PetLogic::handle_confirmed_food(const char* detected_food, BehaviorOutcome& outcome) {
    outcome = BehaviorOutcome::IGNORED_NO_FOOD;
    if (!detected_food || std::strlen(detected_food) >= sizeof(PetLogicEventData{}.detected_food)) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    PetLogicEventData event{};
    copy_text(event.detected_food, detected_food);
    copy_text(event.requested_food, state_.requested_food_id);
    char previous_name[48]{};
    copy_text(previous_name, state_.requested_food_name);
    bool speak = false;
    if (!state_.alive) { outcome = BehaviorOutcome::IGNORED_DEAD; }
    else if (!food_count_ || equals_query(detected_food, "")) {
        outcome = BehaviorOutcome::IGNORED_NO_FOOD;
    } else if (!can_respond_locked(now)) { outcome = BehaviorOutcome::IGNORED_COOLDOWN; }
    else {
        speak = true;
        if (matches_food_locked(detected_food)) {
            outcome = BehaviorOutcome::CORRECT;
            feed_success_locked(30.0F, 20.0F);
            select_food_locked(true);
            hunger_reminder_count_ = 0;
            pending_announcement_us_ = now + 4 * kSecond;
        } else {
            outcome = BehaviorOutcome::WRONG;
            feed_wrong_locked(3.0F); // PetBehaviorEngine differs from PetState's default 5.
        }
        // Accepted detections consume cooldown even if the event queue is full.
        mark_speech_locked(now);
    }
    event.outcome = outcome;
    esp_err_t error = publish_locked(PET_EVENT_FOOD_EVALUATED, &event);
    if (speak) {
        const esp_err_t spoken = outcome == BehaviorOutcome::CORRECT ?
            speak_locked(kCorrect, count_of(kCorrect), PetEmotion::HAPPY, 100, now) :
            speak_locked(kWrong, count_of(kWrong), PetEmotion::CONFUSED, 80, now, previous_name);
        error = first_error(error, spoken);
    }
    return finish_locked(now, error);
}

esp_err_t PetLogic::announce_startup() {
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!state_.alive) { return finish_locked(now, ESP_ERR_INVALID_STATE); }
    const esp_err_t error = speak_locked(kStartup, count_of(kStartup), PetEmotion::HAPPY, 90, now);
    if (error == ESP_OK) { pending_announcement_us_ = now + 3500000; }
    return finish_locked(now, error);
}

esp_err_t PetLogic::schedule_initial_desire_announcement(float delay_seconds) {
    // Bound conversion to int64_t and keep malformed inputs out of timer arithmetic.
    if (!nonnegative(delay_seconds) || delay_seconds > 86400.0F) { return ESP_ERR_INVALID_ARG; }
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (!state_.alive) { return finish_locked(now, ESP_ERR_INVALID_STATE); }
    pending_announcement_us_ = now + static_cast<int64_t>(delay_seconds * kSecond);
    return finish_locked(now);
}

esp_err_t PetLogic::save_memory() {
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    advance_locked(now);
    if (dead_memory_erased_) { return ESP_ERR_INVALID_STATE; }
    return finish_locked(now, ESP_OK, true);
}

esp_err_t PetLogic::erase_memory(bool restart_pet) {
    const esp_err_t context = check_mutation_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    if (!restart_pet && state_.alive) { return ESP_ERR_INVALID_STATE; }
    esp_err_t error = nvs_erase_all(nvs_);
    if (error == ESP_OK) { error = nvs_commit(nvs_); }
    if (error != ESP_OK) { return error; }
    const int64_t now = esp_timer_get_time();
    if (restart_pet) { reset_state_locked(now); }
    else {
        dead_memory_erased_ = true;
        dirty_ = death_save_pending_ = false;
    }
    error = publish_locked(PET_EVENT_MEMORY_ERASED);
    return finish_locked(now, error);
}
