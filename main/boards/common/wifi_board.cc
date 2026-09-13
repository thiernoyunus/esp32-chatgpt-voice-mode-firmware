#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <utility>
#include <algorithm>
#include <cstring>
#include <cstdlib>

#include <material_symbols.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
    ClearRawConnection();
    UnregisterManualJoinHandlers();
    StopRawWifi();
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    config.show_ota_config = true;
    config.show_sleep_config = true;

    // Set a DHCP hostname so the router shows a friendly name instead of "espressif".
    // Uses the same "<prefix>-<last 2 MAC bytes>" scheme as the config AP SSID.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4], mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode. On-demand scans suspend the manager
    // station first, so only one handler consumes the driver's scan records.
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        station_attempt_generation_.fetch_add(1);
        station_attempt_active_.store(true);
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        SetWifiStatus("Scanning...");
        esp_timer_stop(connect_timer_);
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        station_attempt_generation_.fetch_add(1);
        station_attempt_active_.store(false);
        // No SSID configured, enter config mode
        SetWifiStatus("Wi-Fi setup mode");
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    // ScheduleOutcome already updates wifi_status_ for direct-join outcomes,
    // so here we only handle events that come from the WifiManager (i.e. the
    // normal station flow). No "don't clobber manual status" guard is needed
    // because ScheduleOutcome clears manual_join_ before the wifi manager ever
    // observes the next event.
    switch (event) {
        case NetworkEvent::Connected:
            esp_timer_stop(connect_timer_);
            station_attempt_active_.store(false);
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            SetWifiStatus(data.empty() ? std::string("Connected")
                                       : std::string("Connected to ") + data);
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            SetWifiStatus("Scanning...");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            SetWifiStatus(data.empty() ? std::string("Connecting...")
                                       : std::string("Connecting to ") + data);
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            SetWifiStatus("Disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            station_attempt_active_.store(false);
            SetWifiStatus("Wi-Fi setup mode");
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            SetWifiStatus("Scanning...");
            // The manager event runs on the Wi-Fi event task. Start the next
            // station attempt from Application's main task.
            Application::GetInstance().Schedule([this]() { TryWifiConnect(); });
            break;
        default:
            break;
    }

    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    bool manual_active = false;
    {
        std::lock_guard<std::mutex> lock(board->manual_join_mutex_);
        manual_active = board->manual_join_.active;
    }
    if (manual_active) {
        // The user explicitly asked us to join a network. Do not fall back to
        // config mode on a slow AP — abort via the same main-task path the
        // wifi event handlers use, with the generation guard.
        ESP_LOGW(TAG, "Manual join timed out; credential NOT saved");
        const uint32_t generation = board->manual_join_generation_.load();
        board->ScheduleOutcome(generation, false, std::string(), std::string());
        return;
    }

    bool raw_active = false;
    bool raw_connected = false;
    uint32_t raw_generation = 0;
    {
        std::lock_guard<std::mutex> lock(board->raw_connection_mutex_);
        raw_active = board->raw_connection_.active;
        raw_connected = board->raw_connection_.connected;
        raw_generation = board->raw_connection_generation_.load();
    }
    if (raw_active) {
        // A raw connection uses this timer only while a reconnect attempt is
        // waiting for IP. A connected raw station has already stopped it.
        if (raw_connected) return;
        Application::GetInstance().Schedule([board, raw_generation]() {
            std::string ssid;
            {
                std::lock_guard<std::mutex> lock(board->raw_connection_mutex_);
                if (!board->raw_connection_.active ||
                    board->raw_connection_.connected ||
                    board->raw_connection_generation_.load() != raw_generation) {
                    return;
                }
                ssid = board->raw_connection_.ssid;
                board->raw_connection_generation_.fetch_add(1);
                board->raw_connection_ = {};
            }
            ESP_LOGW(TAG, "WiFi reconnect timed out for %s; returning to manager",
                     ssid.c_str());
            board->UnregisterManualJoinHandlers();
            esp_timer_stop(board->connect_timer_);
            board->StopRawWifi();
            board->SetWifiStatus("Wi-Fi reconnect failed");
            board->OnNetworkEvent(NetworkEvent::Disconnected);
            if (!WifiManager::GetInstance().IsConfigMode()) board->TryWifiConnect();
        });
        return;
    }

    if (!board->station_attempt_active_.load()) return;
    const uint32_t generation = board->station_attempt_generation_.load();
    Application::GetInstance().Schedule([board, generation]() {
        if (board->station_attempt_generation_.load() != generation ||
            !board->station_attempt_active_.exchange(false)) {
            return;
        }
        ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");
        esp_timer_stop(board->connect_timer_);
        WifiManager::GetInstance().StopStation();
        board->StartWifiConfigMode();
    });
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
}

