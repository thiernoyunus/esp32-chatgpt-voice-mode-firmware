#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "assets/sound_variants.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "display/confirm_geometry.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "codex_voice_protocol.h"
#include "display/voice_geometry.h"
#include "display/lcd_display.h"
#include "display/voice_character.h"

#include <driver/gpio.h>
#include <esp_log.h>
#ifdef CONFIG_VOICEMODE_PROTOCOL
#include <esp_app_desc.h>
#include <esp_netif_sntp.h>
#include <wifi_manager.h>
#endif
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>

#define TAG "Application"

#ifdef CONFIG_VOICEMODE_PROTOCOL
// The backlight is the single biggest draw on this board, so it goes first.
static constexpr int kScreenSleepAfterSeconds = 60;
// Slow enough that a server that keeps hanging up does not turn into a
// reconnect loop, quick enough that the channel is ready when a hand arrives.
static constexpr int kChannelReopenIntervalSeconds = 10;
// Draining the encoder after the finger lifts: a few short waits rather than one
// long one, so a quiet tail costs nothing but a real one still gets through.
static constexpr int kListenFlushAttempts = 6;
static constexpr int kListenFlushWaitMs = 40;
static constexpr int kTelemetryIntervalSeconds = 60;
#endif

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
#if CONFIG_USE_DEVICE_AEC
    audio_service_.EnableDeviceAec(true);
