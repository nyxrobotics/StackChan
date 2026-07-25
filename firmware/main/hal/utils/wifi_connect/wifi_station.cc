#include "wifi_station.h"
#include <cstring>
#include <exception>
#include <utility>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include "ssid_manager.h"

#define TAG                  "StackChanWifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_STOPPED   BIT1
#define MAX_RECONNECT_COUNT  1

namespace {

void InvokeCallbackSafely(const std::function<void(const std::string&)>& callback,
                          const std::string& ssid,
                          const char* callback_name)
{
    if (!callback) {
        return;
    }

    try {
        callback(ssid);
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "%s callback failed: %s", callback_name, e.what());
    } catch (...) {
        ESP_LOGE(TAG, "%s callback failed with an unknown exception", callback_name);
    }
}

}  // namespace

StackChanWifiStation::StackChanWifiStation()
{
    event_group_ = xEventGroupCreate();
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
    }
}

StackChanWifiStation::~StackChanWifiStation()
{
    Stop();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool StackChanWifiStation::AddAuth(const std::string& ssid, const std::string& password)
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);

    auto notify_failure = [this, &ssid]() {
        std::function<void(const std::string&)> callback;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            is_connecting_ = false;
            callback       = on_connect_failed_;
        }
        InvokeCallbackSafely(callback, ssid, "connection failure");
    };

    wifi_config_t wifi_config = {};
    bool station_ready = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        station_ready = is_started_ && event_group_ != nullptr;
    }
    if (!station_ready) {
        ESP_LOGE(TAG, "WiFi station is not ready");
        notify_failure();
        return false;
    }
    // SsidManager later copies these values as C strings, so reserve one byte
    // for a terminator even though the ESP-IDF SSID field itself permits 32 bytes.
    if (ssid.empty() || ssid.size() >= sizeof(wifi_config.sta.ssid) ||
        password.size() >= sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "Invalid WiFi credentials or station is not ready");
        notify_failure();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ssid_ = ssid;
    }
    memcpy(wifi_config.sta.ssid, ssid.c_str(), ssid.length());
    memcpy(wifi_config.sta.password, password.c_str(), password.length());

    ESP_LOGI(TAG, "Setting WiFi configuration SSID: %s", ssid.c_str());
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi configuration: %s", esp_err_to_name(err));
        notify_failure();
        return false;
    }

    // Save only a configuration accepted by the WiFi driver.
    SsidManager::GetInstance().AddSsid(ssid, password);

    std::function<void(const std::string&)> connect_callback;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        reconnect_count_ = 0;
        is_connecting_   = true;
        connect_callback = on_connect_;
    }
    InvokeCallbackSafely(connect_callback, ssid, "connecting");

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ESP_LOGI(TAG, "Already connected, disconnecting first...");
        err = esp_wifi_disconnect();
        // The reconnection will be handled by WIFI_EVENT_STA_DISCONNECTED
    } else {
        err = esp_wifi_connect();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi connection: %s", esp_err_to_name(err));
        notify_failure();
        return false;
    }
    return true;
}

void StackChanWifiStation::CleanupStartFailure()
{
    esp_event_handler_instance_t any_id = nullptr;
    esp_event_handler_instance_t got_ip = nullptr;
    esp_netif_t* station_netif          = nullptr;
    bool wifi_initialized_here          = false;
    bool netif_created_here             = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        is_started_            = false;
        is_connecting_         = false;
        any_id                 = instance_any_id_;
        got_ip                 = instance_got_ip_;
        station_netif          = station_netif_;
        wifi_initialized_here  = wifi_initialized_here_;
        netif_created_here     = netif_created_here_;
        instance_any_id_       = nullptr;
        instance_got_ip_       = nullptr;
        station_netif_         = nullptr;
        wifi_initialized_here_ = false;
        netif_created_here_    = false;
    }

    if (any_id != nullptr) {
        esp_err_t err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, any_id);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "WiFi event unregistration failed: %s", esp_err_to_name(err));
        }
    }
    if (got_ip != nullptr) {
        esp_err_t err = esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "IP event unregistration failed: %s", esp_err_to_name(err));
        }
    }

    bool wifi_released = !wifi_initialized_here;
    if (wifi_initialized_here) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "WiFi stop failed during cleanup: %s", esp_err_to_name(stop_err));
        }
        esp_err_t deinit_err = esp_wifi_deinit();
        wifi_released        = deinit_err == ESP_OK || deinit_err == ESP_ERR_WIFI_NOT_INIT;
        if (!wifi_released) {
            ESP_LOGW(TAG, "WiFi deinitialization failed during cleanup: %s", esp_err_to_name(deinit_err));
        }
    }

    if (netif_created_here && station_netif != nullptr && wifi_released) {
        esp_netif_destroy_default_wifi(station_netif);
    } else if (!wifi_released) {
        // Keep ownership information so a later Stop()/Start() can retry the
        // failed driver cleanup instead of treating the live driver as foreign.
        std::lock_guard<std::mutex> lock(state_mutex_);
        wifi_initialized_here_ = wifi_initialized_here;
        netif_created_here_    = netif_created_here;
        station_netif_         = station_netif;
    }

    if (event_group_ != nullptr) {
        xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED);
        xEventGroupSetBits(event_group_, WIFI_EVENT_STOPPED);
    }
}

void StackChanWifiStation::Stop()
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    CleanupStartFailure();
}

void StackChanWifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_connect_ = std::move(on_connect);
}

void StackChanWifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_connected_ = std::move(on_connected);
}

