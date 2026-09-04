#include "codex_voice_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_peer_default.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <cstring>

#define TAG "CodexVoice"

namespace {

constexpr EventBits_t kVoiceReadyBit = BIT0;
constexpr EventBits_t kVoiceFailedBit = BIT1;
constexpr EventBits_t kPeerStoppedBit = BIT2;
constexpr int kVoiceSetupTimeoutMs = 30000;
constexpr int kPeerTaskStackSize = 12 * 1024;

uint32_t NowMilliseconds() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

std::string BuildConnectionUrl(const std::string& base_url, const std::string& device_id,
                               const std::string& token) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + "/agents/apollo/" + device_id + "?token=" + token;
}

constexpr int OpusFrameDurationMs(uint8_t config) {
    if (config < 12) {
        constexpr int kSilkDurations[] = {10, 20, 40, 60};
        return kSilkDurations[config % 4];
    }
    if (config < 16) {
        return config % 2 == 0 ? 10 : 20;
    }
    constexpr int kCeltDurations[] = {0, 5, 10, 20};
    return kCeltDurations[config % 4];
}

int OpusPacketDurationMs(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 20;
    }
    const int frame_duration = OpusFrameDurationMs(data[0] >> 3);
    const uint8_t frame_code = data[0] & 0x03;
    const int frame_count = frame_code == 0 ? 1 : frame_code < 3 ? 2 : size > 1 ? data[1] & 0x3f : 0;
    const int duration = frame_duration * frame_count;
    switch (duration) {
        case 5:
        case 10:
        case 20:
        case 40:
        case 60:
        case 80:
        case 100:
        case 120:
            return duration;
        default:
            return 20;
    }
}

static_assert(OpusFrameDurationMs(31) == 20);

const char* ReadErrorMessage(const cJSON* root) {
    const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON* message = cJSON_IsObject(error)
                               ? cJSON_GetObjectItemCaseSensitive(error, "message")
                               : cJSON_GetObjectItemCaseSensitive(root, "message");
    return cJSON_IsString(message) ? message->valuestring : "ChatGPT Voice failed.";
}

}  // namespace

CodexVoiceProtocol::CodexVoiceProtocol() { peer_events_ = xEventGroupCreate(); }

CodexVoiceProtocol::~CodexVoiceProtocol() {
    CloseAudioChannel(false);
    if (peer_events_ != nullptr) {
        vEventGroupDelete(peer_events_);
    }
}

bool CodexVoiceProtocol::Start() { return true; }

bool CodexVoiceProtocol::SendText(const std::string& text) {
    return websocket_ != nullptr && websocket_->IsConnected() && websocket_->Send(text);
}