#endif
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        const int64_t now = esp_timer_get_time();
        vad_in_speech_.store(speaking);
        if (speaking) {
            vad_last_onset_us_.store(now);
        } else {
            vad_last_offset_us_.store(now);
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_LISTEN_WATCHDOG;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            // A reply transcribed but never heard is the one failure a fresh
            // call fixes, and the message already promised it. Nothing did it.
            bool reopen = false;
            if (auto* voice = dynamic_cast<CodexVoiceProtocol*>(protocol_.get())) {
                reopen = voice->TakeStallRecovery() && !call_end_requested_.load();
            }
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
            SetDeviceState(kDeviceStateIdle);
            // Going idle queues a state change whose handler blanks the screen
            // back to STANDBY. Drain it here, before the alert is drawn, or the
            // next loop pass erases the very message this event exists to show.
            xEventGroupClearBits(event_group_, MAIN_EVENT_STATE_CHANGED);
            HandleStateChangedEvent();
            if (reopen) {
                ESP_LOGW(TAG, "Reply audio stalled; reopening the call");
                // Idle is the state HandleToggleChatEvent opens a call from,
                // and it is the state we just landed in. The flag is checked
                // again inside: this runs a loop pass later, the UI task can
                // hang up in between, and HandleToggleChatEvent clears it.
                Schedule([this]() {
                    if (!call_end_requested_.load()) {
                        HandleToggleChatEvent();
                    }
                });
            } else {
                Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                      Lang::Sounds::OGG_EXCLAMATION);
            }
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }

            // Deferred end of speech: the reply had finished arriving long
            // before it finished playing.
            if (pending_speech_stop_ && GetDeviceState() == kDeviceStateSpeaking &&
                audio_service_.IsPlaybackIdle()) {
                pending_speech_stop_ = false;
                FinishSpeaking();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_LISTEN_WATCHDOG) {
            HandleListenWatchdogEvent();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            if (clock_ticks_ % 3 == 0) RefreshWatchInfo();

#ifdef CONFIG_VOICEMODE_PROTOCOL
            if (GetDeviceState() == kDeviceStateIdle) {
                idle_seconds_++;
                if (screen_sleep_seconds_ > 0 && idle_seconds_ >= screen_sleep_seconds_) {
                    SleepScreen();
                }
            } else {
                // Anything but idle is the device working for the user.
                NoteUserActivity();
            }
            MaybeSendTelemetry();
            // Playback acks close the server's pacing loop. Speaking covers the
            // post-run drain too: FinishSpeaking is deferred until the queue
            // empties, and effect playback outside a turn must not ack.
            if (protocol_ != nullptr && protocol_->IsAudioChannelOpened() &&
                GetDeviceState() == kDeviceStateSpeaking) {
                protocol_->SendPlaybackAck(audio_service_.played_tts_milliseconds());
            }
#endif

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::MaybeSendTelemetry() {
#ifdef CONFIG_VOICEMODE_PROTOCOL
    if (protocol_ == nullptr || !protocol_->IsAudioChannelOpened()) {
        telemetry_sent_since_open_ = false;
        return;
    }

    auto& board = Board::GetInstance();
    DeviceTelemetry telemetry;
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        telemetry.battery_level = battery_level;
        telemetry.battery_valid = true;
        telemetry.charging = charging;
        telemetry.charging_valid = true;
    }
    telemetry.volume = board.GetAudioCodec()->output_volume();
    telemetry.wifi_rssi = WifiManager::GetInstance().GetRssi();
    telemetry.firmware_version = esp_app_get_description()->version;

    bool is_first_since_open = !telemetry_sent_since_open_;
    bool is_interval_due = clock_ticks_ - last_telemetry_ticks_ >= kTelemetryIntervalSeconds;
    // A charging edge jumps the queue: waiting a minute to learn the cable was
    // plugged or pulled makes the server's reactions feel broken.
    bool did_charging_flip = !is_first_since_open && telemetry.charging_valid &&
                             telemetry.charging != last_reported_charging_;
    if (!is_first_since_open && !is_interval_due && !did_charging_flip) {
        return;
    }

    telemetry_sent_since_open_ = true;
    last_telemetry_ticks_ = clock_ticks_;
    if (telemetry.charging_valid) {
        last_reported_charging_ = telemetry.charging;
    }
    protocol_->SendTelemetry(telemetry);
#endif
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
        // A dropped socket cannot finish the current listen or speech run. If
        // the state is left untouched, the listening animation can remain on
        // screen forever with no server left to end it.
        SetDeviceState(kDeviceStateIdle);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    std::string version = esp_app_get_description()->version;
#ifdef CONFIG_VOICEMODE_PROTOCOL
    // ota_ is still in use by the activation task's deferred firmware check;
    // it also owns releasing it. Server time comes from SNTP under voice mode.
#else
    has_server_time_ = ota_->HasServerTime();
    version = ota_->GetCurrentVersion();
    ota_.reset();
#endif

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + version;
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process. Still constructed under voice mode:
    // HandleActivationDoneEvent reads the version and server time off it.
    ota_ = std::make_unique<Ota>();

    // Mounts the assets partition as a side effect of first touching the
    // Assets singleton, so it runs under voice mode too: skipping it left the
    // emote engine with no animations to play at all. The network path inside
    // only runs when a download url was explicitly stored.
    CheckAssetsVersion();

#ifndef CONFIG_VOICEMODE_PROTOCOL
    // Check for new firmware version
    CheckNewVersion();
#else
    // Voice mode is configured locally and has no activation service, so the
    // xiaozhi handshake is skipped entirely: running it would block here
    // against api.tenclass.net waiting for a device registration that will
    // never happen.
    ESP_LOGI(TAG, "Voice mode selected, skipping OTA activation");
    InitializeSystemTime();
#endif

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);

#ifdef CONFIG_VOICEMODE_PROTOCOL
    // After the ready signal on purpose: the check is a full HTTPS round trip
    // (~2.5 s) and the device is perfectly usable while it runs. ota_ is
    // released here, not in the handler, so the check can't race its owner.
    CheckFirmwareUpdate();
    ota_.reset();
#endif
}

