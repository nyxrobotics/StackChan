/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* BLE */
#include "bleprph.h"
#include "console/console.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#if CONFIG_EXAMPLE_EXTENDED_ADV
static uint8_t ext_adv_pattern_1[] = {
    0x02,
    BLE_HS_ADV_TYPE_FLAGS,
    0x06,
    0x03,
    BLE_HS_ADV_TYPE_COMP_UUIDS16,
    0xab,
    0xcd,
    0x03,
    BLE_HS_ADV_TYPE_COMP_UUIDS16,
    0x18,
    0x11,
    0x11,
    BLE_HS_ADV_TYPE_COMP_NAME,
    'n',
    'i',
    'm',
    'b',
    'l',
    'e',
    '-',
    'b',
    'l',
    'e',
    'p',
    'r',
    'p',
    'h',
    '-',
    'e',
};
#endif

static const char *tag = "NimBLE_BLE_PRPH";
static int bleprph_gap_event(struct ble_gap_event *event, void *arg);
#if CONFIG_EXAMPLE_RANDOM_ADDR
static uint8_t own_addr_type = BLE_OWN_ADDR_RANDOM;
#else
static uint8_t own_addr_type;
#endif

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
static uint16_t cids[MYNEWT_VAL(BLE_EATT_CHAN_NUM)];
static uint16_t bearers;
#endif

typedef enum {
    BLEPRPH_STATE_UNINITIALIZED,
    BLEPRPH_STATE_INITIALIZING,
    BLEPRPH_STATE_INITIALIZED,
    BLEPRPH_STATE_FAILED,
} bleprph_state_t;

static bool s_use_alt_uuid = false;
static uint16_t s_notify_payload_len = 20;
static bleprph_state_t s_bleprph_state = BLEPRPH_STATE_UNINITIALIZED;
static portMUX_TYPE s_bleprph_state_lock = portMUX_INITIALIZER_UNLOCKED;

void ble_store_config_init(void);

static void bleprph_set_notify_payload_len(uint16_t payload_len)
{
    taskENTER_CRITICAL(&s_bleprph_state_lock);
    s_notify_payload_len = payload_len;
    taskEXIT_CRITICAL(&s_bleprph_state_lock);
}

#if NIMBLE_BLE_CONNECT
/**
 * Logs information about a connection to the console.
 */
static void bleprph_print_conn_desc(struct ble_gap_conn_desc *desc)
{
    MODLOG_DFLT(INFO, "handle=%d our_ota_addr_type=%d our_ota_addr=", desc->conn_handle, desc->our_ota_addr.type);
    print_addr(desc->our_ota_addr.val);
    MODLOG_DFLT(INFO, " our_id_addr_type=%d our_id_addr=", desc->our_id_addr.type);
    print_addr(desc->our_id_addr.val);
    MODLOG_DFLT(INFO, " peer_ota_addr_type=%d peer_ota_addr=", desc->peer_ota_addr.type);
    print_addr(desc->peer_ota_addr.val);
    MODLOG_DFLT(INFO, " peer_id_addr_type=%d peer_id_addr=", desc->peer_id_addr.type);
    print_addr(desc->peer_id_addr.val);
    MODLOG_DFLT(INFO,
                " conn_itvl=%d conn_latency=%d supervision_timeout=%d "
                "encrypted=%d authenticated=%d bonded=%d\n",
                desc->conn_itvl, desc->conn_latency, desc->supervision_timeout, desc->sec_state.encrypted,
                desc->sec_state.authenticated, desc->sec_state.bonded);
}
#endif

#if CONFIG_EXAMPLE_EXTENDED_ADV
/**
 * Enables advertising with the following parameters:
 *     o General discoverable mode.
 *     o Undirected connectable mode.
 */
