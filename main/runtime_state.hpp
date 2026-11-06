#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "led_matrix.hpp"
#include "pet_logic.hpp"

namespace pely {
struct VisionInput {
    int64_t observed_us = 0;
    uint32_t sequence = 0;
    float confidence = 0;
    char food_id[32] = {}; // Empty input clears the candidate after missing frames.
    bool frame_valid = false; // A camera/inference failure is not proof the card left view.
};

// A presented card is consumed once. It can be retried while the pet's response
// cooldown is active, but an accepted card must leave the frame or change first.
class VisionGate final {
public:
    bool observe(const VisionInput& input, int64_t now, uint32_t required_frames,
                 float threshold, int64_t maximum_age_us) {
        const bool gap = have_sequence_ && input.sequence != last_sequence_ + uint32_t{1};
        last_sequence_ = input.sequence;
        have_sequence_ = true;
        if (gap) { candidate_[0] = '\0'; consecutive_ = missing_ = 0; }
        if (!input.frame_valid || input.observed_us < 0 || now < input.observed_us || now - input.observed_us > maximum_age_us) {
            candidate_[0] = '\0'; consecutive_ = missing_ = 0;
            return false; // Stale frames are not proof that a consumed card left view.
        }
        const bool valid = std::isfinite(input.confidence) &&
            input.confidence >= threshold && input.confidence <= 1.0F && input.food_id[0] &&
            std::memchr(input.food_id, '\0', sizeof(input.food_id));
        if (!valid) {
            candidate_[0] = '\0'; consecutive_ = 0;
            if (missing_ < 3) { ++missing_; }
            if (missing_ >= 3) { consumed_[0] = '\0'; }
            return false;
        }
        missing_ = 0;
        if (std::strcmp(candidate_, input.food_id) != 0) {
            std::memcpy(candidate_, input.food_id, sizeof(candidate_));
            consecutive_ = 1;
        } else if (consecutive_ < required_frames) { ++consecutive_; }
        return consecutive_ >= required_frames && std::strcmp(consumed_, input.food_id) != 0;
    }
    void consume(const char* id) {
        std::strncpy(consumed_, id, sizeof(consumed_) - 1);
        consumed_[sizeof(consumed_) - 1] = '\0';
    }
    void reset() { *this = VisionGate{}; }
private:
    char candidate_[32] = {}, consumed_[32] = {};
    uint32_t last_sequence_ = 0, consecutive_ = 0, missing_ = 0;
    bool have_sequence_ = false;
};

inline Emotion mood_emotion(PetMood mood) {
    switch (mood) {
    case PetMood::HAPPY: return Emotion::HAPPY;
    case PetMood::SAD: return Emotion::SAD;
    case PetMood::LONELY: return Emotion::LONELY;
    case PetMood::SLEEPY: return Emotion::SLEEPY;
    case PetMood::DEAD: return Emotion::DEAD;
    case PetMood::HUNGRY: return Emotion::HUNGRY;
    }
    return Emotion::IDLE;
}

struct UiState {
    Emotion base = Emotion::IDLE;
    Emotion reaction = Emotion::IDLE;
    uint8_t priority = 0;
    int64_t reaction_until_us = 0;
    bool dead = false;

    void set_pet(const PetSnapshot& state) {
        dead = !state.alive;
        base = dead ? Emotion::DEAD : mood_emotion(state.mood);
        if (dead) { reaction_until_us = 0; priority = 0; }
    }
    void react(Emotion emotion, uint8_t new_priority, uint32_t duration_ms, int64_t now) {
        if (dead || (now < reaction_until_us && new_priority < priority)) { return; }
        reaction = emotion;
        priority = new_priority;
        reaction_until_us = now + duration_ms * int64_t{1000};
    }
    Emotion effective(int64_t now) const {
        return dead ? Emotion::DEAD : (now < reaction_until_us ? reaction : base);
    }
};
} // namespace pely
