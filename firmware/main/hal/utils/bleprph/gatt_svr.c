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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "bleprph.h"
#include "services/ans/ble_svc_ans.h"
#include "esp_heap_caps.h"

/*** Maximum number of characteristics with the notify flag ***/
#define MAX_NOTIFY 5
#define GATT_DATA_LOCK_TIMEOUT_MS 10

/* Stack-Chan Service */
static const ble_uuid128_t stackchan_svc_uuid     = BLE_UUID128_INIT(STACKCHAN_SVC_UUID_BASE);
static const ble_uuid128_t stackchan_svc_uuid_alt = BLE_UUID128_INIT(STACKCHAN_SVC_UUID_BASE_ALT);

static const ble_uuid128_t stackchan_chr_motion_uuid = BLE_UUID128_INIT(STACKCHAN_CHR_MOTION_UUID);

static const ble_uuid128_t stackchan_chr_avatar_uuid = BLE_UUID128_INIT(STACKCHAN_CHR_AVATAR_UUID);

static const ble_uuid128_t stackchan_chr_config_uuid = BLE_UUID128_INIT(STACKCHAN_CHR_CONFIG_UUID);

static const ble_uuid128_t stackchan_chr_rgb_uuid = BLE_UUID128_INIT(STACKCHAN_CHR_RGB_UUID);

/* Stack-Chan characteristic data buffers */
static char *stackchan_motion_data   = NULL;
static uint16_t stackchan_motion_len = 0;
static uint16_t stackchan_motion_handle;

static char *stackchan_avatar_data   = NULL;
static uint16_t stackchan_avatar_len = 0;
static uint16_t stackchan_avatar_handle;

static char *stackchan_config_data   = NULL;
static uint16_t stackchan_config_len = 0;
static uint16_t stackchan_config_handle;

static char *stackchan_rgb_data   = NULL;
static uint16_t stackchan_rgb_len = 0;
static uint16_t stackchan_rgb_handle;

/* Battery level */
static uint8_t battery_level = 100;
static uint16_t battery_level_handle;

/* Callback storage */
static stackchan_ble_callbacks_t g_stackchan_callbacks = {0};

/* Connection handle for notifications */
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static portMUX_TYPE g_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t g_data_mutex;
static bool gatt_initialized;
static bool gatt_uses_alt_uuid;

static int stackchan_svc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                                void *arg);

static int battery_svc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

static struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /*** Stack-Chan Service ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &stackchan_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){{
                                            /* Motion Characteristic - Read/Write/Notify */
                                            .uuid      = &stackchan_chr_motion_uuid.u,
                                            .access_cb = stackchan_svc_access,
                                            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                                            .val_handle = &stackchan_motion_handle,
                                        },
                                        {
                                            /* Avatar Characteristic - Read/Write/Notify */
                                            .uuid      = &stackchan_chr_avatar_uuid.u,
                                            .access_cb = stackchan_svc_access,
                                            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                                            .val_handle = &stackchan_avatar_handle,
                                        },
                                        {
                                            /* Config Characteristic - Read/Write/Notify */
                                            .uuid      = &stackchan_chr_config_uuid.u,
                                            .access_cb = stackchan_svc_access,
                                            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                                            .val_handle = &stackchan_config_handle,
                                        },
                                        {
                                            /* RGB Characteristic - Read/Write/Notify */
                                            .uuid      = &stackchan_chr_rgb_uuid.u,
                                            .access_cb = stackchan_svc_access,
                                            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                                            .val_handle = &stackchan_rgb_handle,
                                        },
                                        {
                                            0, /* No more characteristics */
                                        }},
    },

    {
        /*** Battery Service (standard 0x180F) ***/
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){{
                                                           /* Battery Level Characteristic (standard 0x2A19) */
                                                           .uuid       = BLE_UUID16_DECLARE(0x2A19),
                                                           .access_cb  = battery_svc_access,
                                                           .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                                                           .val_handle = &battery_level_handle,
                                                       },
                                                       {
                                                           0, /* No more characteristics */
                                                       }},
    },

    {
        0, /* No more services. */
    },
};