#ifdef CONFIG_VOICEMODE_PROTOCOL
void Application::CheckFirmwareUpdate() {
    // First and unconditionally: with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
    // an image installed over the air boots as pending-verify and reverts on
    // the next reboot unless it is marked valid. The xiaozhi path that did
    // this lives in CheckNewVersion, which is compiled out under voice mode.
    ota_->MarkCurrentVersionValid();
    if (ota_->RolledBack()) {
        /* Said in the words someone who did not build this would use. The boot
         * check below asks the server for the current version and reinstalls
         * if there is one, so this is a report of something already being put
         * right, not a dead end. */
        pending_watch_notification_ = "Update didn't finish. Running your last working version.";
    }

    Settings settings("voicemode", false);
    std::string base_url = settings.GetString("url");
    if (base_url.empty()) {
        base_url = CONFIG_VOICEMODE_URL;
    }
    std::string token = settings.GetString("token");
    if (token.empty()) {
        token = CONFIG_VOICEMODE_TOKEN;
    }
    if (base_url.empty() || token.empty()) {
        ESP_LOGW(TAG, "Voice mode url or token not configured, skipping firmware check");
        return;
    }

    if (base_url.rfind("wss://", 0) == 0) {
        base_url = "https://" + base_url.substr(6);
    } else if (base_url.rfind("ws://", 0) == 0) {
        base_url = "http://" + base_url.substr(5);
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    ota_->SetCheckVersionUrl(base_url + "/ota/check?token=" + token);

    // A single attempt with no retries: unlike xiaozhi's activation loop, a
    // failed check must never hold the device in kDeviceStateActivating.
    esp_err_t error = ota_->CheckVersion();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Firmware check failed (0x%x), continuing boot", error);
        return;
    }
    if (!ota_->HasNewVersion()) {
        ESP_LOGI(TAG, "Firmware %s is current", ota_->GetCurrentVersion().c_str());
        return;
    }
    ESP_LOGI(TAG, "Upgrading firmware %s -> %s", ota_->GetCurrentVersion().c_str(),
             ota_->GetFirmwareVersion().c_str());
    UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion());
}
#endif

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");

}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // The connection is configured from NVS, not from an OTA config response. The
    // upstream MQTT/websocket protocols are gone from this fork: the voice
    // dialect is the only one the device speaks.
    protocol_ = std::make_unique<CodexVoiceProtocol>();

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        // Live audio and captions arrive independently; captions must not gate playback.
        if (!protocol_->IsAudioChannelOpened()) {
            return;
        }
        const DeviceState state = GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            if (audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
                protocol_->MarkPlaybackAdmitted();
            } else {
                /* The speaker queue was full and this frame is gone. Say so: a
                 * reply that goes quiet mid-sentence looks like a network
                 * fault, and this is the one place that can tell the two
                 * apart. */
                static int64_t last_full_report_us = 0;
                const int64_t now_us = esp_timer_get_time();
                if (now_us - last_full_report_us >= 1000000) {
                    last_full_report_us = now_us;
                    ESP_LOGW(TAG, "Speaker queue full; dropped a frame of the reply");
                }
            }
            return;
        }
        /* The greeting lands while the call is still connecting, before the
         * device starts listening - and starting to listen clears the speaker
         * queues, which would take that greeting with it. Hold it instead. */
        if (state == kDeviceStateConnecting) {
            voice_preroll_.Push(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this]() {
        Schedule([this]() {
            // An old call can finish closing after the next call has opened.
            if (protocol_->IsAudioChannelOpened()) {
                return;
            }
            audio_service_.ResetDecoder();
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            DismissConfirm();
            if (GetDeviceState() == kDeviceStateIdle) {
                return;
            }
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    /* Three kinds of message reach this, and all three are made by the voice
     * protocol itself rather than sent over the wire: "tts" and "stt" are the
     * captions it builds from the call's transcripts, and "mcp" is a request
     * for one of this device's own tools. The dialect this once spoke was much
     * larger - emotions, accent colours, countdown arcs, system commands,
     * alerts - and every one of those came from a server that no longer
     * exists. */
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    audio_service_.ResetPlayedTtsMilliseconds();
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() != kDeviceStateSpeaking) {
                        return;
                    }
                    FinishSpeaking();
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    call_end_requested_.store(false);
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting || call_end_requested_.load()) {
        return;
    }
    Board::GetInstance().GetDisplay()->ShowVoicePage();

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    if (call_end_requested_.load()) {
        protocol_->CloseAudioChannel();
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

#ifdef CONFIG_VOICEMODE_PROTOCOL
void Application::InitializeSystemTime() {
    // Fire and forget: the clock is only used for the on-screen display, so
    // nothing here should delay reaching the idle state.
    setenv("TZ", CONFIG_VOICEMODE_TIMEZONE, 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_VOICEMODE_NTP_SERVER);
    sntp_config.start = true;
    sntp_config.server_from_dhcp = false;
    auto err = esp_netif_sntp_init(&sntp_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started against %s (TZ %s)", CONFIG_VOICEMODE_NTP_SERVER,
             CONFIG_VOICEMODE_TIMEZONE);
}
#endif

void Application::SleepScreen() {
    if (is_screen_asleep_) {
        return;
    }
    is_screen_asleep_ = true;
    ESP_LOGI(TAG, "Screen asleep after %d s idle", idle_seconds_);

    auto& board = Board::GetInstance();
    board.GetDisplay()->SetPowerSaveMode(true);
    auto backlight = board.GetBacklight();
    if (backlight != nullptr) {
        // Not permanent: the configured brightness has to survive the nap.
        backlight->SetBrightness(0);
    }
}

void Application::NoteUserActivity() {
    idle_seconds_ = 0;
    if (!is_screen_asleep_) {
        return;
    }
    is_screen_asleep_ = false;
    ESP_LOGI(TAG, "Screen awake");

    auto& board = Board::GetInstance();
    auto backlight = board.GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();
    }
    board.GetDisplay()->SetPowerSaveMode(false);
}

void Application::OnVoiceTouchRelease(int x, int y) {
    Schedule([this, x, y]() {
        const bool was_asleep = is_screen_asleep_;
        NoteUserActivity();
        if (was_asleep || !protocol_ || IsConfirmActive()) {
            return;
        }
        if (!protocol_->IsAudioChannelOpened()) {
            HandleToggleChatEvent();
            return;
        }
        if (voice_geometry::ContainsButton(voice_geometry::kMuteLeft, x, y)) {
            // Mute and the send-queue drain both run on this main task.
            const bool muted = !audio_service_.IsMicrophoneMuted();
            audio_service_.SetMicrophoneMuted(muted);
            Board::GetInstance().GetDisplay()->SetVoiceMicrophoneMuted(muted);
        } else if (voice_geometry::ContainsButton(voice_geometry::kEndLeft, x, y)) {
            // End means end. HandleToggleChatEvent only aborts speech while
            // Speaking, which left the channel open. Same steps as
            // WatchUi::Action::EndCall.
            call_end_requested_.store(true);
            protocol_->CloseAudioChannel();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::ShowConfirm(const std::string& summary, uint32_t timeout_ms) {
    Schedule([this, summary, timeout_ms]() {
        NoteUserActivity();
        if (confirm_expiry_timer_ == nullptr) {
            const esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) { static_cast<Application*>(arg)->DismissConfirm(); },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "confirm_expiry",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &confirm_expiry_timer_));
        }
        esp_timer_stop(confirm_expiry_timer_);
        confirm_active_.store(true);
        Board::GetInstance().GetDisplay()->ShowConfirmScreen(summary.c_str());
        ESP_ERROR_CHECK(esp_timer_start_once(confirm_expiry_timer_, (uint64_t)timeout_ms * 1000));
    });
}

bool Application::TakeConfirmSession() {
    if (!confirm_active_.exchange(false)) {
        return false;
    }
    if (confirm_expiry_timer_ != nullptr) {
        esp_timer_stop(confirm_expiry_timer_);
    }
    return true;
}

void Application::DismissConfirm() {
    if (!TakeConfirmSession()) {
        return;
    }
    Schedule([]() { Board::GetInstance().GetDisplay()->HideConfirmScreen(); });
}

void Application::OnConfirmTouchRelease(int x, int y) {
    const bool is_approved = confirm_geometry::IsInsideApproveZone(x, y);
    if (!is_approved && !confirm_geometry::IsInsideRejectZone(x, y)) {
        return;
    }
    if (!TakeConfirmSession()) {
        return;
    }
    Schedule([this, is_approved]() {
        Board::GetInstance().GetDisplay()->HideConfirmScreen();
        // The summary's own TTS is usually still playing; an answer means the
        // user heard enough.
        if (GetDeviceState() == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonNone);
            audio_service_.ResetDecoder();
            SetDeviceState(kDeviceStateIdle);
        }
        if (protocol_ != nullptr) {
            protocol_->SendConfirm(is_approved);
        }
        PlaySound(is_approved ? Lang::Sounds::OGG_SUCCESS : Lang::Sounds::OGG_POPUP);
    });
}

void Application::FinishSpeaking() {
    if (listening_mode_ == kListeningModeManualStop) {
        // An interruption is not a completion, so no cue on abort. In auto
        // mode the listening branch plays its own cue, and this one would be
        // cleared by the decoder reset anyway.
        if (!aborted_) {
            audio_service_.PlaySound(Lang::SoundVariants::SpeechDone());
        }
        SetDeviceState(kDeviceStateIdle);
    } else if (reopen_listening_after_speak_) {
        SetDeviceState(kDeviceStateListening);
    } else {
        // The server's turn_end said the reply expects no answer, so the
        // conversation is over instead of looping back to the microphone.
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::OnTurnEnd(bool expects_reply) {
    Schedule([this, expects_reply]() {
        reopen_listening_after_speak_ = expects_reply;
        // The no-answer verdict can land while the mic is already open again
        // (short replies drain before turn_end arrives); close it too.
        if (!expects_reply && GetDeviceState() == kDeviceStateListening &&
            listening_mode_ != kListeningModeManualStop) {
            CancelListening();
        }
    });
}

void Application::StartListenWatchdog() {
    if (listening_mode_ != kListeningModeAutoStop) {
        return;
    }
    if (listen_watchdog_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto* app = static_cast<Application*>(arg);
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_LISTEN_WATCHDOG);
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "listen_watchdog",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &listen_watchdog_timer_));
    }
    listen_started_us_ = esp_timer_get_time();
    listen_heard_speech_ = false;
    vad_in_speech_.store(false);
    vad_last_onset_us_.store(0);
    vad_last_offset_us_.store(0);
    esp_timer_stop(listen_watchdog_timer_);
    ESP_ERROR_CHECK(esp_timer_start_periodic(listen_watchdog_timer_, 250 * 1000));
}

void Application::HandleListenWatchdogEvent() {
    if (GetDeviceState() != kDeviceStateListening ||
        listening_mode_ != kListeningModeAutoStop) {
        return;
    }
    // No AEC runs while listening, so the ListenStart cue registers on the VAD
    // as speech; activity inside the grace window is the device hearing itself.
    constexpr int64_t kCueGraceUs = 700 * 1000;
    constexpr int64_t kMinSpeechUs = 300 * 1000;
    constexpr int64_t kEndpointSilenceUs = 1200 * 1000;
    // Counted from mic-open, so after the cue+grace (~0.7 s) the user has
    // about 2.3 s to start talking before the session gives up.
    constexpr int64_t kNoSpeechTimeoutUs = 3000 * 1000;

    const int64_t now = esp_timer_get_time();
    const bool in_speech = vad_in_speech_.load();
    const int64_t onset = vad_last_onset_us_.load();
    const int64_t offset = vad_last_offset_us_.load();
    const int64_t effective_onset = std::max(onset, listen_started_us_ + kCueGraceUs);

    if (onset > 0) {
        const int64_t speech_end = in_speech ? now : offset;
        if (speech_end - effective_onset >= kMinSpeechUs) {
            listen_heard_speech_ = true;
        }
    }

    if (listen_heard_speech_) {
        if (!in_speech && offset > 0 && now - offset >= kEndpointSilenceUs) {
            ESP_LOGI(TAG, "VAD endpoint reached, committing the turn");
            StopListening();
        }
    } else if (now - listen_started_us_ >= kNoSpeechTimeoutUs) {
        ESP_LOGI(TAG, "Nothing said, cancelling the listen session");
        CancelListening();
    }
}

void Application::CancelListening() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    if (protocol_) {
        protocol_->SendListenCancel();
    }
    SetDeviceState(kDeviceStateIdle);
    audio_service_.PlaySound(Lang::SoundVariants::ListenEnd());
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateConnecting) {
        // A short hold can end while its audio socket is still opening. Record
        // that release by leaving connecting now; the queued continuation
        // checks the state and will no longer open the mic after the finger is
        // already gone.
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateListening) {
        // hold_end is what makes the server transcribe, so everything recorded
        // has to be on the wire before it goes out. The tail of an utterance is
        // still working its way through the encoder when the finger lifts, and
        // anything arriving after hold_end is simply not part of the turn.
        if (protocol_) {
            for (int attempt = 0; attempt < kListenFlushAttempts; ++attempt) {
                bool sent_any = false;
                while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                    sent_any = true;
                    if (!protocol_->SendAudio(std::move(packet))) {
                        break;
                    }
                }
                if (!sent_any && attempt > 0) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(kListenFlushWaitMs));
            }
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
        // The idle transition disables voice processing without resetting the
        // decoder, so a cue queued here survives it.
        audio_service_.PlaySound(Lang::SoundVariants::ListenEnd());
    }
}