void WifiBoard::EnterWifiConfigMode() {
    Application::GetInstance().Schedule([this]() {
        ESP_LOGI(TAG, "EnterWifiConfigMode called");
        GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

        auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
            state == kDeviceStateIdle) {
            // Queue protocol shutdown before Wi-Fi teardown. Both mutations
            // stay on Application's main task and keep the voice path ordered.
            Application::GetInstance().ResetProtocol();
            Application::GetInstance().Schedule([this]() {
                CancelWifiOperations();
                StartWifiConfigMode();
            });
            return;
        }

        if (state != kDeviceStateStarting) {
            ESP_LOGE(TAG, "EnterWifiConfigMode called in unsupported device state: %d", state);
            return;
        }

        CancelWifiOperations();
        StartWifiConfigMode();
    });
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (!IsWifiConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }

    int rssi = GetWifiRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + GetCurrentWifiNetwork() + R"(",)";
        json += R"("rssi":)" + std::to_string(GetWifiRssi()) + R"(,)";
        // Channel/ip come from the WifiManager because the raw-netif path
        // does not track them; they report 0 / "" while WifiBoard owns the
        // connection.
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    // Apply via the raw-mode Board override so this works whether the
    // connection is owned by WifiStation or by WifiBoard.
    SetWifiPowerSave(level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    const std::string current_ssid = GetCurrentWifiNetwork();
    cJSON_AddStringToObject(network, "ssid", current_ssid.c_str());
    int rssi = GetWifiRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}

// =============================================================================
// Validation + helpers (anonymous namespace)
// =============================================================================

namespace {

// 802.11 SSID bounds (1..32 octets, no embedded NUL).
constexpr size_t kSsidMinLen = 1;
constexpr size_t kSsidMaxLen = 32;
// Password accepted: empty (open), 8..63 ASCII printable (WPA/WPA2 PSK),
// 64 hex chars (WPA3-SAE PMK / hex-encoded PSK).
constexpr size_t kPasswordMinLen = 8;
constexpr size_t kPasswordMaxLen = 64;
constexpr size_t kPasswordOpenMaxLen = 63;  // ASCII printable; 0 = open

bool IsAllHex(const std::string& s) {
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        bool ok = (uc >= '0' && uc <= '9') ||
                  (uc >= 'a' && uc <= 'f') ||
                  (uc >= 'A' && uc <= 'F');
        if (!ok) return false;
    }
    return true;
}

bool IsAsciiPrintable(const std::string& s) {
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7e) return false;
    }
    return true;
}

// Returns true and populates *out if ssid/password pass the validation rules.
// Never logs the password; only ever writes it into out->sta.password.
bool BuildWifiStaConfig(const std::string& ssid, const std::string& password,
                        wifi_config_t* out) {
    if (out == nullptr) return false;
    if (ssid.size() < kSsidMinLen || ssid.size() > kSsidMaxLen) return false;
    if (ssid.find('\0') != std::string::npos) return false;
    if (password.size() > kPasswordMaxLen) return false;
    if (!password.empty()) {
        if (password.size() == kPasswordMaxLen) {
            if (!IsAllHex(password)) return false;
        } else if (password.size() >= kPasswordMinLen &&
                   password.size() <= kPasswordOpenMaxLen) {
            if (!IsAsciiPrintable(password)) return false;
        } else {
            return false;
        }
    }
    std::memset(out, 0, sizeof(*out));
    // memcpy the exact validated byte count. strlcpy would silently truncate
    // a legal 32-byte SSID to 31 + NUL and a legal 64-byte (PMK / WPA3-SAE
    // hex) password to 63 + NUL; the buffers are fixed-size and we own the
    // exact length.
    std::memcpy(out->sta.ssid, ssid.data(), ssid.size());
    if (!password.empty()) {
        std::memcpy(out->sta.password, password.data(), password.size());
    }
    out->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    out->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    out->sta.failure_retry_cnt = 3;
    return true;
}

}  // namespace

