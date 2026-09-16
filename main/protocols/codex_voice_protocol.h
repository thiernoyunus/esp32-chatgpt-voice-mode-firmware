#ifndef CODEX_VOICE_PROTOCOL_H
#define CODEX_VOICE_PROTOCOL_H

#include "protocol.h"
#include "voice_readiness.h"

#include <esp_peer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WebSocket;

class CodexVoiceProtocol : public Protocol {
public:
    struct ModelChoice { std::string id; std::string name; };
    struct ChatChoice { std::string id; std::string name; };
    const std::vector<ModelChoice>& GetModels() const { return models_; }
    bool SelectModel(size_t index);
    // Recent Codex chats offered by the bridge on the last answer.
    const std::vector<ChatChoice>& GetChats() const { return chats_; }
    // Pick the chat the next call resumes. index 0 means "New chat".
    bool SelectChat(size_t index);
    CodexVoiceProtocol();
    ~CodexVoiceProtocol() override;

    bool Start() override;
    void SendMcpMessage(const std::string& payload) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendListenCancel() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;

    // True once per stalled reply, while the retry budget lasts. The stall
    // check tears the call down because a wedged audio track never recovers on
    // its own; this tells the application the teardown was ours and a fresh
    // call is worth making. Reading it clears it, so one stall buys one retry.
    bool TakeStallRecovery() { return stall_recovery_.exchange(false); }

    // The five links a call needs, in the order a healthy one reaches them.
    // A silent call can then say which link never arrived instead of only that
    // nothing was heard.
    uint32_t VoiceStageMask() const { return readiness_.Mask(); }
    std::string DescribeVoiceStages() const { return readiness_.Describe(); }
    // The application calls this once a frame has been accepted for playback,
    // which is the last link the device can check for itself.
    void MarkPlaybackAdmitted() override { MarkStage(kVoiceStagePlaybackAdmitted); }

private:
    std::vector<ModelChoice> models_{{"", "Default"}};
    std::vector<ChatChoice> chats_;
    std::unique_ptr<WebSocket> websocket_;
    esp_peer_handle_t peer_ = nullptr;
    EventGroupHandle_t peer_events_ = nullptr;
    std::atomic<bool> peer_running_{false};
    std::atomic<bool> channel_open_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> speaking_{false};
    std::string request_id_;
    uint32_t uplink_pts_ms_ = 0;
    VoiceReadiness readiness_;

    bool OpenControlChannel();
    bool SendText(const std::string& text) override;
    bool SendSignalOffer(const uint8_t* data, size_t size);
    void HandleSignal(const char* data, size_t size);
    void HandleRealtimeEvent(const uint8_t* data, size_t size);
    void StartSpeaking();
    void StopSpeaking();
    void EmitSpeechEvent(const char* state, const char* text = nullptr);
    void EmitTranscript(const char* role, const char* text);
    void StreamTranscript(const char* role, const char* delta);
    void Fail(const std::string& message);
    // Records a link once, and says so the first time only.
    void MarkStage(uint32_t stage);
    void RunPeerLoop();
    // Recovers a reply that is being transcribed but never reaches the speaker.
    void CheckInboundAudioStall();

    // Millisecond of the last inbound audio frame, and of the last assistant
    // reply that was expected to be spoken. Both are written from peer/data
    // callbacks and read by the peer loop.
    std::atomic<uint32_t> last_audio_frame_ms_{0};
    std::atomic<uint32_t> speech_expected_since_ms_{0};
    // Reset by the first real speech frame, so the budget runs down only while
    // calls keep coming up silent - it is not a lifetime cap.
    std::atomic<bool> stall_recovery_{false};
    std::atomic<int> stall_retries_{0};
    // The reply as it is being written. Touched only from the data-channel
    // callback, which is the one task that parses these messages.
    std::string transcript_partial_;
    std::string transcript_role_;
    uint32_t transcript_emitted_at_ = 0;

    static int OnPeerState(esp_peer_state_t state, void* context);
    static int OnPeerMessage(esp_peer_msg_t* message, void* context);
    static int OnPeerAudioInfo(esp_peer_audio_stream_info_t* info, void* context);
    static int OnPeerAudio(esp_peer_audio_frame_t* frame, void* context);
    static int OnDataChannelOpen(esp_peer_data_channel_info_t* channel, void* context);
    static int OnPeerData(esp_peer_data_frame_t* frame, void* context);
};

#endif  // CODEX_VOICE_PROTOCOL_H