void Application::HandleWakeWordDetectedEvent() {
#ifdef CONFIG_VOICEMODE_PROTOCOL
    // Saying the wake word to a dark device should light it up, whatever the
    // rest of this handler decides to do about the turn itself.
    NoteUserActivity();
#endif
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::SoundVariants::ListenStart());
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
#ifndef CONFIG_VOICEMODE_PROTOCOL
    audio_service_.EncodeWakeWord();
#else
    // Voice mode starts a fresh raw-PCM stream after the wake event. Encoding the
    // cached wake phrase as Opus only delays that event, and it is discarded.
#endif

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }
    call_end_requested_.store(false);
    Board::GetInstance().GetDisplay()->ShowVoicePage();

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA && !defined(CONFIG_VOICEMODE_PROTOCOL)
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;
    // The watchdog only polices an open auto-mode mic; StartListeningAudio
    // re-arms it when the mic actually opens.
    if (listen_watchdog_timer_ != nullptr) {
        esp_timer_stop(listen_watchdog_timer_);
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // A call that ended before it started must not speak into the next one.
            voice_preroll_.Clear();
            voice_model_picker_open_ = false;
            display->HideVoiceModels();
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            // SetStatus already raises EMOTE_MGR_EVT_LISTEN, which drives the
            // listen_anim object. Setting a "listening" emotion on top of that
            // replaces the face itself with listen.eaf, which is not a face and
            // reads as an error.
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Every listening start gets the cue, held button or wake word alike. It
    // has to play after ResetDecoder (in EnableVoiceProcessing) or it would be
    // cleared before it sounds.
    play_popup_on_listening_ = false;
    audio_service_.PlaySound(Lang::SoundVariants::ListenStart());

    // Each new listen session starts optimistic; the reply's turn_end says
    // whether the mic reopens after the assistant speaks.
    reopen_listening_after_speak_ = true;
    StartListenWatchdog();
    // After the queue clear above, never before: anything held for this call is
    // the beginning of the reply the user is waiting to hear.
    FlushVoicePreroll();
}