// =============================================================================
// Board overrides: Wi-Fi inventory + raw-mode + status
// =============================================================================

std::vector<std::string> WifiBoard::GetSavedWifiNetworks() {
    // Names only. Passwords are intentionally dropped here.
    const auto& ssid_list = SsidManager::GetInstance().GetSsidList();
    std::vector<std::string> result;
    result.reserve(ssid_list.size());
    for (const auto& item : ssid_list) {
        result.push_back(item.ssid);
    }
    return result;
}

std::string WifiBoard::GetCurrentWifiNetwork() {
    // The WifiManager ssid_ cache may disagree with the live driver state when
    // WifiBoard owns the connection (no StartStation call). Prefer the live
    // driver value when available.
    wifi_ap_record_t ap_info{};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        char buf[33];
        std::memcpy(buf, ap_info.ssid, sizeof(ap_info.ssid));
        buf[32] = '\0';
        buf[strnlen(buf, sizeof(buf))] = '\0';
        return std::string(buf);
    }
    return {};
}

std::vector<std::string> WifiBoard::GetAvailableWifiNetworks() {
    std::lock_guard<std::mutex> lock(scanned_ssids_mutex_);
    return scanned_ssids_;
}

std::string WifiBoard::GetWifiStatus() {
    std::lock_guard<std::mutex> lock(wifi_status_mutex_);
    return wifi_status_;
}

void WifiBoard::SetWifiStatus(std::string status) {
    std::lock_guard<std::mutex> lock(wifi_status_mutex_);
    wifi_status_ = std::move(status);
}

bool WifiBoard::IsWifiConnected() const {
    wifi_ap_record_t ap_info{};
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

int WifiBoard::GetWifiRssi() const {
    wifi_ap_record_t ap_info{};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    // 0 dBm would read as a perfect signal. Real RSSI is negative, so this
    // sentinel is unambiguous and every consumer classifies it as "weak".
    return kRssiUnavailable;
}

void WifiBoard::SetWifiPowerSave(PowerSaveLevel level) {
    // Apply directly to the esp_wifi driver; this works whether WifiStation
    // or WifiBoard is the current owner of the connection.
    wifi_ps_type_t ps_type;
    switch (level) {
        case PowerSaveLevel::LOW_POWER: ps_type = WIFI_PS_MAX_MODEM; break;
        case PowerSaveLevel::BALANCED:  ps_type = WIFI_PS_MIN_MODEM; break;
        default:                          ps_type = WIFI_PS_NONE; break;
    }
    esp_wifi_set_ps(ps_type);
}

bool WifiBoard::PrepareRawWifi() {
    if (raw_sta_netif_ != nullptr || raw_wifi_started_) {
        ESP_LOGW(TAG, "Raw WiFi already owned; cleaning it before reuse");
        StopRawWifi();
    }

    // WifiManager::StopStation() must have destroyed its station netif before
    // this point. Refuse to create a second default netif if another owner is
    // still present; destroying it here would violate manager ownership.
    if (esp_netif_get_default_netif() != nullptr) {
        ESP_LOGE(TAG, "Cannot start raw WiFi while another default netif exists");
        return false;
    }

    raw_sta_netif_ = esp_netif_create_default_wifi_sta();
    if (raw_sta_netif_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create raw STA netif");
        return false;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure raw STA WiFi");
        StopRawWifi();
        return false;
    }
    return true;
}

bool WifiBoard::StartRawWifi() {
    if (raw_sta_netif_ == nullptr || esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start raw STA WiFi");
        StopRawWifi();
        return false;
    }
    raw_wifi_started_ = true;
    return true;
}

void WifiBoard::StopRawWifi() {
    if (raw_wifi_started_) {
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        esp_wifi_stop();
        raw_wifi_started_ = false;
    }
    if (raw_sta_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(raw_sta_netif_);
        raw_sta_netif_ = nullptr;
    }
}

void WifiBoard::ClearRawConnection() {
    std::lock_guard<std::mutex> lock(raw_connection_mutex_);
    raw_connection_generation_.fetch_add(1);
    raw_connection_ = {};
}

void WifiBoard::CancelWifiOperations() {
    esp_timer_stop(connect_timer_);
    station_attempt_generation_.fetch_add(1);
    station_attempt_active_.store(false);

    {
        std::lock_guard<std::mutex> lock(manual_join_mutex_);
        manual_join_generation_.fetch_add(1);
        manual_join_ = {};
    }
    ClearRawConnection();
    UnregisterManualJoinHandlers();

    {
        std::lock_guard<std::mutex> lock(scan_state_mutex_);
        scan_generation_.fetch_add(1);
        scan_in_progress_ = false;
        scan_previous_ssid_.clear();
    }
    if (scan_done_instance_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              scan_done_instance_);
        scan_done_instance_ = nullptr;
    }

    WifiManager::GetInstance().StopStation();
    StopRawWifi();
}

