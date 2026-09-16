#include "codex_voice_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
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

// How long a reply may be transcribed with no inbound audio before the call is
// treated as wedged. Long enough to survive a slow first frame, short enough
// that the user does not sit through a whole silent answer.
// ponytail: fixed threshold; revisit only if healthy calls trip it.
constexpr uint32_t kInboundAudioStallMs = 4000;

/* Did `sample` happen at or after `since`? Subtracting first keeps this honest
 * across the millisecond clock's 49-day wrap, which a plain > would not. */
bool AtOrAfter(uint32_t sample, uint32_t since) {
    return static_cast<int32_t>(sample - since) >= 0;
}
constexpr uint32_t kAudioLogBurstGapMs = 250;
/* Opus sends 1-3 byte DTX/comfort-noise frames when there is nothing to say.
 * At 3 they counted as speech, so a track carrying only silence looked alive
 * to the stall check and to the retry budget below. Real 20ms speech at 16kHz
 * is tens of bytes; 10 clears comfort noise without clipping quiet syllables. */
constexpr size_t kMinimumVoiceAudioBytes = 10;
/* Consecutive silent calls to rebuild before handing it back to the user. Each
 * attempt costs kInboundAudioStallMs, so this is seconds, not minutes. */
constexpr int kMaxStallRetries = 3;

std::string BuildConnectionUrl(const std::string& base_url, const std::string& device_id,
                               const std::string& token) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + "/agents/voicemode/" + device_id + "?token=" + token;
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

/* What to tell someone whose call went quiet, decided by the link that never
 * arrived. "Nothing came through" is true of five different faults, and only one
 * of them is worth retrying unchanged; naming the link is what makes the
 * message worth reading. */
std::string StallMessage(uint32_t missing_stage, bool will_retry) {
    const char* cause = "The voice stopped coming through";
    switch (missing_stage) {
        case kVoiceStagePeerConnected:
        case kVoiceStageAudioTrack:
            cause = "The reply's audio never arrived";
            break;
        case kVoiceStageEventChannel:
            cause = "The voice channel never opened";
            break;
        case kVoiceStageSessionStarted:
            cause = "The voice session never started";
            break;
        case kVoiceStagePlaybackAdmitted:
            cause = "The reply never reached the speaker";
            break;
        default:
            break;
    }
    return std::string(cause) + (will_retry ? ". Reconnecting." : ". Tap to try again.");
}

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

bool CodexVoiceProtocol::Start() {
    // Keep the MCP control path available while idle. WebRTC is opened only
    // when the user starts a voice call.
    closing_ = true;
    error_occurred_ = false;
    return OpenControlChannel();
}

bool CodexVoiceProtocol::OpenControlChannel() {
    if (websocket_ != nullptr && websocket_->IsConnected()) {
        return true;
    }
    websocket_.reset();

    Settings settings("voicemode", false);
    const std::string base_url = settings.GetString("url", CONFIG_VOICEMODE_URL);
    const std::string token = settings.GetString("token", CONFIG_VOICEMODE_TOKEN);
    std::string device_id = settings.GetString("device_id", CONFIG_VOICEMODE_DEVICE_ID);
    if (base_url.empty()) {
        ESP_LOGE(TAG, "Voice mode URL is not configured");
        return false;
    }
    if (device_id.empty()) {
        device_id = SystemInfo::GetMacAddress();
    }

    websocket_ = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Could not create control channel");
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
            Fail("The Mac disconnected.");
        }
    });
    if (!websocket_->Connect(BuildConnectionUrl(base_url, device_id, token).c_str())) {
        ESP_LOGE(TAG, "Could not connect to control channel");
        websocket_.reset();
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
        ESP_LOGE(TAG, "Could not identify control channel");
        websocket_.reset();
        return false;
    }
    ESP_LOGI(TAG, "control channel ready");
    return true;
}

bool CodexVoiceProtocol::SendText(const std::string& text) {
    return websocket_ != nullptr && websocket_->IsConnected() && websocket_->Send(text);
}

