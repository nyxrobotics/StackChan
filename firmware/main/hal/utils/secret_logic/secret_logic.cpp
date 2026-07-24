/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "secret_logic.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <sdkconfig.h>
#include <cstdio>
#include <cstring>
#include <vector>

#define TAG "secret_logic"

namespace {

constexpr char kStackChanBluePublicKey[] = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAzEA9BzulpncMvpUp2aAs
TRSiB5nvF4oQpbfYjGeGInDka1ZzFy0yJ4mjxlPFJ9AcZUIJD2vWxUUKQOf9feU8
RREJCHIe2rEhx1LzvIbt2FaDgup2QUSQsjAX+MKeS6121AScrHJTv9M7tWnVYwsz
6pHk7/6qJ3MP1E7JHbqc8y93VBRqlOFNgUGXmspP5MHuhSyTj8WrKew+jfMyuxVB
mIWpGN5weM3gewVKJufiC2geF4+D9gHHivjrkG/4k5YM5u3tFQ7N+3g1cx7rC1Oa
9Umydxd0UMdCVacUtPpo3HsmK5fTwPJ/nS6n5Elc18q+081ypE1Y3aY8MMji07VJ
jQIDAQAB
-----END PUBLIC KEY-----
)";

std::string mbedtls_error(int ret)
{
    char buffer[128] = {};
    mbedtls_strerror(ret, buffer, sizeof(buffer));
    return buffer;
}

std::string get_wifi_sta_mac_hex()
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGW(TAG, "failed to read Wi-Fi STA MAC for BLE handshake");
        return {};
    }

    char buffer[13] = {};
    std::snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                  mac[5]);
    return buffer;
}

std::string base64_encode(const unsigned char* data, size_t length)
{
    size_t output_length = 0;
    int ret = mbedtls_base64_encode(nullptr, 0, &output_length, data, length);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGW(TAG, "base64 length calculation failed: %s", mbedtls_error(ret).c_str());
        return {};
    }

    std::string output(output_length, '\0');
    ret = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output.data()), output.size(), &output_length, data,
                                length);
    if (ret != 0) {
        ESP_LOGW(TAG, "base64 encode failed: %s", mbedtls_error(ret).c_str());
        return {};
    }

    output.resize(output_length);
    return output;
}

std::string encrypt_stackchan_blue_payload(std::string_view payload)
{
    mbedtls_pk_context public_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_pk_init(&public_key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    std::string result;
    const char* personalization = "stackchan_ble";

    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                    reinterpret_cast<const unsigned char*>(personalization),
                                    std::strlen(personalization));
    if (ret != 0) {
        ESP_LOGW(TAG, "CTR_DRBG seed failed: %s", mbedtls_error(ret).c_str());
        goto cleanup;
    }

    ret = mbedtls_pk_parse_public_key(&public_key, reinterpret_cast<const unsigned char*>(kStackChanBluePublicKey),
                                      sizeof(kStackChanBluePublicKey));
    if (ret != 0) {
        ESP_LOGW(TAG, "parse StackChan BLE public key failed: %s", mbedtls_error(ret).c_str());
        goto cleanup;
    }

    if (!mbedtls_pk_can_do(&public_key, MBEDTLS_PK_RSA)) {
        ESP_LOGW(TAG, "StackChan BLE public key is not RSA");
        goto cleanup;
    }

    mbedtls_rsa_set_padding(mbedtls_pk_rsa(public_key), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    {
        std::vector<unsigned char> encrypted(mbedtls_pk_get_len(&public_key));
        size_t encrypted_length = 0;
        ret                     = mbedtls_pk_encrypt(&public_key, reinterpret_cast<const unsigned char*>(payload.data()),
                                                     payload.size(), encrypted.data(), &encrypted_length,
                                                     encrypted.size(), mbedtls_ctr_drbg_random, &ctr_drbg);
        if (ret != 0) {
            ESP_LOGW(TAG, "StackChan BLE handshake encryption failed: %s", mbedtls_error(ret).c_str());
            goto cleanup;
        }

        result = base64_encode(encrypted.data(), encrypted_length);
    }

cleanup:
    mbedtls_pk_free(&public_key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}

}  // namespace

namespace secret_logic {

__attribute__((weak)) std::string get_server_url()
{
#ifdef CONFIG_STACKCHAN_SERVER_URL
    return CONFIG_STACKCHAN_SERVER_URL;
#else
    return "http://localhost:3000";
#endif
}

__attribute__((weak)) std::string generate_auth_token()
{
    return "hi-stack-chan";
}

__attribute__((weak)) std::string generate_handshake_token(std::string_view data)
{
    auto mac = get_wifi_sta_mac_hex();
    if (mac.empty()) {
        return {};
    }

    std::string payload = mac;
    payload.append(data);
    return encrypt_stackchan_blue_payload(payload);
}

}  // namespace secret_logic
