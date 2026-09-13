#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <atomic>
#include <string>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    // ===== Status string for UI (mutex-protected) =====
    std::string wifi_status_;
    std::mutex wifi_status_mutex_;

    // ===== Manual join state (mutex + atomic generation) =====
    // The application task calls BeginManualJoin. The wifi event task fires
    // our handlers; they capture the current generation and Schedule the
    // actual commit/cleanup on the main task. The generation guard prevents a
    // stale outcome callback from clobbering a newer attempt.
    struct ManualJoin {
        std::string ssid;       // requested SSID (never logged with password)
        std::string password;   // kept here only to commit on success
        bool active = false;
        bool connect_requested = false;
        enum class Origin { Initial, Saved, Manual } origin = Origin::Initial;
    };
    ManualJoin manual_join_;
    std::mutex manual_join_mutex_;
    // Incremented on every BeginManualJoin; the Schedule callback compares
    // its captured value against this to detect a stale outcome.
    std::atomic<uint32_t> manual_join_generation_{0};

    esp_event_handler_instance_t manual_join_wifi_instance_ = nullptr;
    esp_event_handler_instance_t manual_join_ip_instance_ = nullptr;

    // After a direct join succeeds, WifiBoard remains the owner of the raw
    // station. ESP-IDF does not reconnect by itself after a later
    // STA_DISCONNECTED event, so the same handlers stay registered and use
    // this separate generation to recover the exact selected SSID.
    struct RawConnection {
        std::string ssid;
        bool active = false;
        bool connected = false;
        bool reconnect_pending = false;
    };
    RawConnection raw_connection_;
    std::mutex raw_connection_mutex_;
    std::atomic<uint32_t> raw_connection_generation_{0};

    // Raw station ownership is explicit. Never destroy the manager's default
    // netif by looking it up globally; WifiManager owns that netif.
    esp_netif_t* raw_sta_netif_ = nullptr;
    bool raw_wifi_started_ = false;

    // Normal station timeouts use a separate generation so a queued timer
    // callback cannot enter setup mode after a connection has already won.
    std::atomic<uint32_t> station_attempt_generation_{0};
    std::atomic<bool> station_attempt_active_{false};

    // ===== Scan cache (SSID names only; populated by ScanDoneOneShot) =====
    std::vector<std::string> scanned_ssids_;
    std::mutex scanned_ssids_mutex_;

    // ===== On-demand scan state =====
    // ScanWifiNetworks does an isolated scan with the WifiStation suspended so
    // there is only one destructive esp_wifi_scan_get_ap_records() consumer
    // (ours). The handler is registered just-in-time, fires once on SCAN_DONE,
    // and unregisters it on the main task before restarting the station. The
    // previously-connected SSID is captured up front and rejoined via the same
    // manual-join path so the user's selection survives the scan.
    esp_event_handler_instance_t scan_done_instance_ = nullptr;
    std::mutex scan_state_mutex_;
    bool scan_in_progress_ = false;
    std::string scan_previous_ssid_;
    std::atomic<uint32_t> scan_generation_{0};

    virtual std::string GetBoardJson() override;

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");
    void TryWifiConnect();
    void StartWifiConfigMode();
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();

    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;

    void EnterWifiConfigMode() override;

    // ===== Board overrides: Wi-Fi inventory + raw-mode queries =====
    std::vector<std::string> GetSavedWifiNetworks() override;
    std::string GetCurrentWifiNetwork() override;
    std::vector<std::string> GetAvailableWifiNetworks() override;
    std::string GetWifiStatus() override;
    // Raw-mode (no station required). After a successful direct join the
    // WifiBoard intentionally does not call StartStation so the chosen AP is
    // not immediately replaced by the strongest other saved AP. The raw
    // connection handler keeps that exact AP alive across transient drops.
    bool IsWifiConnected() const override;
    // Returned by GetWifiRssi when no AP is associated. Well below any real
    // reading, so signal classifiers land on the weakest bucket.
    static constexpr int kRssiUnavailable = -127;
    int GetWifiRssi() const override;
    void SetWifiPowerSave(PowerSaveLevel level) override;

    // ===== Board overrides: Wi-Fi actions =====
    // Trigger an on-demand Wi-Fi scan with the WifiStation suspended so
    // there is only one destructive esp_wifi_scan_get_ap_records() consumer.
    // The previously-connected SSID is captured and rejoined through
    // BeginManualJoin after the scan completes, so the user's selection
    // survives the brief disconnect.
    bool ScanWifiNetworks() override;
    // ConnectSavedWifiNetwork: direct-join a saved SSID; outcome reported
    // through SetNetworkEventCallback(). The saved list is not rewritten.
    bool ConnectSavedWifiNetwork(const std::string& ssid) override;
    // ConnectWifiNetwork: direct-join a new (ssid, password). On success the
    // credential is committed to SsidManager on the main task. On failure
    // (wrong SSID, disconnect, timeout) SsidManager is never touched and the
    // raw netif is torn down so WifiStation can resume normal flow.
    bool ConnectWifiNetwork(const std::string& ssid, const std::string& password) override;

private:
    void SetWifiStatus(std::string status);

    // BeginManualJoin assumes inputs are already validated. It stops the
    // WifiStation, brings up the wifi driver in STA mode with the requested
    // config, registers our event handlers, and starts the connect timer.
    bool BeginManualJoin(const std::string& ssid, const std::string& password,
                         ManualJoin::Origin origin);
    // ScheduleOutcome is the single place that mutates SsidManager or tears
    // down the raw netif; it always runs on the main task and gates on a
    // generation value so a stale event cannot clobber a newer attempt.
    // Successful raw joins keep the manual handlers registered for later
    // disconnect recovery.
    void ScheduleOutcome(uint32_t generation, bool commit,
                         std::string ssid, std::string password);
    void ClearRawConnection();
    void ScheduleRawReconnect(uint32_t generation);
    void ScheduleRawConnected(uint32_t generation);
    bool RegisterManualJoinHandlers();
    void UnregisterManualJoinHandlers();
    bool PrepareRawWifi();
    bool StartRawWifi();
    void StopRawWifi();
    void CancelWifiOperations();
    static void ScanDoneOneShot(void* arg, esp_event_base_t base,
                                int32_t id, void* data);
    static void ManualJoinWifiHandler(void* arg, esp_event_base_t base,
                                      int32_t id, void* data);
    static void ManualJoinIpHandler(void* arg, esp_event_base_t base,
                                    int32_t id, void* data);
};

#endif // WIFI_BOARD_H
