#!/usr/bin/env python3
"""Check production mute queue handling without a device."""
from pathlib import Path
import subprocess
import tempfile


def main():
    main_path = Path(__file__).resolve().parents[2] / "main"
    source = (main_path / "audio/audio_service.cc").read_text()
    application = (main_path / "application.cc").read_text()
    # OnVoiceTouchRelease used to sit inside #ifdef CONFIG_VOICEMODE_CODEX_VOICE;
    # that wrapper is gone, so stop at the next Application method instead.
    touch_start = application.index("void Application::OnVoiceTouchRelease(")
    touch = application[touch_start:application.index("\nvoid Application::ShowConfirm(", touch_start)]
    mute = source[source.index("void AudioService::SetMicrophoneMuted("):
                  source.index("void AudioService::EncodeWakeWord(")]
    guard = source[source.index("                        if (microphone_muted_ ||"):
                   source.index("                        /* Never let a full send queue")]
    program = r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include "display/voice_geometry.h"
enum { kAudioTaskTypeEncodeToSendQueue, kAudioTaskTypeEncodeToTestingQueue };
struct Task { int type; };
struct AudioService {
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::atomic<bool> microphone_muted_{false};
    uint32_t microphone_generation_ = 0;
    std::deque<int> audio_send_queue_;
    std::deque<std::unique_ptr<Task>> audio_encode_queue_;
    std::deque<int> audio_playback_queue_{7,8};
    void SetMicrophoneMuted(bool muted);
    bool IsMicrophoneMuted() const { return microphone_muted_; }
    bool Publish(uint32_t microphone_generation) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        for (int attempt = 0; attempt < 1; ++attempt) {
            lock.unlock();
            std::unique_lock<std::mutex> lock2(audio_queue_mutex_);
            GUARD
            audio_send_queue_.push_back(1);
            return true;
        }
        return false;
    }
};
MUTE
struct Display {
    bool muted = false;
    bool picker = false;
    void SetVoiceMicrophoneMuted(bool value) { muted = value; }
    void ShowVoiceModels(const std::vector<std::string>&, size_t) { picker = true; }
    void HideVoiceModels() { picker = false; }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
enum { kDeviceStateIdle = 0 };
struct Protocol {
    bool opened = true;
    bool IsAudioChannelOpened() { return opened; }
    void CloseAudioChannel() { opened = false; ++closes; }
    int closes = 0;
};
struct CodexVoiceProtocol : Protocol {
    struct Choice { std::string name; };
    std::vector<Choice> models{{"Astra"}, {"Opus"}, {"Sonnet"}, {"MiniMax"}};
    size_t selected = 0;
    const auto& GetModels() { return models; }
    bool SelectModel(size_t index) { if (index >= models.size()) return false; selected = index; return true; }
};
struct Application {
    bool is_screen_asleep_ = false, confirm = false;
    int toggles = 0;
    std::unique_ptr<CodexVoiceProtocol> protocol_ = std::make_unique<CodexVoiceProtocol>();
    bool voice_model_picker_open_ = false;
    size_t voice_model_page_ = 0;
    std::atomic<bool> call_end_requested_{false};
    int state_ = 0;
    AudioService audio_service_;
    template<class F> void Schedule(F function) { function(); }
    void NoteUserActivity() { is_screen_asleep_ = false; }
    bool IsConfirmActive() { return confirm; }
    void HandleToggleChatEvent() { ++toggles; }
    void SetDeviceState(int state) { state_ = state; }
    void OnVoiceTouchRelease(int x, int y);
};
TOUCH
int main() {
    AudioService audio;
    assert(audio.Publish(0));
    audio.audio_encode_queue_.push_back(std::make_unique<Task>(Task{kAudioTaskTypeEncodeToSendQueue}));
    audio.audio_encode_queue_.push_back(std::make_unique<Task>(Task{kAudioTaskTypeEncodeToTestingQueue}));
    audio.SetMicrophoneMuted(true);
    assert(audio.audio_send_queue_.empty());
    assert(audio.audio_encode_queue_.size() == 1);
    assert(audio.audio_encode_queue_[0]->type == kAudioTaskTypeEncodeToTestingQueue);
    assert(!audio.Publish(0));
    assert(!audio.Publish(1));
    audio.SetMicrophoneMuted(true);
    assert(audio.microphone_generation_ == 1);
    audio.SetMicrophoneMuted(false);
    assert(!audio.Publish(0));
    assert(!audio.Publish(1));
    assert(audio.Publish(2));
    assert((audio.audio_playback_queue_ == std::deque<int>{7,8}));
    Application app;
    app.OnVoiceTouchRelease(88, 272);
    assert(app.audio_service_.IsMicrophoneMuted());
    assert(Board::GetInstance().display.muted);
    assert(app.toggles == 0);
    app.OnVoiceTouchRelease(180, 180);
    assert(app.toggles == 0);
    app.OnVoiceTouchRelease(180, 40);
    assert(app.toggles == 0);
    assert(!app.voice_model_picker_open_);
    app.OnVoiceTouchRelease(88, 272);
    assert(!app.audio_service_.IsMicrophoneMuted());
    app.OnVoiceTouchRelease(272, 272);
    // End closes the call itself; it must not route through the toggle path,
    // which only aborts speech and would leave the channel open.
    assert(app.toggles == 0);
    assert(app.protocol_->closes == 1 && !app.protocol_->opened);
    assert(app.call_end_requested_.load());
    app.is_screen_asleep_ = true;
    app.OnVoiceTouchRelease(272, 272);
    assert(app.protocol_->closes == 1);
    app.protocol_->opened = true;
    app.confirm = true;
    app.OnVoiceTouchRelease(88, 272);
    assert(!app.audio_service_.IsMicrophoneMuted());
    app.confirm = false;
    // Closed channel: any touch falls through to HandleToggleChatEvent().
    // Mute/end do not fire while idle, so the mic and toggles move together.
    app.protocol_->opened = false;
    app.OnVoiceTouchRelease(88, 272);
    assert(!app.audio_service_.IsMicrophoneMuted() && app.toggles == 1);
    app.OnVoiceTouchRelease(180, 180);
    assert(app.toggles == 2);
    // Back on the call: mute/end respond again, tapping outside the buttons
    // is a no-op (no model picker was ever installed at 180,40).
    app.protocol_->opened = true;
    app.OnVoiceTouchRelease(88, 272);
    assert(app.audio_service_.IsMicrophoneMuted());
    assert(app.toggles == 2);
    app.OnVoiceTouchRelease(272, 272);
    assert(app.toggles == 2 && app.protocol_->closes == 2);
    app.protocol_->opened = true;
    app.OnVoiceTouchRelease(180, 40);
    assert(app.toggles == 2);
    assert(!app.voice_model_picker_open_);
}
'''.replace("MUTE", mute).replace("GUARD", guard).replace("TOUCH", touch)
    with tempfile.TemporaryDirectory(prefix="voicemode-mute-test-") as directory:
        path = Path(directory)
        (path / "test.cc").write_text(program)
        subprocess.run(["c++", "-std=c++17", "-pthread", "-I", str(main_path), str(path / "test.cc"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True, timeout=5)
    print("PASS: mute clears microphone queues, rejects in-flight old audio, and preserves playback")


if __name__ == "__main__":
    main()