void Application::FlushVoicePreroll() {
    if (voice_preroll_.Empty()) {
        return;
    }
    const size_t held = voice_preroll_.Size();
    const size_t delivered = voice_preroll_.Flush(
        [this](std::unique_ptr<AudioStreamPacket> packet) {
            return audio_service_.PushPacketToDecodeQueue(std::move(packet));
        });
    if (delivered > 0) {
        protocol_->MarkPlaybackAdmitted();
    }
    if (delivered < held) {
        ESP_LOGW(TAG, "Speaker queue full while opening the call; %u of %u opening frames were dropped",
                 (unsigned) (held - delivered), (unsigned) held);
    } else {
        ESP_LOGI(TAG, "Playing %u opening frames held since the call connected", (unsigned) delivered);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

void Application::RefreshWatchInfo() {
    auto& board = Board::GetInstance();
    auto display = dynamic_cast<LcdDisplay*>(board.GetDisplay());
    if (!display) return;
    WatchUi::Info info;
    Settings saved_display("display", false);
    info.brightness = saved_display.GetInt("brightness", 75);
    info.sleep_seconds = saved_display.GetInt("sleep_seconds", 60);
    info.shape = std::clamp<int32_t>(saved_display.GetInt("voice_shape", 0), 0,
                                     voice_character::kShapeCount - 1);
    info.colour = std::clamp<int32_t>(saved_display.GetInt("voice_colour", 0), 0,
                                      voice_character::kColorCount - 1);
    screen_sleep_seconds_ = info.sleep_seconds;
    if (auto codec = board.GetAudioCodec()) info.volume = codec->output_volume();
    bool discharging = false;
    if (!board.GetBatteryLevel(info.battery, info.charging, discharging)) info.battery = -1;
    info.network = board.GetCurrentWifiNetwork();
    info.connected = !info.network.empty();
    info.wifi_status = board.GetWifiStatus();
    info.saved_networks = board.GetSavedWifiNetworks();
    info.networks = board.GetAvailableWifiNetworks();
    for (const auto& name : info.saved_networks)
        if (std::find(info.networks.begin(), info.networks.end(), name) == info.networks.end())
            info.networks.push_back(name);
    info.version = esp_app_get_description()->version;
    if (ota_ != nullptr) {
        info.slot = ota_->GetRunningSlot();
        info.rolled_back = ota_->RolledBack();
    }
    if (auto voice = dynamic_cast<CodexVoiceProtocol*>(protocol_.get())) {
        Settings settings("codex_voice", false);
        const std::string selected = settings.GetString("model", "");
        for (const auto& model : voice->GetModels()) {
            info.models.push_back(model.name);
            if (model.id == selected) info.model = model.name;
        }
        const std::string selected_chat = settings.GetString("chat", "");
        for (const auto& chat : voice->GetChats()) {
            info.chats.push_back(chat.name);
            if (chat.id == selected_chat) info.chat = chat.name;
        }
    }
    {
        Settings settings("codex_voice", false);
        info.temporary_chat = settings.GetBool("temporary", false);
        info.captions = settings.GetBool("captions", true);
        info.voice = settings.GetString("voice", "");
    }
    {
        Settings codex("codex", false);
        info.reasoning = codex.GetString("reasoning", "Default");
    }
    info.notice = pending_watch_notification_;
    if (!pending_watch_notification_.empty()) {
        ESP_LOGI(TAG, "Watch notification: %s", pending_watch_notification_.c_str());
        pending_watch_notification_.clear();
    }
    display->UpdateWatchInfo(info);
}
void Application::OnWatchAction(WatchUi::Action action, int value,
                                const std::string& text, const std::string& secret) {
    if (action == WatchUi::Action::EndCall) call_end_requested_.store(true);
    Schedule([this, action, value, text, secret]() {
        NoteUserActivity();
        auto& board = Board::GetInstance();
        const bool open = protocol_ && protocol_->IsAudioChannelOpened();
        switch (action) {
            case WatchUi::Action::OpenVoice:
                if (!open && GetDeviceState() == kDeviceStateIdle) HandleToggleChatEvent();
                break;
            case WatchUi::Action::EndCall:
                call_end_requested_.store(true);
                if (protocol_) protocol_->CloseAudioChannel();
                if (GetDeviceState() == kDeviceStateConnecting || open) SetDeviceState(kDeviceStateIdle);
                break;
            case WatchUi::Action::Mute: {
                if (!open) break;
                const bool muted = !audio_service_.IsMicrophoneMuted();
                audio_service_.SetMicrophoneMuted(muted);
                board.GetDisplay()->SetVoiceMicrophoneMuted(muted);
                break;
            }
            case WatchUi::Action::Brightness:
                if (auto backlight = board.GetBacklight()) backlight->SetBrightness(std::clamp(value, 5, 100), true);
                break;
            case WatchUi::Action::Volume:
                if (auto codec = board.GetAudioCodec()) codec->SetOutputVolume(std::clamp(value, 0, 100));
                break;
            case WatchUi::Action::ScanWifi:
                if (open || GetDeviceState() == kDeviceStateConnecting) {
                    call_end_requested_.store(true);
                    protocol_->CloseAudioChannel();
                    SetDeviceState(kDeviceStateIdle);
                }
                if (!board.ScanWifiNetworks())
                    pending_watch_notification_ = "Wi-Fi is busy. Try again shortly.";
                break;
            case WatchUi::Action::JoinWifi:
                if (protocol_) protocol_->CloseAudioChannel();
                if (open || GetDeviceState() == kDeviceStateConnecting) SetDeviceState(kDeviceStateIdle);
                if (!(value == 1 ? board.ConnectSavedWifiNetwork(text) : board.ConnectWifiNetwork(text, secret)))
                    pending_watch_notification_ = "Wi-Fi request could not start";
                break;
            case WatchUi::Action::SetupWifi:
                if (protocol_) protocol_->CloseAudioChannel();
                board.EnterWifiConfigMode();
                break;
            case WatchUi::Action::SelectModel:
                if (auto voice = dynamic_cast<CodexVoiceProtocol*>(protocol_.get())) {
                    if (value >= 0 && voice->SelectModel(static_cast<size_t>(value))) {
                        pending_watch_notification_ = "Model saved for next call";
                    }
                }
                break;
            case WatchUi::Action::SelectChat:
                if (auto voice = dynamic_cast<CodexVoiceProtocol*>(protocol_.get())) {
                    if (value >= 0 && voice->SelectChat(static_cast<size_t>(value))) {
                        pending_watch_notification_ =
                            value == 0 ? "Next call starts a new chat" : "Next call continues this chat";
                    }
                }
                break;
            case WatchUi::Action::Captions: {
                Settings s("codex_voice", true);
                s.SetBool("captions", value != 0);
                if (auto* lcd = dynamic_cast<LcdDisplay*>(board.GetDisplay())) {
                    lcd->SetHideSubtitle(value == 0);
                }
                pending_watch_notification_ =
                    value != 0 ? "Captions on" : "Captions off";
                break;
            }
            case WatchUi::Action::TemporaryChat: {
                Settings s("codex_voice", true);
                s.SetBool("temporary", value != 0);
                if (value != 0) s.SetString("chat", "");
                pending_watch_notification_ =
                    value != 0 ? "Calls stay out of Codex" : "Calls are saved in Codex";
                break;
            }
            case WatchUi::Action::SelectReasoning: {
                Settings s("codex", true);
                s.SetString("reasoning", text);
                pending_watch_notification_ = "Reasoning saved for next call";
                break;
            }
            case WatchUi::Action::SelectVoice: {
                Settings s("codex_voice", true);
                s.SetString("voice", text);
                pending_watch_notification_ =
                    text.empty() ? "Default voice for next call" : "Voice saved for next call";
                break;
            }
            case WatchUi::Action::SelectShape:
            case WatchUi::Action::SelectColour: {
                Settings s("display", true);
                int shape = s.GetInt("voice_shape", 0);
                int colour = s.GetInt("voice_colour", 0);
                if (action == WatchUi::Action::SelectShape) {
                    shape = std::clamp(value, 0, voice_character::kShapeCount - 1);
                } else {
                    colour = std::clamp(value, 0, voice_character::kColorCount - 1);
                }
                s.SetInt("voice_shape", shape);
                s.SetInt("voice_colour", colour);
                if (auto lcd = dynamic_cast<LcdDisplay*>(board.GetDisplay())) lcd->SetVoiceCharacter(shape, colour);
                pending_watch_notification_ = action == WatchUi::Action::SelectShape ? "Shape saved" : "Colour saved";
                break;
            }
            case WatchUi::Action::Sleep:
                if (value != 0 && value != 30 && value != 60 && value != 120 && value != 300) break;
                screen_sleep_seconds_ = value;
                { Settings s("display", true); s.SetInt("sleep_seconds", value); }
                break;
            default: break;
        }
        RefreshWatchInfo();
    });
}
