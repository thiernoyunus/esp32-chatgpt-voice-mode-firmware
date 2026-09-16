#ifndef VOICE_READINESS_H
#define VOICE_READINESS_H

#include <atomic>
#include <cstdint>
#include <string>

/*
 * Five separate things have to happen before a spoken reply can be heard, and
 * treating them as one is what made silent calls so hard to explain: the call
 * reported itself open and ready while the audio path was still missing a link.
 * Each stage below is reachable and checkable on its own, so a call that ends up
 * silent can say which link never arrived instead of only that no sound came
 * out.
 *
 * They are recorded in the order a healthy call reaches them, which is also the
 * order worth checking when one is missing.
 */
enum VoiceStage : uint32_t {
    /* The WebRTC transport itself came up: candidates paired and the peer
     * answered, so there is somewhere for audio to travel. */
    kVoiceStagePeerConnected = 1u << 0,
    /* The server has a remote audio track to send down: without this, no amount
     * of healthy signalling produces sound. */
    kVoiceStageAudioTrack = 1u << 1,
    /* The event channel carries the conversation's events - captions, turns,
     * speaking state. It can be open while the audio track is not, which is the
     * classic "captions move, speaker is silent" case. */
    kVoiceStageEventChannel = 1u << 2,
    /* Real traffic arrived on that channel, so the voice session behind it is
     * genuinely live rather than merely connected. */
    kVoiceStageSessionStarted = 1u << 3,
    /* The device accepted a frame into its own playback pipeline. Reaching this
     * is the last thing the device can check for itself; whether the speaker
     * made a sound needs a person standing there. */
    kVoiceStagePlaybackAdmitted = 1u << 4,
};

constexpr int kVoiceStageCount = 5;
constexpr uint32_t kVoiceStageAll = (1u << kVoiceStageCount) - 1;

/* Short names, for one log line rather than five. */
inline const char* VoiceStageName(uint32_t stage) {
    switch (stage) {
        case kVoiceStagePeerConnected:
            return "peer";
        case kVoiceStageAudioTrack:
            return "audio-track";
        case kVoiceStageEventChannel:
            return "event-channel";
        case kVoiceStageSessionStarted:
            return "session";
        case kVoiceStagePlaybackAdmitted:
            return "playback";
        default:
            return "unknown";
    }
}

/* e.g. "peer,event-channel" - what has been reached so far. */
inline std::string DescribeVoiceStages(uint32_t mask) {
    std::string description;
    for (int index = 0; index < kVoiceStageCount; ++index) {
        const uint32_t stage = 1u << index;
        if ((mask & stage) == 0) {
            continue;
        }
        if (!description.empty()) {
            description += ",";
        }
        description += VoiceStageName(stage);
    }
    return description.empty() ? "none" : description;
}

/* The earliest stage still missing, as a single bit, or 0 when the call has
 * reached every one of them. */
inline uint32_t FirstMissingVoiceStage(uint32_t mask) {
    for (int index = 0; index < kVoiceStageCount; ++index) {
        const uint32_t stage = 1u << index;
        if ((mask & stage) == 0) {
            return stage;
        }
    }
    return 0;
}

/* Written from the WebRTC callback task and read from the main task, so every
 * touch goes through the atomic mask. */
class VoiceReadiness {
public:
    /* True the first time this stage is reached, and only then, so each stage
     * earns exactly one log line per call however often it is reported. */
    bool Mark(uint32_t stage) {
        const uint32_t previous = mask_.fetch_or(stage);
        return (previous & stage) == 0;
    }

    uint32_t Mask() const { return mask_.load(); }
    uint32_t FirstMissing() const { return FirstMissingVoiceStage(mask_.load()); }
    std::string Describe() const { return DescribeVoiceStages(mask_.load()); }

    void Reset() { mask_.store(0); }

private:
    std::atomic<uint32_t> mask_{0};
};

#endif  // VOICE_READINESS_H