// =============================================================================
// Board overrides: Wi-Fi actions
// =============================================================================

bool WifiBoard::ConnectSavedWifiNetwork(const std::string& ssid) {
    if (ssid.empty()) return false;
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || wifi.IsConfigMode()) return false;
    {
        std::lock_guard<std::mutex> lock(manual_join_mutex_);
        if (manual_join_.active) return false;
    }
    {
        std::lock_guard<std::mutex> lock(scan_state_mutex_);
        if (scan_in_progress_) return false;
    }
    // WifiStation is still running; the read below is concurrent with
    // WifiStation::HandleScanResult but both sides only read so it is safe.
    std::string password;
    bool found = false;
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        if (item.ssid == ssid) {
            password = item.password;
            found = true;
            break;
        }
    }
    if (!found) return false;
    return BeginManualJoin(ssid, password, ManualJoin::Origin::Saved);
}

bool WifiBoard::ConnectWifiNetwork(const std::string& ssid, const std::string& password) {
    wifi_config_t config{};
    if (!BuildWifiStaConfig(ssid, password, &config)) return false;
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || wifi.IsConfigMode()) return false;
    {
        std::lock_guard<std::mutex> lock(manual_join_mutex_);
        if (manual_join_.active) return false;
    }
    {
        std::lock_guard<std::mutex> lock(scan_state_mutex_);
        if (scan_in_progress_) return false;
    }
    return BeginManualJoin(ssid, password, ManualJoin::Origin::Manual);
}

// =============================================================================
// Scan lifecycle (on-demand, station suspended)
// =============================================================================
//
// The WifiStation's own SCAN_DONE handler also consumes
// esp_wifi_scan_get_ap_records() (a destructive read), so the only safe way
// to expose a user-triggered scan is to suspend the station for the duration
// of our scan, register a one-shot handler, run the scan, capture results,
// then rejoin the previous network via BeginManualJoin so the user's selected
// AP survives the brief disconnect.

bool WifiBoard::ScanWifiNetworks() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || wifi.IsConfigMode()) return false;
    {
        std::lock_guard<std::mutex> lock(scan_state_mutex_);
        if (scan_in_progress_) return false;
    }
    {
        std::lock_guard<std::mutex> lock(manual_join_mutex_);
        if (manual_join_.active) return false;
    }

    // Capture the currently-connected SSID BEFORE we suspend the station so we
    // can rejoin it after the scan. Empty when there is no active connection.
    std::string previous;
    {
        wifi_ap_record_t ap_info{};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            char buf[33];
            std::memcpy(buf, ap_info.ssid, sizeof(ap_info.ssid));
            buf[32] = '\0';
            previous = buf;
        }
    }

    esp_timer_stop(connect_timer_);
    station_attempt_generation_.fetch_add(1);
    station_attempt_active_.store(false);
    // Suspend the WifiStation so its own SCAN_DONE handler cannot race us for
    // the destructive ap_records read.
    ClearRawConnection();
    UnregisterManualJoinHandlers();
    wifi.StopStation();
    StopRawWifi();

    {
        std::lock_guard<std::mutex> lock(scan_state_mutex_);
        scan_previous_ssid_ = previous;
        scan_in_progress_ = true;
        scan_generation_.fetch_add(1);
    }

    if (!PrepareRawWifi()) {
        {
            std::lock_guard<std::mutex> lock(scan_state_mutex_);
            scan_in_progress_ = false;
        }
        TryWifiConnect();
        return false;
    }

    // Register before starting the scan. The manager's station handlers were
    // removed by StopStation(), so this is the sole scan-record consumer.
    esp_err_t reg_err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
        &WifiBoard::ScanDoneOneShot, this, &scan_done_instance_);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "ScanWifiNetworks: register SCAN_DONE failed");
        {
            std::lock_guard<std::mutex> lock(scan_state_mutex_);
            scan_in_progress_ = false;
        }
        StopRawWifi();
        TryWifiConnect();
        return false;
    }

    if (!StartRawWifi()) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              scan_done_instance_);
        scan_done_instance_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(scan_state_mutex_);
            scan_in_progress_ = false;
        }
        TryWifiConnect();
        return false;
    }

    esp_err_t err = esp_wifi_scan_start(nullptr, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ScanWifiNetworks: esp_wifi_scan_start failed: %s",
                 esp_err_to_name(err));
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              scan_done_instance_);
        scan_done_instance_ = nullptr;
        {
            std::lock_guard<std::mutex> lock(scan_state_mutex_);
            scan_in_progress_ = false;
            scan_generation_.fetch_add(1);
        }
        StopRawWifi();
        TryWifiConnect();
        return false;
    }

    SetWifiStatus("Scanning...");
    return true;
}