bool CodexVoiceProtocol::OpenAudioChannel() {
    if (IsAudioChannelOpened()) {
        return true;
    }
    CloseAudioChannel(false);

    Settings settings("apollo", false);
    std::string base_url = settings.GetString("url", CONFIG_APOLLO_URL);
    std::string token = settings.GetString("token", CONFIG_APOLLO_TOKEN);
    std::string device_id = settings.GetString("device_id", CONFIG_APOLLO_DEVICE_ID);
    if (base_url.empty()) {
        SetError(Lang::Strings::SERVER_NOT_FOUND);
        return false;
    }
    if (device_id.empty()) {
        device_id = SystemInfo::GetMacAddress();
    }

    error_occurred_ = false;
    closing_ = false;
    speaking_ = false;
    user_transcript_.clear();
    assistant_transcript_.clear();
    uplink_pts_ms_ = 0;
    server_sample_rate_ = 16000;
    server_frame_duration_ = 20;
    request_id_ = std::to_string(esp_random()) + "-" + std::to_string(NowMilliseconds());
    xEventGroupClearBits(peer_events_, kVoiceReadyBit | kVoiceFailedBit | kPeerStoppedBit);

    websocket_ = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }
    websocket_->OnData([this](const char* data, size_t size, bool binary) {
        if (!binary) {
            HandleSignal(data, size);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });
    websocket_->OnDisconnected([this]() {
        channel_open_ = false;
        if (!closing_) {
            Fail("Apollo's Codex Voice bridge disconnected.");
        }
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });
    if (!websocket_->Connect(BuildConnectionUrl(base_url, device_id, token).c_str())) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        CloseAudioChannel(false);
        return false;
    }

    cJSON* hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "deviceId", device_id.c_str());
    cJSON_AddNumberToObject(hello, "ts", NowMilliseconds());
    char* hello_json = cJSON_PrintUnformatted(hello);
    const bool hello_sent = hello_json != nullptr && SendText(hello_json);
    cJSON_free(hello_json);
    cJSON_Delete(hello);
    if (!hello_sent) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        CloseAudioChannel(false);
        return false;
    }

    esp_peer_default_cfg_t peer_defaults = {};
    peer_defaults.agent_recv_timeout = 500;
    peer_defaults.ice_use_lite_mode = true;
    peer_defaults.data_ch_cfg.send_cache_size = 8 * 1024;
    peer_defaults.rtp_cfg.audio_recv_jitter.cache_size = 16 * 1024;
    peer_defaults.rtp_cfg.send_pool_size = 16 * 1024;
    peer_defaults.rtp_cfg.send_queue_num = 40;

    esp_peer_cfg_t peer_config = {};
    peer_config.role = ESP_PEER_ROLE_CONTROLLING;
    peer_config.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
    peer_config.audio_info.sample_rate = 16000;
    peer_config.audio_info.channel = 1;
    peer_config.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    peer_config.enable_data_channel = true;
    peer_config.manual_ch_create = true;
    peer_config.no_auto_reconnect = true;
    peer_config.on_state = OnPeerState;
    peer_config.on_msg = OnPeerMessage;
    peer_config.on_audio_info = OnPeerAudioInfo;
    peer_config.on_audio_data = OnPeerAudio;
    peer_config.on_channel_open = OnDataChannelOpen;
    peer_config.on_data = OnPeerData;
    peer_config.ctx = this;
    peer_config.extra_cfg = &peer_defaults;
    peer_config.extra_size = sizeof(peer_defaults);

    esp_peer_pre_generate_cert();
    if (esp_peer_open(&peer_config, esp_peer_get_default_impl(), &peer_) != ESP_PEER_ERR_NONE) {
        SetError("Could not start WebRTC on Apollo.");
        CloseAudioChannel(false);
        return false;
    }

    peer_running_ = true;
    if (xTaskCreate(
            [](void* context) { static_cast<CodexVoiceProtocol*>(context)->RunPeerLoop(); },
            "codex_voice_peer", kPeerTaskStackSize, this, 5, nullptr) != pdPASS) {
        peer_running_ = false;
        SetError("Could not start Apollo's WebRTC task.");
        CloseAudioChannel(false);
        return false;
    }
    if (esp_peer_new_connection(peer_) != ESP_PEER_ERR_NONE) {
        SetError("Could not create the ChatGPT Voice call.");
        CloseAudioChannel(false);
        return false;
    }

    const EventBits_t result = xEventGroupWaitBits(
        peer_events_, kVoiceReadyBit | kVoiceFailedBit, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(kVoiceSetupTimeoutMs));
    if ((result & kVoiceReadyBit) == 0) {
        if ((result & kVoiceFailedBit) == 0) {
            SetError("ChatGPT Voice setup timed out.");
        }
        CloseAudioChannel(false);
        return false;
    }

    if (on_connected_ != nullptr) {
        on_connected_();
    }
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void CodexVoiceProtocol::CloseAudioChannel(bool send_goodbye) {
    const bool was_running = peer_ != nullptr || websocket_ != nullptr;
    closing_ = true;
    channel_open_ = false;
    speaking_ = false;

    if (send_goodbye && websocket_ != nullptr && websocket_->IsConnected() &&
        !request_id_.empty()) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "realtime_stop");
        cJSON_AddStringToObject(root, "requestId", request_id_.c_str());
        cJSON_AddNumberToObject(root, "ts", NowMilliseconds());
        char* json = cJSON_PrintUnformatted(root);
        if (json != nullptr) {
            SendText(json);
        }
        cJSON_free(json);
        cJSON_Delete(root);
    }

    if (peer_running_.exchange(false)) {
        xEventGroupWaitBits(peer_events_, kPeerStoppedBit, pdFALSE, pdFALSE, portMAX_DELAY);
    }
    if (peer_ != nullptr) {
        esp_peer_close(peer_);
        peer_ = nullptr;
    }
    websocket_.reset();
    request_id_.clear();
    if (was_running && on_disconnected_ != nullptr) {
        on_disconnected_();
    }
}

bool CodexVoiceProtocol::IsAudioChannelOpened() const {
    return channel_open_ && websocket_ != nullptr && websocket_->IsConnected() &&
           !error_occurred_;
}

bool CodexVoiceProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!channel_open_ || peer_ == nullptr) {
        return true;
    }
    esp_peer_audio_frame_t frame = {};
    frame.data = packet->payload.data();
    frame.size = static_cast<int>(packet->payload.size());
    frame.pts = uplink_pts_ms_;
    uplink_pts_ms_ += packet->frame_duration > 0
                          ? packet->frame_duration
                          : OpusPacketDurationMs(packet->payload.data(), packet->payload.size());
    const int result = esp_peer_send_audio(peer_, &frame);
    return result == ESP_PEER_ERR_NONE || result == ESP_PEER_ERR_WOULD_BLOCK;
}