static int ext_bleprph_advertise(void)
{
    struct ble_gap_ext_adv_params params;
    struct os_mbuf *data;
    uint8_t instance = 0;
    int rc;

    /* First check if any instance is already active */
    if (ble_gap_ext_adv_active(instance)) {
        return 0;
    }

    /* use defaults for non-set params */
    memset(&params, 0, sizeof(params));

    /* enable connectable advertising */
    params.connectable = 1;

    /* advertise using random addr */
    params.own_addr_type = BLE_OWN_ADDR_PUBLIC;

    params.primary_phy   = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_2M;
    params.tx_power      = 127;
    params.sid           = 1;

    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MIN;

    /* configure instance 0 */
    rc = ble_gap_ext_adv_configure(instance, &params, NULL, bleprph_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error configuring extended advertisement; rc=%d", rc);
        return rc;
    }

    /* in this case only scan response is allowed */

    /* get mbuf for scan rsp data */
    data = os_msys_get_pkthdr(sizeof(ext_adv_pattern_1), 0);
    if (data == NULL) {
        MODLOG_DFLT(ERROR, "cannot allocate extended advertisement data");
        return BLE_HS_ENOMEM;
    }

    /* fill mbuf with scan rsp data */
    rc = os_mbuf_append(data, ext_adv_pattern_1, sizeof(ext_adv_pattern_1));
    if (rc != 0) {
        os_mbuf_free_chain(data);
        MODLOG_DFLT(ERROR, "cannot append extended advertisement data; rc=%d", rc);
        return rc;
    }

    rc = ble_gap_ext_adv_set_data(instance, data);
    if (rc != 0) {
        os_mbuf_free_chain(data);
        MODLOG_DFLT(ERROR, "cannot set extended advertisement data; rc=%d", rc);
        return rc;
    }

    /* start advertising */
    rc = ble_gap_ext_adv_start(instance, 0, 0);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "cannot start extended advertisement; rc=%d", rc);
    }
    return rc;
}
#else

// /**
//  * Enables advertising with the following parameters:
//  *     o General discoverable mode.
//  *     o Undirected connectable mode.
//  */
// static void bleprph_advertise(void)
// {
//     struct ble_gap_adv_params adv_params;
//     struct ble_hs_adv_fields fields;
// #if CONFIG_BT_NIMBLE_GAP_SERVICE
//     const char *name;
// #endif
//     int rc;

//     /**
//      *  Set the advertisement data included in our advertisements:
//      *     o Flags (indicates advertisement type and other general info).
//      *     o Advertising tx power.
//      *     o Device name.
//      *     o 16-bit service UUIDs (alert notifications).
//      */

//     memset(&fields, 0, sizeof fields);

//     /* Advertise two flags:
//      *     o Discoverability in forthcoming advertisement (general)
//      *     o BLE-only (BR/EDR unsupported).
//      */
//     fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

//     /* Indicate that the TX power level field should be included; have the
//      * stack fill this value automatically.  This is done by assigning the
//      * special value BLE_HS_ADV_TX_PWR_LVL_AUTO.
//      */
//     fields.tx_pwr_lvl_is_present = 1;
//     fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;

// #if CONFIG_BT_NIMBLE_GAP_SERVICE
//     name                         = ble_svc_gap_device_name();
//     fields.name                  = (uint8_t *)name;
//     fields.name_len              = strlen(name);
//     fields.name_is_complete      = 1;
// #endif

//     fields.uuids16             = (ble_uuid16_t[]){BLE_UUID16_INIT(GATT_SVR_SVC_ALERT_UUID)};
//     fields.num_uuids16         = 1;
//     fields.uuids16_is_complete = 1;

//     rc = ble_gap_adv_set_fields(&fields);
//     if (rc != 0) {
//         MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
//         return;
//     }

//     /* Begin advertising. */
//     memset(&adv_params, 0, sizeof adv_params);
//     adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
//     adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
//     rc                   = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bleprph_gap_event,
//     NULL); if (rc != 0) {
//         MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
//         return;
//     }
// }

