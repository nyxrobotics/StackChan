/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "application.h"       // IsNetworkRequired
#include <stackchan/stackchan.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <wifi_manager.h>
#include <board.h>
#include <mutex>
#include <queue>
#include <vector>
#include <ctime>
#include <sys/time.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <memory>
#include <utility>

static std::string _tag = "Network";

namespace {

constexpr EventBits_t kNetworkConnectedBit   = BIT0;
constexpr EventBits_t kNetworkUnavailableBit = BIT1;
constexpr TickType_t kNetworkConnectTimeout  = pdMS_TO_TICKS(30000);
std::mutex networkStartMutex;

struct NetworkWaitState {
    NetworkWaitState() : events(xEventGroupCreate())
    {
    }

    ~NetworkWaitState()
    {
        if (events != nullptr) {
            vEventGroupDelete(events);
        }
    }

    void handle(NetworkEvent event, const std::string& data)
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        if (!active) {
            return;
        }

        switch (event) {
            case NetworkEvent::Scanning:
                if (onLog) {
                    onLog("WiFi scanning...");
                }
                break;
            case NetworkEvent::Connecting:
                if (onLog) {
                    onLog(data.empty() ? "WiFi connecting..." : fmt::format("Connecting to {} ...", data));
                }
                break;
            case NetworkEvent::Connected:
                xEventGroupSetBits(events, kNetworkConnectedBit);
                break;
            case NetworkEvent::WifiConfigModeEnter: {
                xEventGroupSetBits(events, kNetworkUnavailableBit);
                auto& wifi_manager = WifiManager::GetInstance();
                if (onLog) {
                    onLog(fmt::format("Enter WiFi config mode. Hotspot: {}, Config URL: {}",
                                      wifi_manager.GetApSsid(), wifi_manager.GetApWebUrl()));
                }
                break;
            }
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
            case NetworkEvent::Unavailable:
                xEventGroupSetBits(events, kNetworkUnavailableBit);
                break;
            case NetworkEvent::Disconnected:
            case NetworkEvent::WifiConfigModeExit:
            case NetworkEvent::ModemDetecting:
                break;
        }
    }

    void deactivate()
    {
        std::lock_guard<std::mutex> lock(callbackMutex);
        active = false;
        onLog = nullptr;
    }

    EventGroupHandle_t events = nullptr;
    std::mutex callbackMutex;
    std::function<void(std::string_view)> onLog;
    bool active = true;
};

}  // namespace

static void time_sync_notification_cb(struct timeval* tv)
{
    mclog::tagInfo(_tag, "SNTP time synchronized");
    GetHAL().syncSystemTimeToRtc();
}

void Hal::startSntp()
{
    // Auto can become ready through its local fallback before Wi-Fi/lwIP has
    // been initialized. Calling SNTP in that state asserts inside tcpip_callback.
    if (!Application::GetInstance().IsNetworkEnabled() ||
        !WifiManager::GetInstance().IsConnected()) {
        mclog::tagInfo(_tag, "SNTP skipped (network not connected)");
        return;
    }

    mclog::tagInfo(_tag, "SNTP init");

    if (esp_sntp_enabled()) {
    } else {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "cn.pool.ntp.org");

        sntp_set_time_sync_notification_cb(time_sync_notification_cb);

        esp_sntp_init();
    }
}

bool Hal::startNetwork(std::function<void(std::string_view)> onLog)
{
    std::lock_guard<std::mutex> start_lock(networkStartMutex);

    auto& wifi_manager = WifiManager::GetInstance();
    if (wifi_manager.IsConnected()) {
        mclog::tagInfo(_tag, "network already connected");
        startSntp();
        return true;
    }
    if (!wifi_manager.IsInitialized()) {
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) == ESP_OK) {
            mclog::tagError(_tag, "WiFi mode {} is currently owned by another subsystem",
                            static_cast<int>(mode));
            if (onLog) {
                onLog("WiFi is busy in another mode.");
            }
            return false;
        }
    }

    auto& board = Board::GetInstance();
    mclog::tagInfo(_tag, "start and wait for network connected...");

    auto wait_state = std::make_shared<NetworkWaitState>();
    if (wait_state->events == nullptr) {
        mclog::tagError(_tag, "failed to allocate network wait event");
        if (onLog) {
            onLog("WiFi initialization failed.");
        }
        return false;
    }
    wait_state->onLog = std::move(onLog);

    hal_bridge::board_set_network_wait_callback(
        [wait_state](NetworkEvent event, const std::string& data) { wait_state->handle(event, data); });
    board.StartNetwork();
    if (!wifi_manager.IsInitialized()) {
        hal_bridge::board_set_network_wait_callback(nullptr);
        wait_state->deactivate();
        mclog::tagError(_tag, "network initialization failed");
        return false;
    }
    if (wifi_manager.IsConnected()) {
        xEventGroupSetBits(wait_state->events, kNetworkConnectedBit);
    } else if (wifi_manager.IsConfigMode()) {
        xEventGroupSetBits(wait_state->events, kNetworkUnavailableBit);
    }

    EventBits_t bits = xEventGroupWaitBits(wait_state->events, kNetworkConnectedBit | kNetworkUnavailableBit,
                                           pdFALSE, pdFALSE, kNetworkConnectTimeout);

    hal_bridge::board_set_network_wait_callback(nullptr);
    wait_state->deactivate();

    if (bits & kNetworkUnavailableBit) {
        mclog::tagWarn(_tag, "network requires configuration before it can connect");
        return false;
    }
    if ((bits & kNetworkConnectedBit) == 0 || !wifi_manager.IsConnected()) {
        mclog::tagError(_tag, "network connection timed out");
        return false;
    }

    mclog::tagInfo(_tag, "network connected");
    startSntp();
    return true;
}

WifiStatus Hal::getWifiStatus()
{
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return WifiStatus::None;
    }
    if (!wifi.IsConnected()) {
        return WifiStatus::None;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return WifiStatus::High;
    } else if (rssi >= -75) {
        return WifiStatus::Medium;
    }
    return WifiStatus::Low;
}