// =============================================================================
// Manual join lifecycle (direct esp_wifi API)
// =============================================================================

bool WifiBoard::BeginManualJoin(const std::string& ssid, const std::string& password,
                                ManualJoin::Origin origin) {
    wifi_config_t config{};
    if (!BuildWifiStaConfig(ssid, password, &config)) return false;

    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConfigMode()) return false;

    // Bump the generation and publish the active attempt before touching the
    // driver. Event handlers are installed before esp_wifi_start/connect, so a
    // fast AP cannot win before the attempt has an owner.
    uint32_t my_gen;
    {
        std::lock_guard<std::mutex> lock(manual_join_mutex_);
        if (manual_join_.active) return false;
        my_gen = manual_join_generation_.fetch_add(1) + 1;
        manual_join_.ssid = ssid;
        manual_join_.password = password;
        manual_join_.active = true;
        manual_join_.connect_requested = false;
        manual_join_.origin = origin;
    }
    station_attempt_generation_.fetch_add(1);
    station_attempt_active_.store(false);

    // Stop the WifiStation. After this the wifi event task is idle, so we own
    // the netif + driver state until we hand control back via ScheduleOutcome.
    ClearRawConnection();
    UnregisterManualJoinHandlers();
    wifi.StopStation();
    StopRawWifi();

    if (!PrepareRawWifi()) {
        ESP_LOGE(TAG, "BeginManualJoin: failed to prepare raw STA WiFi");
        ScheduleOutcome(my_gen, false, std::string(), std::string());
        return false;
    }

    if (!RegisterManualJoinHandlers() || !StartRawWifi()) {
        ESP_LOGE(TAG, "BeginManualJoin: failed to install/start raw STA WiFi");
        ScheduleOutcome(my_gen, false, std::string(), std::string());
        return false;
    }

    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        ESP_LOGE(TAG, "BeginManualJoin: esp_wifi_set_config failed");
        ScheduleOutcome(my_gen, false, std::string(), std::string());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(manual_join_mutex_);
        if (manual_join_generation_.load() != my_gen || !manual_join_.active) {
            return false;
        }
        manual_join_.connect_requested = true;
    }
    SetWifiStatus("Connecting to " + ssid);
    if (esp_wifi_connect() != ESP_OK) {
        ESP_LOGE(TAG, "BeginManualJoin: esp_wifi_connect failed");
        ScheduleOutcome(my_gen, false, std::string(), std::string());
        return false;
    }

    esp_timer_stop(connect_timer_);
    esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);

    ESP_LOGI(TAG, "Manual join started for saved entry (origin=%s, gen=%u)",
             origin == ManualJoin::Origin::Initial ? "initial" : "manual",
             static_cast<unsigned>(my_gen));
    return true;
}