static int bleprph_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    // 获取MAC地址
    uint8_t mac[6] = {0};
    rc = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (rc != ESP_OK) {
        MODLOG_DFLT(ERROR, "error reading factory MAC; rc=%d", rc);
        return rc;
    }

    // 厂商数据
    // uint8_t mfg_data[10];
    uint8_t mfg_data[8];
    memset(mfg_data, 0, sizeof(mfg_data));

    mfg_data[0] = 0xE5;
    mfg_data[1] = 0x02;
    memcpy(&mfg_data[2], mac, 6);
    // mfg_data[8] = 0x01;
    // mfg_data[9] = 0x10;

    // ========== 广播包：只放必要信息 ==========
    memset(&fields, 0, sizeof fields);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // 只放UUID
    // fields.uuids16             = (ble_uuid16_t[]){BLE_UUID16_INIT(GATT_SVR_SVC_ALERT_UUID)};
    // fields.num_uuids16         = 1;
    // fields.uuids16_is_complete = 1;

    ble_uuid128_t stackchan_uuid     = BLE_UUID128_INIT(STACKCHAN_SVC_UUID_BASE);
    ble_uuid128_t stackchan_uuid_alt = BLE_UUID128_INIT(STACKCHAN_SVC_UUID_BASE_ALT);

    if (s_use_alt_uuid) {
        fields.uuids128 = &stackchan_uuid_alt;
    } else {
        fields.uuids128 = &stackchan_uuid;
    }

    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return rc;
    }

    // ========== 扫描响应包：放详细信息 ==========
    memset(&rsp_fields, 0, sizeof rsp_fields);

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    const char *name            = ble_svc_gap_device_name();
    if (name != NULL) {
        rsp_fields.name             = (uint8_t *)name;
        rsp_fields.name_len         = strlen(name);
        rsp_fields.name_is_complete = 1;
    }
#endif

    // TX Power放在扫描响应
    rsp_fields.tx_pwr_lvl_is_present = 1;
    rsp_fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    // 厂商数据放在扫描响应
    rsp_fields.mfg_data     = mfg_data;
    rsp_fields.mfg_data_len = sizeof(mfg_data);

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting scan response data; rc=%d\n", rc);
        return rc;
    }

    // 启动广播
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bleprph_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
    }
    return rc;
}

#endif

#if MYNEWT_VAL(BLE_POWER_CONTROL)
static int bleprph_power_control(uint16_t conn_handle)
{
    int rc;

    rc = ble_gap_read_remote_transmit_power_level(conn_handle, 0x01);  // Attempting on LE 1M phy
    if (rc != 0) {
        MODLOG_DFLT(WARN, "cannot read remote transmit power; rc=%d", rc);
        return rc;
    }

    rc = ble_gap_set_transmit_power_reporting_enable(conn_handle, 0x1, 0x1);
    if (rc != 0) {
        MODLOG_DFLT(WARN, "cannot enable transmit power reporting; rc=%d", rc);
    }
    return rc;
}
#endif

/**
 * The nimble host executes this callback when a GAP event occurs.  The
 * application associates a GAP event callback with each connection that forms.
 * bleprph uses the same callback for all connections.
 *
 * @param event                 The type of event being signalled.
 * @param ctxt                  Various information pertaining to the event.
 * @param arg                   Application-specified argument; unused by
 *                                  bleprph.
 *
 * @return                      0 if the application successfully handled the
 *                                  event; nonzero on failure.  The semantics
 *                                  of the return code is specific to the
 *                                  particular GAP event being signalled.
 */