void CodexVoiceProtocol::SendStartListening(ListeningMode mode) { (void)mode; }

void CodexVoiceProtocol::SendStopListening() {}

void CodexVoiceProtocol::SendListenCancel() { CloseAudioChannel(); }

void CodexVoiceProtocol::SendWakeWordDetected(const std::string& wake_word) { (void)wake_word; }

void CodexVoiceProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    StopSpeaking();
    CloseAudioChannel();
}

bool CodexVoiceProtocol::SendSignalOffer(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || websocket_ == nullptr) {
        return false;
    }
    const std::string sdp(reinterpret_cast<const char*>(data), size);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "realtime_offer");
    cJSON_AddStringToObject(root, "requestId", request_id_.c_str());
    cJSON_AddStringToObject(root, "sdp", sdp.c_str());
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());
    char* json = cJSON_PrintUnformatted(root);
    const bool sent = json != nullptr && SendText(json);
    cJSON_free(json);
    cJSON_Delete(root);
    if (!sent) {
        Fail("Could not send Apollo's WebRTC offer.");
    }
    return sent;
}

void CodexVoiceProtocol::HandleSignal(const char* data, size_t size) {
    cJSON* root = cJSON_ParseWithLength(data, size);
    if (root == nullptr) {
        return;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON* request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (!cJSON_IsString(type) || !cJSON_IsString(request_id) ||
        request_id_ != request_id->valuestring) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "realtime_answer") == 0) {
        const cJSON* sdp = cJSON_GetObjectItemCaseSensitive(root, "sdp");
        if (cJSON_IsString(sdp) && peer_ != nullptr) {
            esp_peer_msg_t message = {};
            message.type = ESP_PEER_MSG_TYPE_SDP;
            message.data = reinterpret_cast<uint8_t*>(sdp->valuestring);
            message.size = static_cast<int>(strlen(sdp->valuestring));
            if (esp_peer_send_msg(peer_, &message) != ESP_PEER_ERR_NONE) {
                Fail("ChatGPT returned an unusable WebRTC answer.");
            }
        }
    } else if (strcmp(type->valuestring, "realtime_error") == 0) {
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
        Fail(cJSON_IsString(message) ? message->valuestring : "ChatGPT Voice failed.");
    }
    cJSON_Delete(root);
}

void CodexVoiceProtocol::HandleRealtimeEvent(const uint8_t* data, size_t size) {
    cJSON* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(data), size);
    if (root == nullptr) {
        return;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char* event_type = type->valuestring;
    if (strcmp(event_type, "error") == 0 ||
        strcmp(event_type, "invalid_request_error") == 0) {
        Fail(ReadErrorMessage(root));
    } else if (strcmp(event_type, "input_transcript.added") == 0 ||
               strcmp(event_type, "output_transcript.added") == 0) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, "item");
        const cJSON* text = cJSON_IsObject(item)
                                ? cJSON_GetObjectItemCaseSensitive(item, "text")
                                : nullptr;
        if (cJSON_IsString(text)) {
            auto& transcript = event_type[0] == 'i' ? user_transcript_
                                                    : assistant_transcript_;
            transcript += text->valuestring;
        }
    } else if (strcmp(event_type, "turn.done") == 0) {
        const cJSON* turn = cJSON_GetObjectItemCaseSensitive(root, "turn");
        const cJSON* role = cJSON_IsObject(turn)
                                ? cJSON_GetObjectItemCaseSensitive(turn, "role")
                                : nullptr;
        const cJSON* transcript = cJSON_IsObject(turn)
                                      ? cJSON_GetObjectItemCaseSensitive(turn, "transcript")
                                      : nullptr;
        if (cJSON_IsString(role)) {
            const bool is_user = strcmp(role->valuestring, "user") == 0;
            const bool is_assistant = strcmp(role->valuestring, "assistant") == 0;
            if (is_user || is_assistant) {
                auto& accumulated = is_user ? user_transcript_ : assistant_transcript_;
                const char* text = cJSON_IsString(transcript) ? transcript->valuestring
                                                              : accumulated.c_str();
                if (text[0] != '\0') {
                    EmitTranscript(role->valuestring, text);
                }
                accumulated.clear();
                if (is_assistant) {
                    StopSpeaking();
                }
            }
        }
    } else if (strcmp(event_type, "response.created") == 0) {
        assistant_transcript_.clear();
        StartSpeaking();
    } else if (strstr(event_type, "audio_transcript.delta") != nullptr) {
        const cJSON* delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(delta)) {
            assistant_transcript_ += delta->valuestring;
        }
    } else if (strstr(event_type, "audio_transcript.done") != nullptr) {
        const cJSON* transcript = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        const char* text = cJSON_IsString(transcript) ? transcript->valuestring
                                                      : assistant_transcript_.c_str();
        if (text[0] != '\0') {
            EmitTranscript("assistant", text);
        }
    } else if (strcmp(event_type, "conversation.item.input_audio_transcription.completed") ==
               0) {
        const cJSON* transcript = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (cJSON_IsString(transcript) && transcript->valuestring[0] != '\0') {
            EmitTranscript("user", transcript->valuestring);
        }
    } else if (strcmp(event_type, "response.done") == 0) {
        StopSpeaking();
    }
    cJSON_Delete(root);
}

