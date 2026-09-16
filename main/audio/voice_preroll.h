#ifndef VOICE_PREROLL_H
#define VOICE_PREROLL_H

#include <cstddef>
#include <deque>
#include <utility>

/*
 * The first words of a call arrive before the device can play them.
 *
 * A call is set up in this order: the WebRTC answer lands, the event channel
 * opens, and only then does the device start listening - and starting to listen
 * clears the speaker queues on purpose, so a previous call's leftover audio
 * cannot play over the new one. The assistant's greeting is sent the moment the
 * session opens, which is exactly inside that window, so its first frames were
 * thrown away twice over: once because the device was still connecting, and
 * again by the clear that follows. The user hears a reply that starts in the
 * middle, or hears nothing at all.
 *
 * This holds those frames outside the audio queues, where the clear cannot
 * reach them, until the device is actually listening. It is deliberately
 * bounded and keeps the earliest frames: they are the beginning of the reply,
 * and the window it has to survive is shorter than a second.
 */
/* How long a call may take to start listening before the held opening is
 * reported rather than quietly grown. A greeting is a second or two of speech,
 * and the gap it has to survive is the one between the voice channel opening
 * and the microphone starting. */
constexpr size_t kVoicePrerollFrames = 50;

template <typename Packet>
class VoicePreroll {
public:
    explicit VoicePreroll(size_t capacity) : capacity_(capacity) {}

    /* Returns false when the buffer is already full, which means the call took
     * far longer to start listening than a greeting is long - worth reporting,
     * never worth growing for. */
    bool Push(Packet packet) {
        if (frames_.size() >= capacity_) {
            ++dropped_;
            return false;
        }
        frames_.push_back(std::move(packet));
        return true;
    }

    /* Hands every held frame to the caller in the order it arrived and empties
     * the buffer. A sink that refuses frames (a full speaker queue) does not
     * block the rest: the held frames are already old. */
    template <typename Sink>
    size_t Flush(Sink&& sink) {
        std::deque<Packet> pending;
        pending.swap(frames_);
        size_t delivered = 0;
        for (auto& frame : pending) {
            if (sink(std::move(frame))) {
                ++delivered;
            }
        }
        return delivered;
    }

    bool Empty() const { return frames_.empty(); }
    size_t Size() const { return frames_.size(); }
    /* Frames refused because the buffer was full; reset with the buffer. */
    size_t Dropped() const { return dropped_; }

    void Clear() {
        frames_.clear();
        dropped_ = 0;
    }

private:
    size_t capacity_;
    std::deque<Packet> frames_;
    size_t dropped_ = 0;
};

#endif  // VOICE_PREROLL_H