static int bleprph_gap_event(struct ble_gap_event *event, void *arg)
{
#if NIMBLE_BLE_CONNECT
    struct ble_gap_conn_desc desc;
    int rc;
#endif

    (void)arg;
    if (event == NULL) {
        return BLE_HS_EINVAL;
    }

    switch (event->type) {
#if NIMBLE_BLE_CONNECT
        case BLE_GAP_EVENT_CONNECT:
            /* A new connection was established or a connection attempt failed. */
            MODLOG_DFLT(INFO, "connection %s; status=%d ", event->connect.status == 0 ? "established" : "failed",
                        event->connect.status);
            if (event->connect.status == 0) {
                rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
                if (rc == 0) {
                    bleprph_print_conn_desc(&desc);
                } else {
                    MODLOG_DFLT(WARN, "cannot inspect new connection; rc=%d", rc);
                }
                stackchan_ble_set_conn_handle(event->connect.conn_handle);
                bleprph_set_notify_payload_len(20);
#if MYNEWT_VAL(BLE_POWER_CONTROL)
                bleprph_power_control(event->connect.conn_handle);
#endif
            }
            MODLOG_DFLT(INFO, "\n");

            if (event->connect.status != 0) {
                /* Connection failed; resume advertising. */
#if CONFIG_EXAMPLE_EXTENDED_ADV
                ext_bleprph_advertise();
#else
                bleprph_advertise();
#endif
            }

            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
            bleprph_print_conn_desc(&event->disconnect.conn);
            stackchan_ble_set_conn_handle(BLE_HS_CONN_HANDLE_NONE);
            bleprph_set_notify_payload_len(20);
            MODLOG_DFLT(INFO, "\n");

            /* Connection terminated; resume advertising. */
#if CONFIG_EXAMPLE_EXTENDED_ADV
            ext_bleprph_advertise();
#else
            bleprph_advertise();
#endif
            return 0;

        case BLE_GAP_EVENT_CONN_UPDATE:
            /* The central has updated the connection parameters. */
            MODLOG_DFLT(INFO, "connection updated; status=%d ", event->conn_update.status);
            rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
            if (rc == 0) {
                bleprph_print_conn_desc(&desc);
            } else {
                MODLOG_DFLT(WARN, "cannot inspect updated connection; rc=%d", rc);
            }
            MODLOG_DFLT(INFO, "\n");
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            MODLOG_DFLT(INFO, "advertise complete; reason=%d", event->adv_complete.reason);
#if CONFIG_EXAMPLE_EXTENDED_ADV
            ext_bleprph_advertise();
#else
            bleprph_advertise();
#endif
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            /* Encryption has been enabled or disabled for this connection. */
            MODLOG_DFLT(INFO, "encryption change event; status=%d ", event->enc_change.status);
            rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc == 0) {
                bleprph_print_conn_desc(&desc);
            } else {
                MODLOG_DFLT(WARN, "cannot inspect encrypted connection; rc=%d", rc);
            }
            MODLOG_DFLT(INFO, "\n");
            return 0;

        case BLE_GAP_EVENT_NOTIFY_TX:
            if (event->notify_tx.status != 0) {
                MODLOG_DFLT(WARN, "notify failed; conn_handle=%d attr_handle=%d status=%d indication=%d",
                            event->notify_tx.conn_handle, event->notify_tx.attr_handle, event->notify_tx.status,
                            event->notify_tx.indication);
            } else {
                MODLOG_DFLT(DEBUG, "notify complete; conn_handle=%d attr_handle=%d",
                            event->notify_tx.conn_handle, event->notify_tx.attr_handle);
            }
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            MODLOG_DFLT(INFO,
                        "subscribe event; conn_handle=%d attr_handle=%d "
                        "reason=%d prevn=%d curn=%d previ=%d curi=%d\n",
                        event->subscribe.conn_handle, event->subscribe.attr_handle, event->subscribe.reason,
                        event->subscribe.prev_notify, event->subscribe.cur_notify, event->subscribe.prev_indicate,
                        event->subscribe.cur_indicate);
            return 0;

        case BLE_GAP_EVENT_MTU:
            MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n", event->mtu.conn_handle,
                        event->mtu.channel_id, event->mtu.value);
            if (event->mtu.value > 3) {
                bleprph_set_notify_payload_len(event->mtu.value - 3);
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            /* We already have a bond with the peer, but it is attempting to
             * establish a new secure link.  This app sacrifices security for
             * convenience: just throw away the old bond and accept the new link.
             */

            /* Delete the old bond. */
            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
            if (rc != 0) {
                MODLOG_DFLT(WARN, "cannot inspect repeat-pairing connection; rc=%d", rc);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            if (rc != 0) {
                MODLOG_DFLT(WARN, "cannot delete previous peer bond; rc=%d", rc);
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
             * continue with the pairing operation.
             */
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            ESP_LOGI(tag, "PASSKEY_ACTION_EVENT started");
            struct ble_sm_io pkey = {0};
            int key               = 0;

            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                pkey.action  = event->passkey.params.action;
                pkey.passkey = 123456;  // This is the passkey to be entered on peer
                ESP_LOGI(tag, "Enter passkey %" PRIu32 "on the peer side", pkey.passkey);
                rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
            } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
                ESP_LOGI(tag, "Passkey on device's display: %" PRIu32, event->passkey.params.numcmp);
                ESP_LOGI(tag,
                         "Accept or reject the passkey through console in this "
                         "format -> key Y or key N");
                pkey.action = event->passkey.params.action;
                if (scli_receive_key(&key)) {
                    pkey.numcmp_accept = key;
                } else {
                    pkey.numcmp_accept = 0;
                    ESP_LOGE(tag, "Timeout! Rejecting the key");
                }
                rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
            } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
                static uint8_t tem_oob[16] = {0};
                pkey.action                = event->passkey.params.action;
                for (int i = 0; i < 16; i++) {
                    pkey.oob[i] = tem_oob[i];
                }
                rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
            } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
                ESP_LOGI(tag, "Enter the passkey through console in this format-> key 123456");
                pkey.action = event->passkey.params.action;
                if (scli_receive_key(&key)) {
                    pkey.passkey = key;
                } else {
                    pkey.passkey = 0;
                    ESP_LOGE(tag, "Timeout! Passing 0 as the key");
                }
                rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
                ESP_LOGI(tag, "ble_sm_inject_io result: %d", rc);
            }
            return 0;

        case BLE_GAP_EVENT_AUTHORIZE:
            MODLOG_DFLT(INFO, "authorize event: conn_handle=%d attr_handle=%d is_read=%d", event->authorize.conn_handle,
                        event->authorize.attr_handle, event->authorize.is_read);

            /* The default behaviour for the event is to reject authorize request */
            event->authorize.out_response = BLE_GAP_AUTHORIZE_REJECT;
            return 0;