void WifiBoard::ScheduleOutcome(uint32_t generation, bool commit,
                                std::string ssid, std::string password) {
    // Always run on the main task: SsidManager writes go through
    // Application::Schedule so the wifi event task never mutates persistent
    // storage. The capture-by-value generation guards against stale callbacks
    // racing with a newer BeginManualJoin.
    Application::GetInstance().Schedule(
        [this, generation, commit, ssid = std::move(ssid),
         password = std::move(password)]() {
            bool accept = false;
            bool save_credentials = false;
            {
                std::lock_guard<std::mutex> lock(manual_join_mutex_);
                if (manual_join_generation_.load() == generation &&
                    manual_join_.active) {
                    // Invalidate every other queued outcome for this attempt
                    // before releasing the lock. Only the first event wins.
                    manual_join_generation_.fetch_add(1);
                    manual_join_.active = false;
                    manual_join_.connect_requested = false;
                    save_credentials = manual_join_.origin == ManualJoin::Origin::Manual;
                    manual_join_.ssid.clear();
                    manual_join_.password.clear();
                    accept = true;
                }
            }
            if (!accept) return;

            esp_timer_stop(connect_timer_);
            station_attempt_generation_.fetch_add(1);
            station_attempt_active_.store(false);

            if (commit && !ssid.empty()) {
                // Success path: stay OWNED by WifiBoard. No StartStation call
                // so the chosen AP is not immediately replaced by the strongest
                // other saved SSID. Keep the event handlers registered: ESP-IDF
                // does not reconnect after a later STA_DISCONNECTED event just
                // because the config remains in the driver.
                {
                    std::lock_guard<std::mutex> raw_lock(raw_connection_mutex_);
                    raw_connection_generation_.fetch_add(1);
                    raw_connection_.ssid = ssid;
                    raw_connection_.active = true;
                    raw_connection_.connected = true;
                    raw_connection_.reconnect_pending = false;
                }
                if (save_credentials) {
                    SsidManager::GetInstance().AddSsid(ssid, password);
                }
                SetWifiStatus("Connected to " + ssid);
                OnNetworkEvent(NetworkEvent::Connected, ssid);
                return;
            }

            // Failure path: tear down our raw mode so WifiStation can take
            // over and try the saved list normally.
            UnregisterManualJoinHandlers();
            ClearRawConnection();
            StopRawWifi();
            SetWifiStatus("Join failed");
            OnNetworkEvent(NetworkEvent::Disconnected);
            if (!WifiManager::GetInstance().IsConfigMode()) TryWifiConnect();
        });
}

void WifiBoard::ScheduleRawReconnect(uint32_t generation) {
    Application::GetInstance().Schedule([this, generation]() {
        std::string ssid;
        {
            std::lock_guard<std::mutex> lock(raw_connection_mutex_);
            if (!raw_connection_.active ||
                raw_connection_generation_.load() != generation) {
                return;
            }
            if (raw_connection_.connected) {
                raw_connection_.reconnect_pending = false;
                return;
            }
            raw_connection_.reconnect_pending = false;
            ssid = raw_connection_.ssid;
        }

        ESP_LOGW(TAG, "WiFi disconnected; reconnecting to %s", ssid.c_str());
        OnNetworkEvent(NetworkEvent::Connecting, ssid);
        if (!esp_timer_is_active(connect_timer_))
            esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        if (esp_wifi_connect() == ESP_OK) return;

        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(raw_connection_mutex_);
            if (raw_connection_.active &&
                !raw_connection_.connected &&
                raw_connection_generation_.load() == generation) {
                raw_connection_generation_.fetch_add(1);
                raw_connection_ = {};
                accepted = true;
            }
        }
        if (!accepted) return;

        ESP_LOGE(TAG, "WiFi reconnect request failed; returning to manager");
        UnregisterManualJoinHandlers();
        esp_timer_stop(connect_timer_);
        StopRawWifi();
        SetWifiStatus("Wi-Fi reconnect failed");
        OnNetworkEvent(NetworkEvent::Disconnected);
        if (!WifiManager::GetInstance().IsConfigMode()) TryWifiConnect();
    });
}

void WifiBoard::ScheduleRawConnected(uint32_t generation) {
    Application::GetInstance().Schedule([this, generation]() {
        std::string ssid;
        {
            std::lock_guard<std::mutex> lock(raw_connection_mutex_);
            if (!raw_connection_.active ||
                !raw_connection_.connected ||
                raw_connection_generation_.load() != generation) {
                return;
            }
            ssid = raw_connection_.ssid;
        }
        esp_timer_stop(connect_timer_);
        SetWifiStatus("Connected to " + ssid);
        OnNetworkEvent(NetworkEvent::Connected, ssid);
    });
}

