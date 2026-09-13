#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <vector>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "assets.h"

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;

    // ===== Wi-Fi inventory (default empty for boards without Wi-Fi) =====
    // GetSavedWifiNetworks: list of SSIDs the device remembers (names only, never passwords).
    virtual std::vector<std::string> GetSavedWifiNetworks() { return {}; }
    // GetCurrentWifiNetwork: SSID the station is currently connected to, empty if not connected.
    virtual std::string GetCurrentWifiNetwork() { return std::string(); }
    // GetAvailableWifiNetworks: cached scan results (SSIDs only, sorted by signal strength).
    virtual std::vector<std::string> GetAvailableWifiNetworks() { return {}; }

    // ===== Wi-Fi actions (default false = not supported on this board) =====
    // ConnectSavedWifiNetwork: switch to a previously-saved SSID. Async; outcome via callback.
    virtual bool ConnectSavedWifiNetwork(const std::string& ssid) { (void)ssid; return false; }
    // ScanWifiNetworks: trigger an async Wi-Fi scan; returns false if a scan is already in progress
    // or the driver is not in a state that allows scanning.
    virtual bool ScanWifiNetworks() { return false; }
    // ConnectWifiNetwork: join a new (ssid, password). Async; outcome via callback.
    // The credential is saved only on success; failed attempts are not retained.
    virtual bool ConnectWifiNetwork(const std::string& ssid, const std::string& password) {
        (void)ssid; (void)password; return false;
    }
    // Enter the board's Wi-Fi setup flow. Boards without Wi-Fi can ignore it.
    virtual void EnterWifiConfigMode() {}

    // ===== Wi-Fi status (human-readable, no secrets) =====
    // Returns the latest Wi-Fi status string for UI consumption: "Scanning...",
    // "Connecting to <ssid>", "Connected to <ssid>", "Join failed", "Wi-Fi setup mode",
    // or "" when idle / uninitialized. Thread-safe; readable from any task.
    // Default empty for boards without Wi-Fi.
    virtual std::string GetWifiStatus() { return std::string(); }

    // ===== Raw-mode Wi-Fi queries (no station required) =====
    // WifiBoard overrides these so a connection held by the raw netif (after a
    // direct-join success that deliberately skipped StartStation) still answers
    // "connected? / RSSI?" correctly. Defaults are conservative (no station).
    virtual bool IsWifiConnected() const { return false; }
    virtual int GetWifiRssi() const { return 0; }
    // SetWifiPowerSave applies power-save directly to the esp_wifi driver.
    // Boards without Wi-Fi can ignore.
    virtual void SetWifiPowerSave(PowerSaveLevel level) { (void)level; }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