#if MYNEWT_VAL(BLE_POWER_CONTROL)
        case BLE_GAP_EVENT_TRANSMIT_POWER:
            MODLOG_DFLT(INFO,
                        "Transmit power event : status=%d conn_handle=%d reason=%d "
                        "phy=%d power_level=%x power_level_flag=%d delta=%d",
                        event->transmit_power.status, event->transmit_power.conn_handle, event->transmit_power.reason,
                        event->transmit_power.phy, event->transmit_power.transmit_power_level,
                        event->transmit_power.transmit_power_level_flag, event->transmit_power.delta);
            return 0;

        case BLE_GAP_EVENT_PATHLOSS_THRESHOLD:
            MODLOG_DFLT(INFO,
                        "Pathloss threshold event : conn_handle=%d current path loss=%d "
                        "zone_entered =%d",
                        event->pathloss_threshold.conn_handle, event->pathloss_threshold.current_path_loss,
                        event->pathloss_threshold.zone_entered);
            return 0;
#endif

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
        case BLE_GAP_EVENT_EATT:
            MODLOG_DFLT(INFO, "EATT %s : conn_handle=%d cid=%d", event->eatt.status ? "disconnected" : "connected",
                        event->eatt.conn_handle, event->eatt.cid);
            if (event->eatt.status) {
                /* Abort if disconnected */
                return 0;
            }
            if (bearers >= MYNEWT_VAL(BLE_EATT_CHAN_NUM)) {
                MODLOG_DFLT(WARN, "ignoring excess EATT bearer cid=%d", event->eatt.cid);
                return 0;
            }
            cids[bearers] = event->eatt.cid;
            bearers += 1;
            if (bearers != MYNEWT_VAL(BLE_EATT_CHAN_NUM)) {
                /* Wait until all EATT bearers are connected before proceeding */
                return 0;
            }
            /* Set the default bearer to use for further procedures */
            rc = ble_att_set_default_bearer_using_cid(event->eatt.conn_handle, cids[0]);
            if (rc != 0) {
                MODLOG_DFLT(INFO, "Cannot set default EATT bearer, rc = %d\n", rc);
                return rc;
            }

            return 0;