static bool gatt_data_lock(void)
{
    return g_data_mutex != NULL &&
           xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(GATT_DATA_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void gatt_data_unlock(void)
{
    xSemaphoreGive(g_data_mutex);
}

static bool gatt_is_initialized(void)
{
    bool initialized;
    taskENTER_CRITICAL(&g_state_lock);
    initialized = gatt_initialized;
    taskEXIT_CRITICAL(&g_state_lock);
    return initialized;
}

static uint16_t stackchan_ble_get_conn_handle(void)
{
    uint16_t conn_handle;
    taskENTER_CRITICAL(&g_state_lock);
    conn_handle = g_conn_handle;
    taskEXIT_CRITICAL(&g_state_lock);
    return conn_handle;
}

static int gatt_svr_write(struct os_mbuf *om, uint16_t min_len, uint16_t max_len, void *dst, uint16_t *len)
{
    uint16_t om_len;
    int rc;

    if (om == NULL || dst == NULL || len == NULL) {
        MODLOG_DFLT(ERROR, "Cannot write an unavailable GATT value");
        return BLE_ATT_ERR_UNLIKELY;
    }

    om_len = OS_MBUF_PKTLEN(om);
    if (om_len < min_len || om_len > max_len) {
        MODLOG_DFLT(ERROR, "Invalid attribute value length: %d (expected %d-%d)", om_len, min_len, max_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rc = ble_hs_mbuf_to_flat(om, dst, max_len, len);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to flatten mbuf: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }

    return 0;
}

static int stackchan_json_access(struct ble_gatt_access_ctxt *ctxt, char *data, uint16_t *data_len,
                                 stackchan_ble_data_callback_t callback, uint16_t conn_handle)
{
    int rc;

    if (ctxt == NULL || ctxt->om == NULL || data == NULL || data_len == NULL || !gatt_is_initialized()) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (!gatt_data_lock()) {
        MODLOG_DFLT(WARN, "Timed out waiting for the GATT data lock");
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        rc = os_mbuf_append(ctxt->om, data, *data_len);
        if (rc != 0) {
            rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        rc = gatt_svr_write(ctxt->om, 0, STACKCHAN_MAX_JSON_LEN, data, data_len);
        if (rc == 0) {
            data[*data_len] = '\0';
            if (callback != NULL) {
                rc = callback(data, *data_len, conn_handle);
            }
        }
    } else {
        rc = BLE_ATT_ERR_UNLIKELY;
    }

    gatt_data_unlock();
    return rc == 0 ? 0 : (rc > 0 && rc <= UINT8_MAX ? rc : BLE_ATT_ERR_UNLIKELY);
}

/**
 * Stack-Chan service access callback
 */
static int stackchan_svc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                                void *arg)
{
    (void)arg;

    if (ctxt == NULL ||
        (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR && ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (attr_handle == stackchan_motion_handle) {
        return stackchan_json_access(ctxt, stackchan_motion_data, &stackchan_motion_len,
                                     g_stackchan_callbacks.motion_cb, conn_handle);
    }
    if (attr_handle == stackchan_avatar_handle) {
        return stackchan_json_access(ctxt, stackchan_avatar_data, &stackchan_avatar_len,
                                     g_stackchan_callbacks.avatar_cb, conn_handle);
    }
    if (attr_handle == stackchan_config_handle) {
        return stackchan_json_access(ctxt, stackchan_config_data, &stackchan_config_len,
                                     g_stackchan_callbacks.config_cb, conn_handle);
    }
    if (attr_handle == stackchan_rgb_handle) {
        return stackchan_json_access(ctxt, stackchan_rgb_data, &stackchan_rgb_len, g_stackchan_callbacks.rgb_cb,
                                     conn_handle);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * Battery service access callback
 */
static int battery_svc_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;
    (void)arg;
    (void)conn_handle;

    if (ctxt == NULL || ctxt->om == NULL || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR ||
        attr_handle != battery_level_handle || !gatt_is_initialized()) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (!gatt_data_lock()) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (g_stackchan_callbacks.battery_read_cb != NULL) {
        battery_level = g_stackchan_callbacks.battery_read_cb();
        if (battery_level > 100) {
            battery_level = 100;
        }
    }
    rc = os_mbuf_append(ctxt->om, &battery_level, sizeof(battery_level));
    gatt_data_unlock();

    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];
    (void)arg;

    if (ctxt == NULL) {
        MODLOG_DFLT(ERROR, "Received a null GATT registration context");
        return;
    }

    switch (ctxt->op) {
        case BLE_GATT_REGISTER_OP_SVC:
            if (ctxt->svc.svc_def == NULL || ctxt->svc.svc_def->uuid == NULL) {
                MODLOG_DFLT(ERROR, "Received an invalid GATT service registration context");
                return;
            }
            MODLOG_DFLT(DEBUG, "registered service %s with handle=%d", ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                        ctxt->svc.handle);
            break;

        case BLE_GATT_REGISTER_OP_CHR:
            if (ctxt->chr.chr_def == NULL || ctxt->chr.chr_def->uuid == NULL) {
                MODLOG_DFLT(ERROR, "Received an invalid GATT characteristic registration context");
                return;
            }
            MODLOG_DFLT(DEBUG,
                        "registering characteristic %s with "
                        "def_handle=%d val_handle=%d",
                        ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf), ctxt->chr.def_handle, ctxt->chr.val_handle);
            break;

        case BLE_GATT_REGISTER_OP_DSC:
            if (ctxt->dsc.dsc_def == NULL || ctxt->dsc.dsc_def->uuid == NULL) {
                MODLOG_DFLT(ERROR, "Received an invalid GATT descriptor registration context");
                return;
            }
            MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d",
                        ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
            break;

        default:
            MODLOG_DFLT(WARN, "Unknown GATT registration operation: %d", ctxt->op);
            break;
    }
}

static char *allocate_json_buffer(size_t buffer_size)
{
    char *buffer = (char *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = (char *)heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

void gatt_svr_cleanup_init_failure(void)
{
    taskENTER_CRITICAL(&g_state_lock);
    gatt_initialized = false;
    gatt_uses_alt_uuid = false;
    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    taskEXIT_CRITICAL(&g_state_lock);

    heap_caps_free(stackchan_motion_data);
    heap_caps_free(stackchan_avatar_data);
    heap_caps_free(stackchan_config_data);
    heap_caps_free(stackchan_rgb_data);
    stackchan_motion_data = NULL;
    stackchan_avatar_data = NULL;
    stackchan_config_data = NULL;
    stackchan_rgb_data = NULL;
    stackchan_motion_len = 0;
    stackchan_avatar_len = 0;
    stackchan_config_len = 0;
    stackchan_rgb_len = 0;
    stackchan_motion_handle = 0;
    stackchan_avatar_handle = 0;
    stackchan_config_handle = 0;
    stackchan_rgb_handle = 0;
    battery_level_handle = 0;
    battery_level = 100;

    if (g_data_mutex != NULL) {
        vSemaphoreDelete(g_data_mutex);
        g_data_mutex = NULL;
    }
}

int gatt_svr_init(bool use_alt_uuid)
{
    int rc;
    bool already_initialized;
    bool current_mode;

    taskENTER_CRITICAL(&g_state_lock);
    already_initialized = gatt_initialized;
    current_mode = gatt_uses_alt_uuid;
    taskEXIT_CRITICAL(&g_state_lock);
    if (already_initialized) {
        return current_mode == use_alt_uuid ? 0 : BLE_HS_EINVAL;
    }

    /* Clear any resources left by an earlier partial initialization. */
    gatt_svr_cleanup_init_failure();

    g_data_mutex = xSemaphoreCreateMutex();
    if (g_data_mutex == NULL) {
        MODLOG_DFLT(ERROR, "Failed to create the Stack-Chan GATT data mutex");
        return BLE_HS_ENOMEM;
    }

    /* Allocate buffers in PSRAM */
    /* The write path accepts STACKCHAN_MAX_JSON_LEN bytes and appends '\0'. */
    const size_t buffer_size = STACKCHAN_MAX_JSON_LEN + 1;
    stackchan_motion_data = allocate_json_buffer(buffer_size);
    stackchan_avatar_data = allocate_json_buffer(buffer_size);
    stackchan_config_data = allocate_json_buffer(buffer_size);
    stackchan_rgb_data    = allocate_json_buffer(buffer_size);

    if (!stackchan_motion_data || !stackchan_avatar_data || !stackchan_config_data || !stackchan_rgb_data) {
        MODLOG_DFLT(ERROR, "Failed to allocate memory for Stack-Chan characteristics\n");
        gatt_svr_cleanup_init_failure();
        return BLE_HS_ENOMEM;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    if (use_alt_uuid) {
        gatt_svr_svcs[0].uuid = &stackchan_svc_uuid_alt.u;
    } else {
        gatt_svr_svcs[0].uuid = &stackchan_svc_uuid.u;
    }

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to count GATT services: %d", rc);
        gatt_svr_cleanup_init_failure();
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to add GATT services: %d", rc);
        gatt_svr_cleanup_init_failure();
        return rc;
    }

    /* Initialize Stack-Chan data with empty JSON */
    strcpy(stackchan_motion_data, "{}");
    stackchan_motion_len = 2;

    strcpy(stackchan_avatar_data, "{}");
    stackchan_avatar_len = 2;

    strcpy(stackchan_config_data, "{}");
    stackchan_config_len = 2;

    strcpy(stackchan_rgb_data, "{}");
    stackchan_rgb_len = 2;

    taskENTER_CRITICAL(&g_state_lock);
    gatt_uses_alt_uuid = use_alt_uuid;
    gatt_initialized = true;
    taskEXIT_CRITICAL(&g_state_lock);

    return 0;
}

/**
 * Public API implementations
 */

void stackchan_ble_register_callbacks(const stackchan_ble_callbacks_t *callbacks)
{
    bool locked = false;

    if (gatt_is_initialized()) {
        locked = gatt_data_lock();
        if (!locked) {
            MODLOG_DFLT(ERROR, "Failed to update Stack-Chan callbacks: GATT data is busy");
            return;
        }
    }

    if (callbacks != NULL) {
        g_stackchan_callbacks = *callbacks;
        MODLOG_DFLT(INFO, "Stack-Chan callbacks registered");
    } else {
        memset(&g_stackchan_callbacks, 0, sizeof(g_stackchan_callbacks));
        MODLOG_DFLT(INFO, "Stack-Chan callbacks cleared");
    }

    if (locked) {
        gatt_data_unlock();
    }
}

static int stackchan_ble_notify_json(const char *json_data, uint16_t len, char *data, uint16_t *data_len,
                                     uint16_t value_handle)
{
    uint16_t conn_handle;

    if (json_data == NULL || len == 0 || len > STACKCHAN_MAX_JSON_LEN) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (!gatt_is_initialized() || data == NULL || data_len == NULL || value_handle == 0) {
        return BLE_HS_EINVAL;
    }
    if (!gatt_data_lock()) {
        return BLE_HS_EBUSY;
    }

    memcpy(data, json_data, len);
    *data_len = len;
    data[len] = '\0';
    gatt_data_unlock();

    conn_handle = stackchan_ble_get_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return 0;
    }

    /* NimBLE schedules notifications internally; this API has no failure
     * return value in ESP-IDF 5.5. */
    ble_gatts_chr_updated(value_handle);
    return 0;
}

int stackchan_ble_notify_motion(const char *json_data, uint16_t len)
{
    return stackchan_ble_notify_json(json_data, len, stackchan_motion_data, &stackchan_motion_len,
                                     stackchan_motion_handle);
}

int stackchan_ble_notify_avatar(const char *json_data, uint16_t len)
{
    return stackchan_ble_notify_json(json_data, len, stackchan_avatar_data, &stackchan_avatar_len,
                                     stackchan_avatar_handle);
}

int stackchan_ble_notify_config(const char *json_data, uint16_t len)
{
    return stackchan_ble_notify_json(json_data, len, stackchan_config_data, &stackchan_config_len,
                                     stackchan_config_handle);
}

int stackchan_ble_notify_rgb(const char *json_data, uint16_t len)
{
    return stackchan_ble_notify_json(json_data, len, stackchan_rgb_data, &stackchan_rgb_len, stackchan_rgb_handle);
}

int stackchan_ble_update_battery_level(uint8_t level)
{
    if (level > 100) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (!gatt_is_initialized() || battery_level_handle == 0) {
        return BLE_HS_EINVAL;
    }
    if (!gatt_data_lock()) {
        return BLE_HS_EBUSY;
    }

    battery_level = level;
    gatt_data_unlock();

    if (stackchan_ble_get_conn_handle() == BLE_HS_CONN_HANDLE_NONE) {
        return 0;
    }

    ble_gatts_chr_updated(battery_level_handle);
    return 0;
}

void stackchan_ble_set_conn_handle(uint16_t conn_handle)
{
    uint16_t previous_handle;
    taskENTER_CRITICAL(&g_state_lock);
    previous_handle = g_conn_handle;
    g_conn_handle = conn_handle;
    taskEXIT_CRITICAL(&g_state_lock);

    if (previous_handle != conn_handle) {
        MODLOG_DFLT(INFO, "Stack-Chan connection handle updated: %d", conn_handle);
    }
}

bool stackchan_ble_is_connected(void)
{
    return gatt_is_initialized() && stackchan_ble_get_conn_handle() != BLE_HS_CONN_HANDLE_NONE;
}