bool WifiBoard::RegisterManualJoinHandlers() {
    if (manual_join_wifi_instance_ == nullptr) {
        // ESP_EVENT_ANY_ID because we filter for STA_CONNECTED / STA_DISCONNECTED
        // inside the handler (a bitwise OR of the two ids is not a valid event id).
        if (esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            &WifiBoard::ManualJoinWifiHandler, this, &manual_join_wifi_instance_) != ESP_OK) {
            return false;
        }
    }
    if (manual_join_ip_instance_ == nullptr) {
        if (esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            &WifiBoard::ManualJoinIpHandler, this, &manual_join_ip_instance_) != ESP_OK) {
            UnregisterManualJoinHandlers();
            return false;
        }
    }
    return true;
}

void WifiBoard::UnregisterManualJoinHandlers() {
    if (manual_join_wifi_instance_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              manual_join_wifi_instance_);
        manual_join_wifi_instance_ = nullptr;
    }
    if (manual_join_ip_instance_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              manual_join_ip_instance_);
        manual_join_ip_instance_ = nullptr;
    }
}

void WifiBoard::ManualJoinWifiHandler(void* arg, esp_event_base_t base,
                                      int32_t id, void* data) {
    (void)base;
    auto* board = static_cast<WifiBoard*>(arg);
    if (board == nullptr) return;

    bool active = false;
    bool connect_requested = false;
    std::string requested;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(board->manual_join_mutex_);
        active = board->manual_join_.active;
        if (active) {
            connect_requested = board->manual_join_.connect_requested;
            requested = board->manual_join_.ssid;
            generation = board->manual_join_generation_.load();
        }
    }
    if (!active) {
        if (id != WIFI_EVENT_STA_DISCONNECTED) return;
        uint32_t raw_generation;
        {
            std::lock_guard<std::mutex> lock(board->raw_connection_mutex_);
            if (!board->raw_connection_.active || board->raw_connection_.reconnect_pending) return;
            board->raw_connection_.connected = false;
            board->raw_connection_.reconnect_pending = true;
            raw_generation = board->raw_connection_generation_.load();
        }
        board->ScheduleRawReconnect(raw_generation);
        return;
    }

    if (id == WIFI_EVENT_STA_CONNECTED && connect_requested) {
        auto* event = static_cast<wifi_event_sta_connected_t*>(data);
        if (event == nullptr) return;
        char actual_ssid[33];
        std::memcpy(actual_ssid, event->ssid, sizeof(event->ssid));
        actual_ssid[32] = '\0';
        if (!requested.empty() && std::string(actual_ssid) != requested) {
            ESP_LOGW(TAG, "Manual join landed on unexpected ssid; aborting");
            board->ScheduleOutcome(generation, false, std::string(), std::string());
        }
        // IP_EVENT_STA_GOT_IP is the canonical success signal.
    } else if (id == WIFI_EVENT_STA_DISCONNECTED && connect_requested) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
        if (event == nullptr) return;
        ESP_LOGW(TAG, "Manual join disconnected (reason=%d); credential NOT saved",
                 event->reason);
        board->ScheduleOutcome(generation, false, std::string(), std::string());
    }
}

void WifiBoard::ManualJoinIpHandler(void* arg, esp_event_base_t base,
                                    int32_t id, void* data) {
    (void)base;
    (void)data;
    if (id != IP_EVENT_STA_GOT_IP) return;
    auto* board = static_cast<WifiBoard*>(arg);
    if (board == nullptr) return;

    bool active = false;
    bool connect_requested = false;
    std::string ssid;
    std::string password;
    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(board->manual_join_mutex_);
        active = board->manual_join_.active;
        if (active) {
            connect_requested = board->manual_join_.connect_requested;
            ssid = board->manual_join_.ssid;
            password = board->manual_join_.password;
            generation = board->manual_join_generation_.load();
        }
    }
    if (!active) {
        uint32_t raw_generation;
        {
            std::lock_guard<std::mutex> lock(board->raw_connection_mutex_);
            if (!board->raw_connection_.active) return;
            board->raw_connection_.connected = true;
            board->raw_connection_.reconnect_pending = false;
            raw_generation = board->raw_connection_generation_.load();
        }
        board->ScheduleRawConnected(raw_generation);
        return;
    }
    if (!connect_requested) return;

    ESP_LOGI(TAG, "Manual join IP acquired; committing on main task");
    board->ScheduleOutcome(generation, true, std::move(ssid), std::move(password));
}

