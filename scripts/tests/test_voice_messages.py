#!/usr/bin/env python3
"""Exercise the actual voice-message parser with the firmware's JSON library."""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    source = (root / "main/protocols/codex_voice_protocol.cc").read_text()
    handler = source[source.index("void CodexVoiceProtocol::HandleSignal("):
                     source.index("void CodexVoiceProtocol::StartSpeaking(")]
    program = r'''
#include "cJSON.h"
#include "voice_readiness.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#define ESP_LOGI(...)
#define ESP_LOGD(...)
std::string ReadErrorMessage(cJSON*) { return "error"; }
constexpr int ESP_PEER_MSG_TYPE_SDP = 0, ESP_PEER_ERR_NONE = 0;
struct esp_peer_msg_t { int type; uint8_t* data; int size; };
int esp_peer_send_msg(void*, esp_peer_msg_t*) { return 0; }
struct Display {
    std::string model, activity, icon, pixels;
    void SetVoiceModel(const char* value) { model = value; }
    void SetVoiceActivity(const char* value, const char* symbol, const char* image = nullptr) {
        activity = value; icon = symbol; pixels = image ? image : "";
    }
};
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
struct Application {
    struct Audio { int resets = 0; void ResetDecoder() { ++resets; } } audio;
    Audio& GetAudioService() { return audio; }
    std::vector<std::function<void()>> queue;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> function) { queue.push_back(std::move(function)); }
    void Drain() { auto pending = std::move(queue); queue.clear(); for (auto& call : pending) call(); }
};
/* The parser saves a chosen chat and reads the clock; neither is what this
 * test is about, so both are recorded and ignored. */
struct Settings {
    Settings(const char*, bool) {}
    std::string GetString(const char*, const char* fallback = "") { return fallback; }
    void SetString(const char*, const std::string&) {}
};
uint32_t NowMilliseconds() { return 0; }
bool AtOrAfter(uint32_t sample, uint32_t since) {
    return static_cast<int32_t>(sample - since) >= 0;
}
struct CodexVoiceProtocol {
    struct ModelChoice { std::string id, name; };
    struct ChatChoice { std::string id, name; };
    std::vector<ModelChoice> models_{{"", "Default"}};
    std::vector<ChatChoice> chats_;
    std::string request_id_ = "current", error;
    /* Written by the parser when a reply is expected; read by the watchdog. */
    std::atomic<uint32_t> last_audio_frame_ms_{0}, speech_expected_since_ms_{0};
    std::string transcript_partial_, transcript_role_;
    uint32_t transcript_emitted_at_ = 0;
    void* peer_ = nullptr;
    bool opened = true;
    int speaking = 0;
    std::function<void(const cJSON*)> on_incoming_json_;
    bool IsAudioChannelOpened() { return opened; }
    void Fail(const std::string& value) { error = value; }
    void StartSpeaking() { ++speaking; }
    void StopSpeaking() { speaking = 0; }
    void EmitTranscript(const char*, const char*) {}
    void StreamTranscript(const char*, const char*) {}
    void MarkStage(uint32_t) {}
    void HandleSignal(const char*, size_t);
    void HandleRealtimeEvent(const uint8_t*, size_t);
    void Event(const std::string& value) { HandleRealtimeEvent(reinterpret_cast<const uint8_t*>(value.data()), value.size()); }
    void Receive(const std::string& value) { HandleSignal(value.data(), value.size()); }
};
HANDLER
int main() {
    CodexVoiceProtocol voice;
    int tool_requests = 0;
    voice.on_incoming_json_ = [&](const cJSON*) { ++tool_requests; };
    voice.Receive(R"({"type":"mcp","payload":{"jsonrpc":"2.0","id":1,"method":"tools/call"}})");
    voice.Receive(R"({"type":"mcp","payload":[]})");
    assert(tool_requests == 1);
    voice.on_incoming_json_ = nullptr;
    voice.Receive(R"({"type":"mcp","payload":{}})");
    auto& app = Application::GetInstance();
    auto& display = Board::GetInstance().display;
    voice.Receive(R"({"type":"realtime_answer","requestId":"current","models":[{"id":"opus","name":"Opus 5"},{"id":7,"name":"invalid"}],"selectedModel":"opus"})");
    assert(voice.models_.size() == 1);
    app.Drain();
    assert(voice.models_.size() == 2 && display.model == "Opus 5");
    voice.Receive(R"({"type":"realtime_status","requestId":"current","caption":"Searching the web"})");
    app.Drain();
    assert(display.activity == "Searching the web" && voice.speaking == 0);
    voice.Receive(R"({"type":"realtime_status","requestId":"current","caption":"Search commits","icon":"github"})");
    app.Drain();
    assert(display.icon == "none");
    const std::string image(3072, 'A');
    voice.Receive((std::string(R"({"type":"realtime_status","requestId":"current","caption":"Plugin action","iconPixels":")") + image + R"("})").c_str());
    app.Drain();
    assert(display.pixels == image);
    voice.Receive(R"({"type":"realtime_status","requestId":"current","caption":"Plugin action","iconPixels":"too short"})");
    app.Drain();
    assert(display.pixels.empty());
    voice.Receive(R"({"type":"realtime_status","requestId":"current","caption":"Searching the web","icon":"bad"})");
    app.Drain();
    assert(display.icon == "none");
    voice.Receive(R"({"type":"realtime_status","requestId":"old","caption":"stale"})");
    app.Drain();
    assert(display.activity == "Searching the web");
    voice.Receive(R"({"type":"realtime_status","requestId":"current","caption":"late"})");
    voice.request_id_ = "replacement";
    app.Drain();
    assert(display.activity == "Searching the web");
    voice.Receive(R"({"type":"realtime_status","requestId":"replacement","caption":"closed"})");
    voice.opened = false;
    app.Drain();
    assert(display.activity == "Searching the web");
    voice.Receive("not json");
    voice.Receive(R"({"type":[],"requestId":"replacement"})");
    assert(voice.error.empty());
    voice.opened = true;
    voice.speaking = 1;
    voice.Event(R"({"type":"turn.created","turn":{"role":"assistant"}})");
    voice.Event(R"({"type":"turn.created","turn":{"role":5}})");
    voice.Event(R"({"type":"turn.created"})");
    app.Drain();
    assert(app.audio.resets == 0 && voice.speaking == 1);
    voice.Event(R"({"type":"turn.created","turn":{"role":"user"}})");
    assert(app.audio.resets == 0);
    app.Drain();
    assert(app.audio.resets == 1 && voice.speaking == 0 && voice.opened);
    voice.Event(R"({"type":"turn.created","turn":{"role":"user"}})");
    voice.request_id_ = "new-call";
    app.Drain();
    assert(app.audio.resets == 1);
    voice.Event(R"({"type":"turn.created","turn":{"role":"user"}})");
    voice.opened = false;
    app.Drain();
    assert(app.audio.resets == 1);
}
'''.replace("HANDLER", handler)
    cjson = root / "managed_components/espressif__cjson/cJSON"
    with tempfile.TemporaryDirectory(prefix="voicemode-message-test-") as directory:
        path = Path(directory)
        (path / "test.cc").write_text(program)
        subprocess.run(["cc", "-c", str(cjson / "cJSON.c"), "-o", str(path / "json.o")], check=True)
        subprocess.run(["c++", "-std=c++17", "-I", str(cjson),
                        "-I", str(root / "main/protocols"),
                        str(path / "test.cc"), str(path / "json.o"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True, timeout=5)
    print("PASS: live model/activity messages validate input and reject stale call updates")


if __name__ == "__main__":
    main()