void StackChanWifiStation::OnConnectFailed(std::function<void(const std::string& ssid)> on_connect_failed)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    on_connect_failed_ = std::move(on_connect_failed);
}

bool StackChanWifiStation::Start()
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (is_started_) {
            return true;
        }
    }
    if (event_group_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate WiFi event group");
        return false;
    }
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED);

    wifi_mode_t existing_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&existing_mode) == ESP_OK) {
        ESP_LOGE(TAG, "WiFi mode %d is already owned by another subsystem", static_cast<int>(existing_mode));
        CleanupStartFailure();
        return false;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        CleanupStartFailure();
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop initialization failed: %s", esp_err_to_name(err));
        CleanupStartFailure();
        return false;
    }

    station_netif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station_netif_ == nullptr) {
        station_netif_     = esp_netif_create_default_wifi_sta();
        netif_created_here_ = station_netif_ != nullptr;
    }
    if (station_netif_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create WiFi station interface");
        CleanupStartFailure();
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable         = true;  // Enable NVS to store credentials
    err = esp_wifi_init(&cfg);
    if (err == ESP_OK) {
        wifi_initialized_here_ = true;
    } else if (err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "WiFi initialization failed: %s", esp_err_to_name(err));
        CleanupStartFailure();
        return false;
    } else {
        ESP_LOGE(TAG, "WiFi became initialized concurrently");
        CleanupStartFailure();
        return false;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &StackChanWifiStation::WifiEventHandler, this, &instance_any_id_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi event registration failed: %s", esp_err_to_name(err));
        CleanupStartFailure();
        return false;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &StackChanWifiStation::IpEventHandler, this, &instance_got_ip_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IP event registration failed: %s", esp_err_to_name(err));
        CleanupStartFailure();
        return false;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi station startup failed: %s", esp_err_to_name(err));
        CleanupStartFailure();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        is_started_ = true;
    }
    return true;
}

bool StackChanWifiStation::WaitForConnected(int timeout_ms)
{
    if (event_group_ == nullptr || timeout_ms < 0) {
        return false;
    }
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_STOPPED, pdFALSE, pdFALSE,
                                    pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

int8_t StackChanWifiStation::GetRssi()
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

uint8_t StackChanWifiStation::GetChannel()
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.primary;
    }
    return 0;
}

bool StackChanWifiStation::IsConnected()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return is_started_ && event_group_ != nullptr &&
           (xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED);
}

std::string StackChanWifiStation::GetSsid() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return ssid_;
}

std::string StackChanWifiStation::GetIpAddress() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return ip_address_;
}

void StackChanWifiStation::SetPowerSaveMode(bool enabled)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!is_started_) {
        return;
    }
    esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void StackChanWifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* this_ = static_cast<StackChanWifiStation*>(arg);
    try {
        if (event_id == WIFI_EVENT_STA_START) {
            // Do not auto connect on start
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            std::function<void(const std::string&)> failure_callback;
            std::string ssid;
            int retry_count = 0;
            {
                std::lock_guard<std::mutex> lock(this_->state_mutex_);
                if (!this_->is_started_) {
                    return;
                }
                ssid = this_->ssid_;
                if (this_->event_group_ != nullptr) {
                    xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED);
                }

                // Only retry if we are actively trying to connect.
                if (this_->is_connecting_) {
                    if (this_->reconnect_count_ < MAX_RECONNECT_COUNT) {
                        this_->reconnect_count_++;
                        retry_count = this_->reconnect_count_;
                    } else {
                        this_->is_connecting_ = false;
                        failure_callback      = this_->on_connect_failed_;
                    }
                }
            }

            if (retry_count != 0) {
                ESP_LOGI(TAG, "Retry to connect to the AP (attempt %d/%d)", retry_count, MAX_RECONNECT_COUNT);
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WiFi retry failed: %s", esp_err_to_name(err));
                    {
                        std::lock_guard<std::mutex> lock(this_->state_mutex_);
                        if (this_->is_started_) {
                            this_->is_connecting_ = false;
                            failure_callback      = this_->on_connect_failed_;
                            ssid                  = this_->ssid_;
                        }
                    }
                }
            }

            InvokeCallbackSafely(failure_callback, ssid, "connection failure");
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "WiFi event handler failed: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "WiFi event handler failed with an unknown exception");
    }
}

void StackChanWifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* this_ = static_cast<StackChanWifiStation*>(arg);
    try {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);

        char ip_address[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
        std::string connected_ssid;
        wifi_config_t conf = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
            const char* ssid = reinterpret_cast<const char*>(conf.sta.ssid);
            connected_ssid.assign(ssid, strnlen(ssid, sizeof(conf.sta.ssid)));
        }

        std::function<void(const std::string&)> connected_callback;
        {
            std::lock_guard<std::mutex> lock(this_->state_mutex_);
            if (!this_->is_started_) {
                return;
            }
            this_->ip_address_      = ip_address;
            this_->reconnect_count_ = 0;
            this_->is_connecting_   = false;
            if (!connected_ssid.empty()) {
                this_->ssid_ = connected_ssid;
            }
            connected_ssid     = this_->ssid_;
            connected_callback = this_->on_connected_;
        }

        ESP_LOGI(TAG, "Got IP: %s", ip_address);
        if (this_->event_group_ != nullptr) {
            xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED);
        }
        InvokeCallbackSafely(connected_callback, connected_ssid, "connected");
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "IP event handler failed: %s", e.what());
    } catch (...) {
        ESP_LOGE(TAG, "IP event handler failed with an unknown exception");
    }
}