#endif

#if MYNEWT_VAL(BLE_CONN_SUBRATING)
        case BLE_GAP_EVENT_SUBRATE_CHANGE:
            MODLOG_DFLT(INFO, "Subrate change event : conn_handle=%d status=%d factor=%d",
                        event->subrate_change.conn_handle, event->subrate_change.status,
                        event->subrate_change.subrate_factor);
            return 0;
#endif
#endif
    }
    return 0;
}

static void bleprph_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
    stackchan_ble_set_conn_handle(BLE_HS_CONN_HANDLE_NONE);
    bleprph_set_notify_payload_len(20);
}

#if CONFIG_EXAMPLE_RANDOM_ADDR
static int ble_app_set_addr(void)
{
    ble_addr_t addr;
    int rc;

    /* generate new non-resolvable private address */
    rc = ble_hs_id_gen_rnd(0, &addr);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "cannot generate a random BLE address; rc=%d", rc);
        return rc;
    }

    /* set generated address */
    rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "cannot set the random BLE address; rc=%d", rc);
    }
    return rc;
}
#endif

static void bleprph_on_sync(void)
{
    int rc;

#if CONFIG_EXAMPLE_RANDOM_ADDR
    /* Generate a non-resolvable private address. */
    rc = ble_app_set_addr();
    if (rc != 0) {
        return;
    }
#endif

    /* Make sure we have proper identity address set (public preferred) */
#if CONFIG_EXAMPLE_RANDOM_ADDR
    rc = ble_hs_util_ensure_addr(1);
#else
    rc = ble_hs_util_ensure_addr(0);
#endif
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "cannot ensure a BLE identity address; rc=%d", rc);
        return;
    }

    /* Figure out address to use while advertising (no privacy for now) */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error determining address type; rc=%d\n", rc);
        return;
    }

    /* Printing ADDR */
    uint8_t addr_val[6] = {0};
    rc                  = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "cannot copy the BLE identity address; rc=%d", rc);
        return;
    }

    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");
    /* Begin advertising. */
#if CONFIG_EXAMPLE_EXTENDED_ADV
    ext_bleprph_advertise();
#else
    bleprph_advertise();
#endif
}

void bleprph_host_task(void *param)
{
    (void)param;
    ESP_LOGI(tag, "BLE Host Task Started");
    /* This function will return only when nimble_port_stop() is executed */
    nimble_port_run();

    nimble_port_freertos_deinit();
}

int ble_prph_init(bool use_alt_uuid)
{
    bool nimble_initialized = false;
    bool scli_initialized = false;
    bool cleanup_failed = false;
    int rc = 0;
    esp_err_t ret;

    taskENTER_CRITICAL(&s_bleprph_state_lock);
    if (s_bleprph_state == BLEPRPH_STATE_INITIALIZED) {
        const bool same_mode = s_use_alt_uuid == use_alt_uuid;
        taskEXIT_CRITICAL(&s_bleprph_state_lock);
        if (!same_mode) {
            ESP_LOGE(tag, "BLE already uses the %s UUID; refusing an unsafe live switch to %s",
                     s_use_alt_uuid ? "alternate" : "normal", use_alt_uuid ? "alternate" : "normal");
            return ESP_ERR_INVALID_STATE;
        }
        return 0;
    }
    if (s_bleprph_state != BLEPRPH_STATE_UNINITIALIZED) {
        taskEXIT_CRITICAL(&s_bleprph_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_bleprph_state = BLEPRPH_STATE_INITIALIZING;
    s_use_alt_uuid = use_alt_uuid;
    s_notify_payload_len = 20;
    taskEXIT_CRITICAL(&s_bleprph_state_lock);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to initialize NimBLE: %s", esp_err_to_name(ret));
        rc = ret;
        goto init_failed;
    }
    nimble_initialized = true;
    /* Initialize the NimBLE host configuration. */
    ble_hs_cfg.reset_cb          = bleprph_on_reset;
    ble_hs_cfg.sync_cb           = bleprph_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = CONFIG_EXAMPLE_IO_TYPE;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
    /* Enable the appropriate bit masks to make sure the keys
     * that are needed are exchanged
     */
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
#endif
#ifdef CONFIG_EXAMPLE_MITM
    ble_hs_cfg.sm_mitm = 1;
#endif
#ifdef CONFIG_EXAMPLE_USE_SC
    ble_hs_cfg.sm_sc = 1;
#else
    ble_hs_cfg.sm_sc = 0;
#endif
#ifdef CONFIG_EXAMPLE_RESOLVE_PEER_ADDR
    /* Stores the IRK */
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
#endif

#if MYNEWT_VAL(STATIC_PASSKEY) && NIMBLE_BLE_CONNECT
    rc = ble_sm_configure_static_passkey(456789, true);
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to configure the static passkey: %d", rc);
        goto init_failed;
    }
#endif

#if MYNEWT_VAL(BLE_GATTS)
    rc = gatt_svr_init(use_alt_uuid);
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to initialize the GATT server: %d", rc);
        goto init_failed;
    }