void CodexVoiceProtocol::SendMcpMessage(const std::string& payload) {
    cJSON* payload_json = cJSON_Parse(payload.c_str());
    if (payload_json == nullptr) {
        ESP_LOGE(TAG, "Dropping unparseable MCP payload");
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload_json);
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());
    char* serialized = cJSON_PrintUnformatted(root);
    if (serialized != nullptr && !SendText(serialized)) {
        ESP_LOGW(TAG, "MCP response dropped: control channel is offline");
    }
    cJSON_free(serialized);
    cJSON_Delete(root);
}

bool CodexVoiceProtocol::OpenAudioChannel() {
    if (IsAudioChannelOpened()) {
        return true;
    }
    CloseAudioChannel(false);

    error_occurred_ = false;
    closing_ = false;
    speaking_ = false;
    uplink_pts_ms_ = 0;
    last_audio_frame_ms_.store(0);
    speech_expected_since_ms_.store(0);
    readiness_.Reset();
    server_sample_rate_ = 16000;
    server_frame_duration_ = 20;
    request_id_ = std::to_string(esp_random()) + "-" + std::to_string(NowMilliseconds());
    xEventGroupClearBits(peer_events_, kVoiceReadyBit | kVoiceFailedBit | kPeerStoppedBit);

    if (!OpenControlChannel()) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        CloseAudioChannel(false);
        return false;
    }

    esp_peer_default_cfg_t peer_defaults = {};
    peer_defaults.agent_recv_timeout = 500;
    peer_defaults.ice_use_lite_mode = true;
    peer_defaults.data_ch_cfg.send_cache_size = 8 * 1024;
    peer_defaults.rtp_cfg.audio_recv_jitter.cache_size = 16 * 1024;
    /* A lost RTP packet can wedge the inbound audio track: the jitter buffer
     * waits for a frame that never arrives and every later frame is held back,
     * so captions keep flowing on the data channel while the speaker stays
     * silent for the rest of the call. Asking the sender for a fresh keyframe
     * once a second lets the stream re-sync instead of stalling forever.
     * ponytail: fixed 1 s interval; only tune if recovery is visibly slow. */
    peer_defaults.rtp_cfg.audio_recv_jitter.pli_send_interval = 1000;
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
        SetError("Could not start WebRTC.");
        CloseAudioChannel(false);
        return false;
    }

    peer_running_ = true;
    if (xTaskCreate(
            [](void* context) { static_cast<CodexVoiceProtocol*>(context)->RunPeerLoop(); },
            "codex_voice_peer", kPeerTaskStackSize, this, 5, nullptr) != pdPASS) {
        peer_running_ = false;
        SetError("Could not start the WebRTC task.");
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
    if ((result & kVoiceFailedBit) != 0 || (result & kVoiceReadyBit) == 0 ||
        !IsAudioChannelOpened()) {
        if ((result & kVoiceFailedBit) == 0) {
            SetError("ChatGPT Voice setup timed out.");
        }
        CloseAudioChannel(false);
        return false;
    }

    /* Say what the call actually has at the moment it calls itself open: it
     * can reach this point with no audio track and no live session, and that is
     * the state that used to be reported as simply ready. */
    ESP_LOGI(TAG, "Voice call open with: %s", readiness_.Describe().c_str());
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void CodexVoiceProtocol::CloseAudioChannel(bool send_goodbye) {
    const bool was_running = peer_ != nullptr || peer_running_.load() || !request_id_.empty();
    closing_ = true;
    channel_open_ = false;
    speaking_ = false;
    /* One protocol object serves every call. A call that ends mid-reply would
     * otherwise leave half a sentence here for the next one to append to and
     * caption. */
    transcript_partial_.clear();
    transcript_role_.clear();
    transcript_emitted_at_ = 0;
    speech_expected_since_ms_.store(0);

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
    request_id_.clear();
    if (was_running && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
    if (was_running && on_disconnected_ != nullptr) {
        on_disconnected_();
    }
}

bool CodexVoiceProtocol::IsAudioChannelOpened() const {
    return channel_open_ && websocket_ != nullptr && websocket_->IsConnected() &&
           !error_occurred_;
}

bool CodexVoiceProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened() || peer_ == nullptr) {
        return false;
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
    Settings settings("codex_voice", false);
    const auto model = settings.GetString("model", "");
    if (!model.empty()) {
        cJSON_AddStringToObject(root, "model", model.c_str());
    }
    // A saved chat id resumes that Codex chat; no id means a new chat. The
    // chat stays visible in Codex unless the user turned on temporary chats.
    const auto chat = settings.GetString("chat", "");
    const bool temporary = settings.GetBool("temporary", false);
    if (!chat.empty() && !temporary) {
        cJSON_AddStringToObject(root, "threadId", chat.c_str());
    }
    if (temporary) {
        cJSON_AddBoolToObject(root, "temporary", true);
    }
    // Saved spoken voice for this call; absent means the ChatGPT default.
    const auto voice = settings.GetString("voice", "");
    if (!voice.empty()) {
        cJSON_AddStringToObject(root, "voice", voice.c_str());
    }
    cJSON_AddNumberToObject(root, "ts", NowMilliseconds());
    char* json = cJSON_PrintUnformatted(root);
    const bool sent = json != nullptr && SendText(json);
    cJSON_free(json);
    cJSON_Delete(root);
    if (!sent) {
        Fail("Could not send the WebRTC offer.");
    }
    return sent;
}

bool CodexVoiceProtocol::SelectModel(size_t index) {
    if (index >= models_.size()) return false;
    Settings settings("codex_voice", true);
    settings.SetString("model", models_[index].id);
    Board::GetInstance().GetDisplay()->SetVoiceModel(models_[index].name.c_str());
    return true;
}

// index 0 is the "New chat" row the UI prepends, so it clears the saved id.
bool CodexVoiceProtocol::SelectChat(size_t index) {
    if (index > chats_.size()) return false;
    Settings settings("codex_voice", true);
    settings.SetString("chat", index == 0 ? "" : chats_[index - 1].id);
    return true;
}

void CodexVoiceProtocol::HandleSignal(const char* data, size_t size) {
    cJSON* root = cJSON_ParseWithLength(data, size);
    if (root == nullptr) {
        return;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    // Device tools use JSON-RPC IDs, not the voice session's requestId.
    if (cJSON_IsString(type) && strcmp(type->valuestring, "mcp") == 0) {
        if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "payload")) && on_incoming_json_) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        return;
    }
    const cJSON* request_id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (!cJSON_IsString(type) || !cJSON_IsString(request_id) ||
        request_id_ != request_id->valuestring) {
        cJSON_Delete(root);
        return;
    }
    if (strcmp(type->valuestring, "realtime_answer") == 0) {
        const cJSON* models = cJSON_GetObjectItemCaseSensitive(root, "models");
        if (cJSON_IsArray(models) && cJSON_GetArraySize(models) <= 40) {
            std::vector<ModelChoice> choices{{"", "Default"}};
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, models) {
                const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
                const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (cJSON_IsString(id) && cJSON_IsString(name) &&
                    id->valuestring[0] && strlen(id->valuestring) <= 128 &&
                    name->valuestring[0] && strlen(name->valuestring) <= 80) {
                    choices.push_back({id->valuestring, name->valuestring});
                }
            }
            const cJSON* selected = cJSON_GetObjectItemCaseSensitive(root, "selectedModel");
            const std::string selected_id = cJSON_IsString(selected) && strlen(selected->valuestring) <= 128
                                                ? selected->valuestring : "";
            const auto session_id = request_id_;
            Application::GetInstance().Schedule([this, choices = std::move(choices), selected_id, session_id]() mutable {
                if (request_id_ != session_id) return;
                models_ = std::move(choices);
                for (const auto& choice : models_) {
                    if (choice.id == selected_id) {
                        Board::GetInstance().GetDisplay()->SetVoiceModel(choice.name.c_str());
                        break;
                    }
                }
            });
        }
        // Recent Codex chats for the picker. The id this call landed on only
        // replaces an existing selection, so a stale/deleted chat heals itself
        // while the default stays "new chat on every tap".
        const cJSON* chats = cJSON_GetObjectItemCaseSensitive(root, "chats");
        const cJSON* thread = cJSON_GetObjectItemCaseSensitive(root, "threadId");
        if (cJSON_IsArray(chats) || cJSON_IsString(thread)) {
            std::vector<ChatChoice> chat_choices;
            const cJSON* entry = nullptr;
            if (cJSON_IsArray(chats) && cJSON_GetArraySize(chats) <= 20) {
                cJSON_ArrayForEach(entry, chats) {
                    const cJSON* id = cJSON_GetObjectItemCaseSensitive(entry, "id");
                    const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
                    if (cJSON_IsString(id) && cJSON_IsString(name) &&
                        id->valuestring[0] && strlen(id->valuestring) <= 64 &&
                        name->valuestring[0] && strlen(name->valuestring) <= 60) {
                        chat_choices.push_back({id->valuestring, name->valuestring});
                    }
                }
            }
            const std::string active_id =
                cJSON_IsString(thread) && strlen(thread->valuestring) <= 64
                    ? thread->valuestring : "";
            const auto session_id = request_id_;
            Application::GetInstance().Schedule(
                [this, chat_choices = std::move(chat_choices), active_id, session_id]() mutable {
                    if (request_id_ != session_id) return;
                    chats_ = std::move(chat_choices);
                    if (active_id.empty()) return;
                    Settings read("codex_voice", false);
                    if (read.GetString("chat", "").empty()) return;
                    Settings settings("codex_voice", true);
                    settings.SetString("chat", active_id);
                });
        }
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
    } else if (strcmp(type->valuestring, "realtime_transcript_delta") == 0) {
        const cJSON* role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON* delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
        if (cJSON_IsString(role) && cJSON_IsString(delta) && delta->valuestring[0] != '\0') {
            const bool assistant = strcmp(role->valuestring, "assistant") == 0;
            if (assistant) {
                StartSpeaking();
                // The assistant is producing a reply, so audio should follow.
                // Arm the stall check; the check itself only counts frames that
                // arrive after this moment, so a packet still draining from an
                // interrupted reply cannot stand in for this one.
                uint32_t expected = 0;
                speech_expected_since_ms_.compare_exchange_strong(expected, NowMilliseconds());
            }
            if (assistant || strcmp(role->valuestring, "user") == 0) {
                StreamTranscript(role->valuestring, delta->valuestring);
            }
        }
    } else if (strcmp(type->valuestring, "realtime_transcript_done") == 0) {
        const cJSON* role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(role) && cJSON_IsString(text) &&
            (strcmp(role->valuestring, "user") == 0 ||
             strcmp(role->valuestring, "assistant") == 0)) {
            transcript_partial_.clear();
            transcript_role_.clear();
            if (text->valuestring[0] != '\0') {
                // The whole line, now that there is one: the caption stops
                // crawling and scrolls the finished sentence instead.
                EmitTranscript(role->valuestring, text->valuestring);
            }
            ESP_LOGI(TAG, "%s transcript complete", role->valuestring);
            if (strcmp(role->valuestring, "assistant") == 0) {
                // An audible reply has nothing left to recover, so stop
                // watching it - otherwise the silence after it ends would trip
                // the check. A reply that finished its words without ever
                // being heard stays armed, which is the whole point.
                const uint32_t expecting = speech_expected_since_ms_.load();
                if (expecting != 0 && AtOrAfter(last_audio_frame_ms_.load(), expecting)) {
                    speech_expected_since_ms_.store(0);
                }
                StopSpeaking();
            }
        }
    } else if (strcmp(type->valuestring, "realtime_status") == 0) {
        const cJSON* caption = cJSON_GetObjectItemCaseSensitive(root, "caption");
        if (cJSON_IsString(caption) && strlen(caption->valuestring) <= 80) {
            const std::string activity = caption->valuestring;
            const cJSON* icon_value = cJSON_GetObjectItemCaseSensitive(root, "icon");
            std::string icon = "none";
            if (cJSON_IsString(icon_value)) {
                for (const char* allowed : {"none", "search"}) {
                    if (strcmp(icon_value->valuestring, allowed) == 0) { icon = allowed; break; }
                }
            }
            const cJSON* pixels_value = cJSON_GetObjectItemCaseSensitive(root, "iconPixels");
            std::string pixels;
            if (cJSON_IsString(pixels_value) && strlen(pixels_value->valuestring) == 3072) {
                pixels = pixels_value->valuestring;
            }
            const std::string session_id = request_id_;
            Application::GetInstance().Schedule([this, activity, icon, pixels, session_id]() {
                if (IsAudioChannelOpened() && request_id_ == session_id) {
                    Board::GetInstance().GetDisplay()->SetVoiceActivity(activity.c_str(), icon.c_str(), pixels.c_str());
                }
            });
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
    ESP_LOGD(TAG, "Voice event: %s", event_type);
    // Real traffic on the event channel, so the voice session behind it is live
    // rather than merely connected.
    MarkStage(kVoiceStageSessionStarted);
    if (strcmp(event_type, "error") == 0 ||
        strcmp(event_type, "invalid_request_error") == 0) {
        Fail(ReadErrorMessage(root));
    } else if (strcmp(event_type, "turn.created") == 0) {
        const cJSON* turn = cJSON_GetObjectItemCaseSensitive(root, "turn");
        const cJSON* role = cJSON_GetObjectItemCaseSensitive(turn, "role");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
            // A new user turn cancels any reply we were still waiting on.
            speech_expected_since_ms_.store(0);
            const auto session_id = request_id_;
            Application::GetInstance().Schedule([this, session_id]() {
                if (!IsAudioChannelOpened() || request_id_ != session_id) return;
                // Codex has recognized a new user turn; discard only leftover playback.
                Application::GetInstance().GetAudioService().ResetDecoder();
                StopSpeaking();
                ESP_LOGI(TAG, "User turn: cleared pending playback; microphone stays open");
            });
        }
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

namespace {
// What fits on the caption strip at once. The strip scrolls a finished line,
// but a line still being written is replaced every time it grows, which
// restarts the scroll - so while it streams, only the tail is shown, and it
// has to actually FIT. A tail that overruns the strip by even a little starts
// the scroll, and restarting that six times a second is what reads as the
// caption twitching. 18 leaves room for wide characters at the strip's 174px.
constexpr size_t kTranscriptTailChars = 18;
// Redrawing on every token would repaint the screen faster than it can be
// read. Six times a second keeps up with speech.
constexpr uint32_t kTranscriptStreamMs = 160;
// The most of a reply-in-progress worth holding on to. Several times the tail
// that is shown, so trimming never eats into the words being displayed, and
// far short of what an endless stream of deltas could otherwise accumulate.
constexpr size_t kTranscriptKeepBytes = 256;

/* The last words of `text`, cut at a space where there is one, and never
 * through the middle of a multi-byte character. */
std::string TranscriptTail(const std::string& text) {
    if (text.size() <= kTranscriptTailChars) return text;
    size_t cut = text.size() - kTranscriptTailChars;
    const size_t space = text.find(' ', cut);
    if (space != std::string::npos && space + 1 < text.size()) {
        cut = space + 1;
    } else {
        while (cut < text.size() && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) ++cut;
    }
    return text.substr(cut);
}
}  // namespace

/* The reply arrives word by word long before it is finished, and until now the
 * only thing done with a delta was to note that audio should follow - the text
 * itself was dropped and the caption waited for the completed line. Showing it
 * as it lands is the difference between a caption that arrives with the voice
 * and one that arrives after it. */
void CodexVoiceProtocol::StreamTranscript(const char* role, const char* delta) {
    if (transcript_role_ != role) {
        transcript_partial_.clear();
        transcript_role_ = role;
        transcript_emitted_at_ = 0;
    }
    transcript_partial_ += delta;
    /* Only the tail is ever shown, and a peer that never sends a completion
     * event would otherwise grow this without limit. Keep a few tails' worth
     * so a long word cannot be cut into by the trim itself, and cut on a UTF-8
     * boundary so a multi-byte character is never halved. */
    if (transcript_partial_.size() > kTranscriptKeepBytes) {
        size_t cut = transcript_partial_.size() - kTranscriptKeepBytes;
        while (cut < transcript_partial_.size() &&
               (static_cast<unsigned char>(transcript_partial_[cut]) & 0xC0) == 0x80) {
            ++cut;
        }
        transcript_partial_.erase(0, cut);
    }
    const uint32_t now = NowMilliseconds();
    if (transcript_emitted_at_ != 0 && now - transcript_emitted_at_ < kTranscriptStreamMs) return;
    transcript_emitted_at_ = now;
    EmitTranscript(role, TranscriptTail(transcript_partial_).c_str());
}

void CodexVoiceProtocol::MarkStage(uint32_t stage) {
    if (readiness_.Mark(stage)) {
        ESP_LOGI(TAG, "Voice path reached %s (now: %s)", VoiceStageName(stage),
                 readiness_.Describe().c_str());
    }
}

void CodexVoiceProtocol::Fail(const std::string& message) {
    if (closing_ || (xEventGroupGetBits(peer_events_) & kVoiceFailedBit) != 0) {
        return;
    }
    channel_open_ = false;
    ESP_LOGE(TAG, "%s", message.c_str());
    SetError(message);
    xEventGroupSetBits(peer_events_, kVoiceFailedBit);
}

void CodexVoiceProtocol::RunPeerLoop() {
    uint32_t last_query = NowMilliseconds();
    while (peer_running_) {
        esp_peer_main_loop(peer_);
        if (NowMilliseconds() - last_query >= 5000) {
            esp_peer_query(peer_);
            last_query = NowMilliseconds();
        }
        CheckInboundAudioStall();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    xEventGroupSetBits(peer_events_, kPeerStoppedBit);
    vTaskDelete(nullptr);
}

void CodexVoiceProtocol::CheckInboundAudioStall() {
    // The Mac can keep transcribing and captioning a reply while the inbound
    // audio track is wedged, so the user watches words appear in silence. The
    // data channel stays healthy, which means nothing else notices. If speech
    // was expected and no audio frame has arrived for several seconds, the
    // track is not going to recover on its own: fail the call so the normal
    // reconnect path rebuilds it.
    uint32_t expecting = speech_expected_since_ms_.load();
    if (expecting == 0 || closing_ || !IsAudioChannelOpened()) {
        return;
    }
    const uint32_t now = NowMilliseconds();
    const uint32_t last_frame = last_audio_frame_ms_.load();
    // Frames older than the moment we armed do not count: they belong to an
    // earlier reply. This max() is the whole defence against a barge-in's
    // trailing packets passing for the next reply's voice.
    const uint32_t quiet_since = AtOrAfter(last_frame, expecting) ? last_frame : expecting;
    if (now - quiet_since < kInboundAudioStallMs) {
        return;
    }
    // A real frame can arrive between the timeout check and this point. Only
    // the caller that armed this check may consume it and trigger recovery.
    if (!speech_expected_since_ms_.compare_exchange_strong(expecting, 0)) {
        return;
    }
    /* The link that never arrived is the difference between a network fault
     * and a playback fault, so report it rather than only the silence. */
    const uint32_t missing_stage = readiness_.FirstMissing();
    const bool will_retry = stall_retries_.fetch_add(1) < kMaxStallRetries;
    ESP_LOGE(TAG, "No reply audio for %lu ms; reached %s, missing %s",
             (unsigned long)(now - quiet_since), readiness_.Describe().c_str(),
             missing_stage == 0 ? "nothing" : VoiceStageName(missing_stage));
    if (will_retry) {
        stall_recovery_.store(true);
    }
    Fail(StallMessage(missing_stage, will_retry));
}

int CodexVoiceProtocol::OnPeerState(esp_peer_state_t state, void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    ESP_LOGI(TAG, "Peer state: %d", static_cast<int>(state));
    if (state == ESP_PEER_STATE_CONNECTED) {
        protocol->MarkStage(kVoiceStagePeerConnected);
    } else if (state == ESP_PEER_STATE_REMOTE_AUDIO_TRACK_ADDED) {
        protocol->MarkStage(kVoiceStageAudioTrack);
    } else if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
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
    static uint32_t received_frames = 0;
    static uint32_t real_audio_frames = 0;
    static uint32_t last_audio_log = 0;
    static bool previous_frame_was_real_audio = false;
    const uint32_t now = NowMilliseconds();
    const uint32_t previous_audio_frame_ms = protocol->last_audio_frame_ms_.load();
    const bool is_real_audio = frame->size >= kMinimumVoiceAudioBytes;
    const bool new_audio_burst = is_real_audio && !previous_frame_was_real_audio;
    ++received_frames;
    if (is_real_audio) {
        ++real_audio_frames;
        // Either the announced track or its first real frame is enough: both
        // mean the reply's audio is reaching the device.
        protocol->MarkStage(kVoiceStageAudioTrack);
        // A reply arrived, so the run of silent calls is over and the next
        // stall gets a full budget again.
        protocol->stall_retries_.store(0);
        // Only the timestamp. This runs on the WebRTC callback, which knows
        // nothing about which reply the frame belongs to - the stall check
        // compares this against when it started listening and draws its own
        // conclusion.
        protocol->last_audio_frame_ms_.store(now);
    }
    if (now - last_audio_log >= 1000 || new_audio_burst ||
        (is_real_audio && now - previous_audio_frame_ms >= kAudioLogBurstGapMs)) {
        last_audio_log = now;
        ESP_LOGI(TAG, "[DEBUG-audio] received=%lu audio_frames=%lu bytes=%d rate=%d open=%d",
                 (unsigned long)received_frames, (unsigned long)real_audio_frames,
                 (int)frame->size,
                 protocol->server_sample_rate_, protocol->IsAudioChannelOpened());
    }
    previous_frame_was_real_audio = is_real_audio;
    // Schedule stores callbacks in a deque that can be dropped without running
    // (ResetProtocol, shutdown). A raw pointer captured there would have no
    // owner, so the packet and its payload would leak. shared_ptr is needed
    // because Schedule takes a copyable std::function.
    auto packet = std::make_shared<AudioStreamPacket>();
    packet->sample_rate = protocol->server_sample_rate_;
    packet->frame_duration = OpusPacketDurationMs(frame->data, frame->size);
    packet->timestamp = frame->pts;
    packet->payload.assign(frame->data, frame->data + frame->size);
    packet->pcm = false;
    auto callback = protocol->on_incoming_audio_;
    Application::GetInstance().Schedule([callback, packet]() {
        callback(std::make_unique<AudioStreamPacket>(std::move(*packet)));
    });
    return 0;
}

int CodexVoiceProtocol::OnDataChannelOpen(esp_peer_data_channel_info_t* channel,
                                          void* context) {
    auto* protocol = static_cast<CodexVoiceProtocol*>(context);
    if (!protocol->closing_ &&
        (xEventGroupGetBits(protocol->peer_events_) & kVoiceFailedBit) == 0 &&
        channel != nullptr && channel->label != nullptr &&
        strcmp(channel->label, "oai-events") == 0) {
        protocol->channel_open_ = true;
        protocol->MarkStage(kVoiceStageEventChannel);
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
