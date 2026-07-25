/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <atomic>
#include <mooncake_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_event.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <espnow.h>
#include <espnow_storage.h>
#include <espnow_utils.h>
#include <esp_check.h>
#include <wifi_manager.h>

static const std::string_view _tag = "HAL-EspNow";
static std::atomic<bool> _espnow_ready{false};

static bool _wifi_init(int channel = 1)
{
    mclog::tagInfo(_tag, "wifi init");
    if (channel < 1 || channel > 13) {
        mclog::tagError(_tag, "invalid WiFi channel: {}", channel);
        return false;
    }

    auto& wifi_manager = WifiManager::GetInstance();
    if (wifi_manager.IsInitialized() && wifi_manager.IsConfigMode()) {
        mclog::tagError(_tag, "cannot start ESP-NOW while WiFi configuration mode is active");
        return false;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        mclog::tagError(_tag, "esp_netif_init failed: {}", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        mclog::tagError(_tag, "event loop init failed: {}", esp_err_to_name(ret));
        return false;
    }

    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == nullptr) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (sta_netif == nullptr) {
        mclog::tagError(_tag, "failed to create WiFi station interface");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        mclog::tagError(_tag, "WiFi init failed: {}", esp_err_to_name(ret));
        return false;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to read WiFi mode: {}", esp_err_to_name(ret));
        return false;
    }
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            mclog::tagError(_tag, "WiFi mode setup failed: {}", esp_err_to_name(ret));
            return false;
        }
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "WiFi start failed: {}", esp_err_to_name(ret));
        return false;
    }

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        if (channel != ap_info.primary) {
            mclog::tagError(_tag, "ESP-NOW channel {} does not match connected WiFi channel {}", channel,
                            ap_info.primary);
            return false;
        }
        return true;
    }

    // 建议先开启混杂模式再设信道，确保射频频率被强制锁定
    ret = esp_wifi_set_promiscuous(true);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "failed to enter WiFi channel configuration mode: {}", esp_err_to_name(ret));
        return false;
    }

    ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_err_t restore_ret = esp_wifi_set_promiscuous(false);
    if (ret != ESP_OK || restore_ret != ESP_OK) {
        mclog::tagError(_tag, "WiFi channel setup failed: set={}, restore={}", esp_err_to_name(ret),
                        esp_err_to_name(restore_ret));
        return false;
    }

    mclog::tagInfo(_tag, "wifi channel set to {}", channel);
    return true;
}

static esp_err_t _handle_espnow_received(uint8_t* src_addr, void* data, size_t size, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    const char* TAG = "EspNow";

    ESP_PARAM_CHECK(src_addr);
    ESP_PARAM_CHECK(data);
    ESP_PARAM_CHECK(size);
    ESP_PARAM_CHECK(rx_ctrl);
    if (!_espnow_ready.load(std::memory_order_acquire)) {
        return ESP_OK;
    }

    static uint32_t count = 0;

    ESP_LOGD(TAG, "espnow_recv, <%" PRIu32 "> [" MACSTR "][%d][%d][%u]: %.*s", count++, MAC2STR(src_addr),
             rx_ctrl->channel, rx_ctrl->rssi, size, size, "~");

    std::vector<uint8_t> received_data((uint8_t*)data, (uint8_t*)data + size);
    GetHAL().onEspNowData.emit(received_data);

    return ESP_OK;
}

bool Hal::startEspNow(int channel)
{
    mclog::tagInfo(_tag, "start EspNow on channel {}", channel);
    _espnow_ready.store(false, std::memory_order_release);

    if (!_wifi_init(channel)) {
        return false;
    }

    espnow_config_t espnow_config = ESPNOW_INIT_CONFIG_DEFAULT();

    // 2. 修改关键参数以兼容 Arduino
    espnow_config.forward_enable         = false;  // 关闭转发（多跳），Arduino 无法解析带转发头的包
    espnow_config.forward_switch_channel = false;  // 关闭自动切信道
    espnow_config.send_retry_num         = 5;      // 失败重试次数（可按需调，建议5-10）

    // 3. 修改接收使能开关
    espnow_config.receive_enable.forward = false;  // 关闭转发包接收
    espnow_config.receive_enable.data    = true;   // 必须开启这个，才能接收 Arduino 发来的普通数据包

    esp_err_t ret = espnow_init(&espnow_config);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "ESP-NOW init failed: {}", esp_err_to_name(ret));
        return false;
    }
    ret = espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_DATA, true, _handle_espnow_received);
    if (ret != ESP_OK) {
        mclog::tagError(_tag, "ESP-NOW receive handler setup failed: {}", esp_err_to_name(ret));
        esp_err_t cleanup_ret = espnow_deinit();
        if (cleanup_ret != ESP_OK) {
            mclog::tagWarn(_tag, "ESP-NOW cleanup after setup failure failed: {}", esp_err_to_name(cleanup_ret));
        }
        return false;
    }

    mclog::tagInfo(_tag, "factory mac: {}", getFactoryMacString());
    _espnow_ready.store(true, std::memory_order_release);
    return true;
}

bool Hal::espNowSend(const std::vector<uint8_t>& data, const uint8_t* destAddr)
{
    if (!_espnow_ready.load(std::memory_order_acquire) || data.empty()) {
        return false;
    }

    espnow_frame_head_t frame_head = ESPNOW_FRAME_CONFIG_DEFAULT();
    esp_err_t ret                  = ESP_FAIL;
    constexpr TickType_t kSendTimeout = pdMS_TO_TICKS(100);

    if (destAddr == nullptr) {
        ret = espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, data.data(), data.size(), &frame_head,
                          kSendTimeout);
    } else {
        ret = espnow_send(ESPNOW_DATA_TYPE_DATA, destAddr, data.data(), data.size(), &frame_head, kSendTimeout);
    }

    if (ret != ESP_OK) {
        static uint32_t last_error_log_ms = 0;
        uint32_t now = millis();
        if (last_error_log_ms == 0 || now - last_error_log_ms >= 1000) {
            mclog::tagError(_tag, "send failed: {}", esp_err_to_name(ret));
            last_error_log_ms = now;
        }
        return false;
    }
    return true;
}

#include <driver/gpio.h>

void Hal::setLaserEnabled(bool enabled)
{
    static bool laser_enabled = false;
    static bool is_inited     = false;

    if (laser_enabled == enabled) {
        return;
    }

    const gpio_num_t laser_pin = GPIO_NUM_2;

    if (!is_inited) {
        gpio_reset_pin(laser_pin);
        gpio_set_direction(laser_pin, GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(laser_pin, GPIO_PULLUP_ONLY);
        is_inited = true;
    }

    mclog::tagInfo(_tag, "set laser {}", enabled ? "enabled" : "disabled");

    if (enabled) {
        gpio_set_level(laser_pin, 1);
    } else {
        gpio_set_level(laser_pin, 0);
    }
    laser_enabled = enabled;
}
