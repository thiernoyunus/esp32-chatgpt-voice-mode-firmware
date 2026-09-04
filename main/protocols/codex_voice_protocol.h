#ifndef CODEX_VOICE_PROTOCOL_H
#define CODEX_VOICE_PROTOCOL_H

#include "protocol.h"

#include <esp_peer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

class WebSocket;

class CodexVoiceProtocol : public Protocol {
public:
    CodexVoiceProtocol();
    ~CodexVoiceProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendListenCancel() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;

private:
    std::unique_ptr<WebSocket> websocket_;
    esp_peer_handle_t peer_ = nullptr;
    EventGroupHandle_t peer_events_ = nullptr;
    std::atomic<bool> peer_running_{false};
    std::atomic<bool> channel_open_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> speaking_{false};
    std::string request_id_;
    std::string user_transcript_;
    std::string assistant_transcript_;
    uint32_t uplink_pts_ms_ = 0;

    bool SendText(const std::string& text) override;
    bool SendSignalOffer(const uint8_t* data, size_t size);
    void HandleSignal(const char* data, size_t size);
    void HandleRealtimeEvent(const uint8_t* data, size_t size);
    void StartSpeaking();
    void StopSpeaking();
    void EmitSpeechEvent(const char* state, const char* text = nullptr);
    void EmitTranscript(const char* role, const char* text);
    void Fail(const std::string& message);
    void RunPeerLoop();

    static int OnPeerState(esp_peer_state_t state, void* context);
    static int OnPeerMessage(esp_peer_msg_t* message, void* context);
    static int OnPeerAudioInfo(esp_peer_audio_stream_info_t* info, void* context);
    static int OnPeerAudio(esp_peer_audio_frame_t* frame, void* context);
    static int OnDataChannelOpen(esp_peer_data_channel_info_t* channel, void* context);
    static int OnPeerData(esp_peer_data_frame_t* frame, void* context);
};

#endif  // CODEX_VOICE_PROTOCOL_H