#endif

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    /* Set the default device name. */
    rc = ble_svc_gap_device_name_set("StackChan");
    if (rc != 0) {
        ESP_LOGE(tag, "Failed to set the BLE device name: %d", rc);
        goto init_failed;
    }
#endif

    /* XXX Need to have template for store */
    ble_store_config_init();

#if CONFIG_EXAMPLE_IO_TYPE == BLE_HS_IO_DISPLAY_YESNO || CONFIG_EXAMPLE_IO_TYPE == BLE_HS_IO_KEYBOARD_ONLY || \
    CONFIG_EXAMPLE_IO_TYPE == BLE_HS_IO_KEYBOARD_DISPLAY
    /* Start UART input only when the configured security mode can request it. */
    rc = scli_init();
    if (rc != ESP_OK) {
        ESP_LOGE(tag, "Failed to initialize the pairing CLI: %s", esp_err_to_name(rc));
        goto init_failed;
    }
    scli_initialized = true;
#endif

    ret = esp_nimble_enable(bleprph_host_task);
    if (ret != ESP_OK) {
        ESP_LOGE(tag, "Failed to start the NimBLE host task: %s", esp_err_to_name(ret));
        rc = ret;
        goto init_failed;
    }

#if MYNEWT_VAL(BLE_EATT_CHAN_NUM) > 0
    bearers = 0;
    for (int i = 0; i < MYNEWT_VAL(BLE_EATT_CHAN_NUM); i++) {
        cids[i] = 0;
    }
#endif

    taskENTER_CRITICAL(&s_bleprph_state_lock);
    s_bleprph_state = BLEPRPH_STATE_INITIALIZED;
    taskEXIT_CRITICAL(&s_bleprph_state_lock);
    return 0;

init_failed:
    if (scli_initialized) {
        const int cleanup_rc = scli_deinit();
        if (cleanup_rc != ESP_OK) {
            ESP_LOGW(tag, "Pairing CLI cleanup failed: %s", esp_err_to_name(cleanup_rc));
            cleanup_failed = true;
        }
    }
    gatt_svr_cleanup_init_failure();
    if (nimble_initialized) {
        const esp_err_t cleanup_rc = nimble_port_deinit();
        if (cleanup_rc != ESP_OK) {
            ESP_LOGW(tag, "NimBLE cleanup failed: %s", esp_err_to_name(cleanup_rc));
            cleanup_failed = true;
        }
    }

    taskENTER_CRITICAL(&s_bleprph_state_lock);
    s_bleprph_state = cleanup_failed ? BLEPRPH_STATE_FAILED : BLEPRPH_STATE_UNINITIALIZED;
    taskEXIT_CRITICAL(&s_bleprph_state_lock);
    return rc != 0 ? rc : ESP_FAIL;
}

uint16_t stackchan_ble_get_notify_payload_len(void)
{
    uint16_t payload_len;
    taskENTER_CRITICAL(&s_bleprph_state_lock);
    payload_len = s_notify_payload_len;
    taskEXIT_CRITICAL(&s_bleprph_state_lock);
    return payload_len;
}
