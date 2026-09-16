#!/usr/bin/env python3
"""Run production voice callbacks on the host: python3 scripts/tests/test_voice_close.py."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    source = tests.parents[1] / "main"
    app = (source / "application.cc").read_text()
    voice = (source / "protocols/codex_voice_protocol.cc").read_text()
    audio = (source / "audio/audio_service.cc").read_text()
    audio_header = (source / "audio/audio_service.h").read_text()
    preroll_header = (source / "audio/voice_preroll.h").read_text()
    readiness_header = (source / "protocols/voice_readiness.h").read_text()
    continuing = app[app.index("void Application::ContinueOpenAudioChannel("):
                     app.index("void Application::HandleStartListeningEvent()")]
    gain = audio[audio.index("namespace {"):audio.index("#define RATE_CVT_CFG")]
    frame = audio_header[audio_header.index("#define OPUS_FRAME_DURATION_MS"):
                         audio_header.index("#define MAX_ENCODE_TASKS_IN_QUEUE")]
    incoming = app[app.index("protocol_->OnIncomingAudio("):app.index("protocol_->OnAudioChannelOpened(")]
    # Copy whole live definitions, including nested lambdas; never duplicate their logic.
    closed = app[app.index("protocol_->OnAudioChannelClosed("):
                 app.index("protocol_->OnIncomingJson(", app.index("protocol_->OnAudioChannelClosed("))]
    opened = voice[voice.index("int CodexVoiceProtocol::OnDataChannelOpen("):
                   voice.index("int CodexVoiceProtocol::OnPeerData(")]
    harness = r'''
#include <algorithm>
#include <cstdint>
#include <memory>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#define TAG "Application"
#define ESP_LOGW(...) do {} while (0)
static inline int64_t esp_timer_get_time() { return 0; }
GAIN_CODE
FRAME_CODE
VOICE_PREROLL_CODE
VOICE_READINESS_CODE
static_assert(OPUS_FRAME_DURATION_MS == 20);
struct AudioStreamPacket {};
struct AudioService {
    int accepted = 0, resets = 0;
    /* The speaker queue refuses frames when it is full; whoever pushes has to
     * look at the answer. */
    bool full = false;
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket>) {
        if (full) return false;
        ++accepted;
        return true;
    }
    void ResetDecoder() { ++resets; }
};

enum class PowerSaveLevel { PERFORMANCE, LOW_POWER };
enum DeviceState { kDeviceStateIdle, kDeviceStateListening, kDeviceStateSpeaking, kDeviceStateConnecting };
using ListeningMode = int;
struct Display {
    int voice_shown = 0;
    void ShowVoicePage() { ++voice_shown; }
    std::string chat = "reply";
    void SetChatMessage(const char*, const char* text) { chat = text; }
};
struct Board {
    PowerSaveLevel power = PowerSaveLevel::PERFORMANCE;
    int writes = 0;
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
    void SetPowerSaveLevel(PowerSaveLevel level) { power = level; ++writes; }
};
struct Protocol {
    std::function<void()> during_open;
    bool OpenAudioChannel() { opened = true; if (during_open) during_open(); return true; }
    void CloseAudioChannel() { opened = false; }
    bool opened = false;
    std::function<void()> closed;
    std::function<void(std::unique_ptr<AudioStreamPacket>)> incoming;
    bool IsAudioChannelOpened() const { return opened; }
    void MarkPlaybackAdmitted() { ++admitted; }
    int admitted = 0;
    void OnAudioChannelClosed(std::function<void()> callback) { closed = callback; }
    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket>)> callback) { incoming = callback; }
};
struct Application {
    std::atomic<bool> call_end_requested_{false};
    void ContinueOpenAudioChannel(ListeningMode mode);
    void SetListeningMode(ListeningMode) { state = kDeviceStateListening; }
    AudioService audio_service_;
    VoicePreroll<std::unique_ptr<AudioStreamPacket>> voice_preroll_{kVoicePrerollFrames};
    Protocol storage;
    Protocol* protocol_ = &storage;
    DeviceState state = kDeviceStateSpeaking;
    int state_writes = 0, dismissals = 0;
    std::vector<std::function<void()>> queue;
    void Schedule(std::function<void()> callback) { queue.push_back(callback); }
    DeviceState GetDeviceState() const { return state; }
    void SetDeviceState(DeviceState next) { state = next; ++state_writes; }
    void DismissConfirm() { ++dismissals; }
    void Register() {
        auto& board = Board::GetInstance();
        APP_CALLBACK
    }
};
using EventBits_t = unsigned;
constexpr EventBits_t kVoiceReadyBit = 1, kVoiceFailedBit = 2;
EventBits_t xEventGroupGetBits(EventBits_t* events) { return *events; }
EventBits_t xEventGroupSetBits(EventBits_t* events, EventBits_t bits) { return *events |= bits; }
struct esp_peer_data_channel_info_t { const char* label; };
struct CodexVoiceProtocol {
    std::atomic<bool> closing_{false}, channel_open_{false};
    EventBits_t events = 0;
    EventBits_t* peer_events_ = &events;
    VoiceReadiness readiness_;
    void MarkStage(uint32_t stage) { readiness_.Mark(stage); }
    static int OnDataChannelOpen(esp_peer_data_channel_info_t*, void*);
};
OPEN_CALLBACK
CONTINUE_CALLBACK

int main() {
    assert(CodexVoiceLevel({}) == 0);
    assert(CodexVoiceLevel({0, 0, 0}) == 0);
    assert(CodexVoiceLevel({128, -128}) == 0);
    assert(CodexVoiceLevel({32767, -32768}) == 100);
    assert(CodexVoiceLevel({500, -500}) < CodexVoiceLevel({1500, -1500}));
    // Close notification arrives first; replacement opens before queued work runs.
    for (int scenario = 0; scenario < 3; ++scenario) {
        auto& board = Board::GetInstance();
        board = Board{};
        Application app;
        app.Register();
        app.storage.closed();
        assert(app.queue.size() == 1);
        assert(app.state == kDeviceStateSpeaking && app.state_writes == 0);
        assert(board.writes == 0 && app.dismissals == 0 && board.display.chat == "reply");
        if (scenario == 0) {
            app.storage.opened = true;
            app.state = kDeviceStateListening;
        } else if (scenario == 2) {
            app.state = kDeviceStateIdle;
            board.display.chat = "Connection failed";
        }
        app.queue.front()();
        if (scenario == 0) {
            assert(app.state == kDeviceStateListening && app.state_writes == 0);
            assert(board.power == PowerSaveLevel::PERFORMANCE && board.writes == 0);
            assert(app.dismissals == 0 && board.display.chat == "reply");
        } else {
            assert(app.state == kDeviceStateIdle && board.power == PowerSaveLevel::LOW_POWER);
            assert(app.state_writes == (scenario == 1 ? 1 : 0));
            assert(board.display.chat == (scenario == 1 ? "" : "Connection failed"));
        }
    }
    std::cout << "PASS: queued close, replacement survives, genuine close, error chat preserved\n";
    for (bool closing : {false, true}) {
        for (bool failed : {false, true}) {
            for (const char* label : {"oai-events", "other", static_cast<const char*>(nullptr)}) {
                for (bool null_channel : {false, true}) {
                    CodexVoiceProtocol protocol;
                    protocol.closing_ = closing;
                    protocol.events = failed ? kVoiceFailedBit : 0;
                    esp_peer_data_channel_info_t channel{label};
                    assert(CodexVoiceProtocol::OnDataChannelOpen(
                        null_channel ? nullptr : &channel, &protocol) == 0);
                    bool ready = !closing && !failed && !null_channel && label &&
                                 std::strcmp(label, "oai-events") == 0;
                    assert(protocol.channel_open_ == ready);
                    assert(protocol.events == ((failed ? kVoiceFailedBit : 0) |
                                               (ready ? kVoiceReadyBit : 0)));
                }
            }
        }
    }
    std::cout << "PASS: valid open succeeds; closed, failed, and invalid opens stay unready\n";
}
'''.replace("APP_CALLBACK", closed + incoming).replace("OPEN_CALLBACK", opened).replace("CONTINUE_CALLBACK", continuing).replace("GAIN_CODE", gain).replace("FRAME_CODE", frame)
    harness = harness.replace("VOICE_PREROLL_CODE", preroll_header)
    harness = harness.replace("VOICE_READINESS_CODE", readiness_header)
    harness = harness.replace("int main() {", r'''int main() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        Application app;
        app.state = kDeviceStateConnecting;
        if (scenario == 0) app.call_end_requested_ = true;
        if (scenario == 1) app.storage.during_open = [&] { app.call_end_requested_ = true; };
        app.ContinueOpenAudioChannel(0);
        assert(app.storage.opened == (scenario == 2));
        assert((app.state == kDeviceStateListening) == (scenario == 2));
    }
    for (bool open : {false, true}) {
        for (auto state : {kDeviceStateIdle, kDeviceStateConnecting, kDeviceStateListening,
                           kDeviceStateSpeaking}) {
            Application app;
            app.Register();
            app.storage.opened = open;
            app.state = state;
            app.storage.incoming(std::make_unique<AudioStreamPacket>());
            const bool speaking = state == kDeviceStateListening || state == kDeviceStateSpeaking;
            const bool connecting = state == kDeviceStateConnecting;
            assert(app.audio_service_.accepted == (open && speaking ? 1 : 0));
            /* The opening of a call arrives while the device is still
             * connecting, and starting to listen clears the speaker queues.
             * Those frames have to be held, not dropped. */
            assert(app.voice_preroll_.Size() == (open && connecting ? 1 : 0));
            assert(!open || !speaking || app.storage.admitted == 1);
        }
    }
    /* A caller that ends before it starts leaves nothing to speak into the
     * next one. */
    {
        Application app;
        app.Register();
        app.storage.opened = true;
        app.state = kDeviceStateConnecting;
        app.storage.incoming(std::make_unique<AudioStreamPacket>());
        assert(app.voice_preroll_.Size() == 1);
        app.voice_preroll_.Clear();
        assert(app.voice_preroll_.Empty());
    }
    /* A full speaker queue is reported as a dropped frame, not as success. */
    {
        Application app;
        app.Register();
        app.storage.opened = true;
        app.state = kDeviceStateListening;
        app.audio_service_.full = true;
        app.storage.incoming(std::make_unique<AudioStreamPacket>());
        assert(app.audio_service_.accepted == 0 && app.storage.admitted == 0);
    }
    std::cout << "PASS: live audio plays without captions; closed calls stay silent; gain and framing checked\n";
''')
    with tempfile.TemporaryDirectory(prefix="voice-close-", dir=tests) as work:
        cpp = Path(work) / "check.cc"
        binary = Path(work) / "check"
        cpp.write_text(harness)
        subprocess.run(shlex.split(os.environ.get("CXX", "c++")) +
                       ["-std=c++17", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