// Scan completion: runs once on SCAN_DONE. Captures the records into
// scanned_ssids_ and defers handler removal, teardown, and rejoin to the main
// task. The station remains suspended until that callback, so no second
// handler can consume the same records.
void WifiBoard::ScanDoneOneShot(void* arg, esp_event_base_t base,
                                int32_t id, void* data) {
    (void)base;
    (void)data;
    if (id != WIFI_EVENT_SCAN_DONE) return;
    auto* board = static_cast<WifiBoard*>(arg);
    if (board == nullptr) return;

    uint32_t generation;
    std::vector<std::string> ssids;
    std::string previous;
    {
        std::lock_guard<std::mutex> lock(board->scan_state_mutex_);
        if (!board->scan_in_progress_) return;
        generation = board->scan_generation_.load();

        // Read records (this is the destructive read; we own it because the
        // manager station is suspended). Keep the scan-state lock while doing
        // so a main-task cancellation cannot stop the driver underneath us.
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        if (ap_num > 0) {
            constexpr uint16_t kMaxScanRecords = 64;
            if (ap_num > kMaxScanRecords) ap_num = kMaxScanRecords;
            auto* ap_records = static_cast<wifi_ap_record_t*>(
                calloc(ap_num, sizeof(wifi_ap_record_t)));
            if (ap_records != nullptr) {
                uint16_t count = ap_num;
                if (esp_wifi_scan_get_ap_records(&count, ap_records) == ESP_OK) {
                    std::sort(ap_records, ap_records + count,
                              [](const wifi_ap_record_t& left,
                                 const wifi_ap_record_t& right) {
                                  return left.rssi > right.rssi;
                              });
                    ssids.reserve(count);
                    for (uint16_t i = 0; i < count; ++i) {
                        // ESP32 station cannot join 5GHz; filter so the UI never
                        // offers an AP the user could not actually select.
                        if (ap_records[i].primary < 1 || ap_records[i].primary > 14) {
                            continue;
                        }
                        const char* raw = reinterpret_cast<const char*>(ap_records[i].ssid);
                        size_t len = strnlen(raw, sizeof(ap_records[i].ssid));
                        if (len == 0) continue;
                        std::string ssid(raw, len);
                        if (std::find(ssids.begin(), ssids.end(), ssid) == ssids.end()) {
                            ssids.push_back(std::move(ssid));
                        }
                    }
                }
                free(ap_records);
            }
        }
        {
            std::lock_guard<std::mutex> cache_lock(board->scanned_ssids_mutex_);
            board->scanned_ssids_ = ssids;
        }
        previous = std::move(board->scan_previous_ssid_);
    }

    // Defer teardown + rejoin to the main task so we do not call esp_wifi_stop
    // from inside a wifi event handler.
    Application::GetInstance().Schedule(
        [board, generation, previous = std::move(previous)]() {
            {
                std::lock_guard<std::mutex> lock(board->scan_state_mutex_);
                if (!board->scan_in_progress_ ||
                    board->scan_generation_.load() != generation) {
                    return;
                }
                board->scan_in_progress_ = false;
                board->scan_previous_ssid_.clear();
            }

            if (board->scan_done_instance_ != nullptr) {
                esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                                      board->scan_done_instance_);
                board->scan_done_instance_ = nullptr;
            }
            board->StopRawWifi();

            {
                std::string password;
                bool found = false;
                for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
                    if (item.ssid == previous) {
                        password = item.password;
                        found = true;
                        break;
                    }
                }

                // Empty is a valid password for an open network; use an
                // explicit found flag so an open selected SSID is preserved.
                if (!previous.empty() && found) {
                    board->SetWifiStatus("Scanning... -> rejoining " + previous);
                    board->BeginManualJoin(previous, password,
                                           ManualJoin::Origin::Saved);
                    return;
                }
            }
            // No previous SSID or it was deleted; let the station pick.
            if (!WifiManager::GetInstance().IsConfigMode()) board->TryWifiConnect();
        });
}
