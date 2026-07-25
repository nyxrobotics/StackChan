/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "esp_now_init.h"
#include "freertos/FreeRTOS.h"

static bool s_espnow_initialized = false;

static void wifi_cleanup_after_init_failure(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
}

/**
 * @brief Initialize WiFi in STA mode and ESP-NOW with the specified channel
 * @param channel The WiFi channel to use (1-13)
 * @note This function initializes both WiFi subsystem in Station mode and ESP-NOW for communication
 * @details
 *      1. Initializes network interface
 *      2. Creates default event loop
 *      3. Initializes WiFi with default configuration
 *      4. Sets WiFi storage to RAM only
 *      5. Configures WiFi mode to Station (STA)
 *      6. Starts WiFi
 *      7. Sets the specified WiFi channel
 *      8. Optionally enables long range protocol if CONFIG_ESPNOW_ENABLE_LONG_RANGE is defined
 *      9. Configures ESP-NOW with forwarding disabled, 5 retry attempts, and receive disabled
 *      10. Initializes ESP-NOW with the configured parameters
 *      11. Reads and logs the device MAC address
 * @warning This function combines both WiFi and ESP-NOW initialization in a single call
 */
esp_err_t wifi_espnow_init(uint8_t channel)
{
    if (channel < 1 || channel > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_espnow_initialized) {
        return wifi_espnow_set_channel(channel) < 0 ? ESP_FAIL : ESP_OK;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((ret = esp_wifi_init(&cfg)) != ESP_OK) {
        return ret;
    }
    if ((ret = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (ret = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK || (ret = esp_wifi_start()) != ESP_OK ||
        (ret = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE)) != ESP_OK) {
        wifi_cleanup_after_init_failure();
        return ret;
    }

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    if ((ret = esp_wifi_set_protocol(
             ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR)) != ESP_OK) {
        wifi_cleanup_after_init_failure();
        return ret;
    }
#endif
    espnow_config_t espnow_config = ESPNOW_INIT_CONFIG_DEFAULT();

    espnow_config.forward_enable         = false;
    espnow_config.forward_switch_channel = false;
    espnow_config.send_retry_num         = 5;
    espnow_config.receive_enable.forward = false;
    espnow_config.receive_enable.data    = false;

    if ((ret = espnow_init(&espnow_config)) != ESP_OK) {
        wifi_cleanup_after_init_failure();
        return ret;
    }
    s_espnow_initialized = true;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    ESP_LOGI("espnow_init", "ESP-NOW initialized with MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/**
 * @brief Change the WiFi channel without tearing down ESP-NOW
 * @param new_channel The new WiFi channel to use (1-13)
 * @return uint8_t The actual channel that was set
 * @note ESP-NOW remains initialized, so a failed channel change can be retried safely.
 * @details
 *      1. Checks if the new channel is different from current channel
 *      2. Changes the active WiFi channel
 *      3. Reads the channel back to verify the change
 * @warning Communication pauses briefly while the radio changes channel.
 */
int wifi_espnow_set_channel(uint8_t new_channel)
{
    if (!s_espnow_initialized || new_channel < 1 || new_channel > 13) {
        ESP_LOGE("wifi channel", "Invalid ESP-NOW state or channel: %u", new_channel);
        return -1;
    }

    uint8_t channel = 0;
    wifi_second_chan_t second;
    esp_err_t ret = esp_wifi_get_channel(&channel, &second);
    if (ret != ESP_OK) {
        ESP_LOGE("wifi channel", "Failed to read current channel: %s", esp_err_to_name(ret));
        return -1;
    }
    if (new_channel == channel) {
        ESP_LOGI("wifi channel", "Already using channel %u", channel);
        return channel;
    }

    ret = esp_wifi_set_channel(new_channel, WIFI_SECOND_CHAN_NONE);
    if (ret != ESP_OK) {
        ESP_LOGE("wifi channel", "Failed to set channel %u: %s", new_channel, esp_err_to_name(ret));
        return -1;
    }

    if ((ret = esp_wifi_get_channel(&channel, &second)) != ESP_OK) {
        ESP_LOGE("wifi channel", "Failed to verify channel: %s", esp_err_to_name(ret));
        return -1;
    }
    if (channel != new_channel) {
        ESP_LOGE("wifi channel", "Channel verification failed: requested=%u actual=%u", new_channel, channel);
        return -1;
    }
    ESP_LOGI("wifi channel", "Current channel: %u", channel);
    return channel;
}

/**
 * @brief Send data packet via ESP-NOW broadcast
 * @param pkt Pointer to the data packet to send
 * @param len Length of the data packet in bytes
 * @note This function sends data using ESP-NOW broadcast address
 * @details
 *      1. Creates default ESP-NOW frame header
 *      2. Sends data using ESPNOW_DATA_TYPE_DATA type
 *      3. Uses broadcast address to send to all devices on the same channel
 *      4. Waits for at most 100ms for transmission to complete
 */
esp_err_t espnow_send_data(uint8_t *pkt, size_t len)
{
    if (!s_espnow_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pkt == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    espnow_frame_head_t frame_head = ESPNOW_FRAME_CONFIG_DEFAULT();
    return espnow_send(ESPNOW_DATA_TYPE_DATA, ESPNOW_ADDR_BROADCAST, pkt, len, &frame_head, pdMS_TO_TICKS(100));
}