void CodexVoiceProtocol::StartSpeaking() {
    if (!speaking_.exchange(true)) {
        EmitSpeechEvent("start");
    }
}

void CodexVoiceProtocol::StopSpeaking() {
    if (speaking_.exchange(false)) {
        EmitSpeechEvent("stop");
    }
}

void CodexVoiceProtocol::EmitSpeechEvent(const char* state, const char* text) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    if (text != nullptr) {
        cJSON_AddStringToObject(root, "text", text);
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}

void CodexVoiceProtocol::EmitTranscript(const char* role, const char* text) {
    if (on_incoming_json_ == nullptr) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", strcmp(role, "user") == 0 ? "stt" : "tts");
    if (strcmp(role, "user") != 0) {
        cJSON_AddStringToObject(root, "state", "sentence_start");
    }
    cJSON_AddStringToObject(root, "text", text);
    on_incoming_json_(root);
    cJSON_Delete(root);
}

void CodexVoiceProtocol::Fail(const std::string& message) {
    if (closing_) {
        return;
    }
    ESP_LOGE(TAG, "%s", message.c_str());
    SetError(message);
    xEventGroupSetBits(peer_events_, kVoiceFailedBit);
}

void CodexVoiceProtocol::RunPeerLoop() {
    while (peer_running_) {
        esp_peer_main_loop(peer_);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xEventGroupSetBits(peer_events_, kPeerStoppedBit);
    vTaskDelete(nullptr);
}

int CodexVoiceProtocol::OnPeerState(esp_peer_state_t state, void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
        esp_peer_data_channel_cfg_t channel = {};
        channel.type = ESP_PEER_DATA_CHANNEL_RELIABLE;
        channel.ordered = true;
        channel.label = const_cast<char*>("oai-events");
        if (esp_peer_create_data_channel(protocol->peer_, &channel) != ESP_PEER_ERR_NONE) {
            protocol->Fail("Could not open ChatGPT's voice event channel.");
        }
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED ||
               (state == ESP_PEER_STATE_DISCONNECTED && !protocol->closing_)) {
        protocol->Fail("ChatGPT Voice could not connect.");
    }
    return 0;
}

int CodexVoiceProtocol::OnPeerMessage(esp_peer_msg_t* message, void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (message != nullptr && message->type == ESP_PEER_MSG_TYPE_SDP) {
        protocol->SendSignalOffer(message->data, message->size);
    }
    return 0;
}

int CodexVoiceProtocol::OnPeerAudioInfo(esp_peer_audio_stream_info_t* info, void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (info != nullptr && info->sample_rate > 0) {
        protocol->server_sample_rate_ = static_cast<int>(info->sample_rate);
    }
    return 0;
}

int CodexVoiceProtocol::OnPeerAudio(esp_peer_audio_frame_t* frame, void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (frame == nullptr || frame->data == nullptr || frame->size <= 0 ||
        protocol->on_incoming_audio_ == nullptr) {
        return 0;
    }
    protocol->StartSpeaking();
    auto* packet = new AudioStreamPacket();
    packet->sample_rate = protocol->server_sample_rate_;
    packet->frame_duration = OpusPacketDurationMs(frame->data, frame->size);
    packet->timestamp = frame->pts;
    packet->payload.assign(frame->data, frame->data + frame->size);
    packet->pcm = false;
    auto callback = protocol->on_incoming_audio_;
    Application::GetInstance().Schedule([callback, packet]() {
        callback(std::unique_ptr<AudioStreamPacket>(packet));
    });
    return 0;
}

int CodexVoiceProtocol::OnDataChannelOpen(esp_peer_data_channel_info_t* channel,
                                          void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (channel != nullptr && channel->label != nullptr &&
        strcmp(channel->label, "oai-events") == 0) {
        protocol->channel_open_ = true;
        xEventGroupSetBits(protocol->peer_events_, kVoiceReadyBit);
    }
    return 0;
}

int CodexVoiceProtocol::OnPeerData(esp_peer_data_frame_t* frame, void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (frame != nullptr && frame->data != nullptr && frame->size > 0) {
        protocol->HandleRealtimeEvent(frame->data, frame->size);
    }
    return 0;
}
