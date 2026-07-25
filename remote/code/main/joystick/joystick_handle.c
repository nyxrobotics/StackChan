/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "joystick_handle.h"
#include "freertos/event_groups.h"

static i2c_bus_device_handle_t i2c_device1;  // i2c device handle
static portMUX_TYPE joystick_data_mux = portMUX_INITIALIZER_UNLOCKED;
static EventGroupHandle_t joystick_task_gate;
static TaskHandle_t setup_task_handle;
static TaskHandle_t running_task_handle;
static TaskHandle_t imu_task_handle;

#define JOYSTICK_TASK_GATE_OPEN BIT0

bool joystick_task_gate_init(void)
{
    if (joystick_task_gate != NULL) {
        return true;
    }
    joystick_task_gate = xEventGroupCreate();
    return joystick_task_gate != NULL;
}

void joystick_task_gate_open(void)
{
    if (joystick_task_gate != NULL) {
        xEventGroupSetBits(joystick_task_gate, JOYSTICK_TASK_GATE_OPEN);
    }
}

void joystick_task_gate_deinit(void)
{
    if (joystick_task_gate != NULL) {
        vEventGroupDelete(joystick_task_gate);
        joystick_task_gate = NULL;
    }
}

static bool joystick_task_gate_wait(void)
{
    EventGroupHandle_t gate = joystick_task_gate;
    if (gate == NULL) {
        return false;
    }
    return (xEventGroupWaitBits(gate, JOYSTICK_TASK_GATE_OPEN, pdFALSE, pdFALSE, portMAX_DELAY) &
            JOYSTICK_TASK_GATE_OPEN) != 0;
}

void joystick_data_lock(void)
{
    portENTER_CRITICAL(&joystick_data_mux);
}

void joystick_data_unlock(void)
{
    portEXIT_CRITICAL(&joystick_data_mux);
}

void joystick_notify_mode_change(uint8_t screen_mode)
{
    TaskHandle_t task = NULL;
    joystick_data_lock();
    if (screen_mode == MODE_SETUP) {
        task = setup_task_handle;
    } else if (screen_mode == MODE_RUNNING) {
        task = running_task_handle;
    } else if (screen_mode == MODE_IMU) {
        task = imu_task_handle;
    }
    joystick_data_unlock();

    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

/**
 * @brief Initialize joystick via I2C interface
 * @note This is an internal static function that configures I2C_NUM_0 as master with SDA on GPIO0 and SCL on GPIO26
 * @details
 *      1. Configures I2C master mode with 100kHz clock speed
 *      2. Creates I2C bus handle using I2C_NUM_0
 *      3. Creates a device handle for the joystick at I2C address 0x54
 *      4. Probes one register and disables joystick input if it is unavailable
 * @warning This function assumes the joystick device is at I2C address 0x54
 */
static bool joystick_i2c_init()
{
    i2c_config_t conf;
    {
        conf.mode             = I2C_MODE_MASTER;
        conf.sda_io_num       = 0;
        conf.scl_io_num       = 26;
        conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = 100000;
        conf.clk_flags        = 0;
    };
    i2c_bus_handle_t i2c0_bus = i2c_bus_create(I2C_NUM_0, &conf);
    if (i2c0_bus == NULL) {
        ESP_LOGW("I2C Joystick", "Failed to create I2C bus; joystick disabled");
        return false;
    }

    i2c_device1 = i2c_bus_device_create(i2c0_bus, 0x54, 0);
    if (i2c_device1 == NULL) {
        ESP_LOGW("I2C Joystick", "Failed to create joystick device; joystick disabled");
        i2c_bus_delete(&i2c0_bus);
        return false;
    }

    uint8_t probe = 0;
    if (i2c_bus_read_byte(i2c_device1, 0x00, &probe) != ESP_OK) {
        ESP_LOGW("I2C Joystick", "Joystick not found at 0x54; controls disabled");
        i2c_bus_device_delete(&i2c_device1);
        i2c_device1 = NULL;
        i2c_bus_delete(&i2c0_bus);
        return false;
    }
    return true;
}

/**
 * @brief Read X and Y axis values from the joystick via I2C
 * @param joyX Pointer to store X-axis value (16-bit unsigned integer)
 * @param joyY Pointer to store Y-axis value (16-bit unsigned integer)
 * @return void
 * @details
 *      1. Reads 2 bytes from register address 0x00 (X-axis low/high bytes)
 *      2. Reads 2 bytes from register address 0x02 (Y-axis low/high bytes)
 *      3. Combines high and low bytes for both X and Y axes using bit shifting
 *      4. Stores the combined values in the provided pointers
 * @warning This function assumes the joystick provides 16-bit data in little-endian format
 */
static bool joystick_read_xy(uint16_t *joyX, uint16_t *joyY)
{
    if (i2c_device1 == NULL || joyX == NULL || joyY == NULL) {
        return false;
    }

    uint8_t data[4];
    esp_err_t ret = i2c_bus_read_bytes(i2c_device1, 0x00, 2, data);
    if (ret == ESP_OK) {
        ret = i2c_bus_read_bytes(i2c_device1, 0x02, 2, &data[2]);
    }
    if (ret == ESP_OK) {
        *joyX = (data[1] << 8) | data[0];
        *joyY = (data[3] << 8) | data[2];
        return true;
    }
    return false;
}

/**
 * @brief Public interface to initialize the joystick and return default configuration
 * @return joystick_data_t Structure containing initialized joystick parameters
 * @note This is the main initialization function exposed to users
 * @details
 *      1. Calls internal： device_joystick_init()
 *      2. Initializes all fields of 'joystick_data_t'
 *         - channel: 1 (default communication channel)
 *         - id: 0 (default target ID)
 *         - bat: 0 (battery level, to be updated later)
 *         - joyX, joyY: 0 (initial joystick positions)
 *         - screen_mode: MODE_SETUP (start in setup mode)
 *         - select_mode: CHANNEL_SELECT (default selection mode)
 * @return joystick_data_t
 */
joystick_data_t joystick_init()
{
    joystick_data_t tmp = {0};
    tmp.channel     = 1;
    tmp.id          = 0;
    tmp.bat         = 0;
    tmp.joyX        = X_CENTER;
    tmp.joyY        = Y_CENTER;
    tmp.accel_x     = 0.0f;
    tmp.accel_y     = 0.0f;
    tmp.accel_z     = 0.0f;
    tmp.screen_mode = MODE_SETUP;
    tmp.select_mode = CHANNEL_SELECT;
    tmp.btnB_status = false;
    tmp.joystick_available = joystick_i2c_init();
    return tmp;
}

/**
 * @brief Task to handle joystick setup screen
 * @param pvParam Pointer to joystick data, pointing to joystick_data_t structure
 * @note This function runs an infinite loop that continuously reads joystick XY coordinates
 *       and updates the setup screen when the screen mode is MODE_SETUP
 * @details Reads raw joystick data and then calls update_setup_screen function to update screen display
 *          Each loop iteration has a 50ms delay to ensure interface responsiveness
 */
void handle_setup_screen(void *pvParam)
{
    joystick_data_t *joystick_data = (joystick_data_t *)pvParam;
    if (joystick_data == NULL) {
        vTaskDelete(NULL);
        return;
    }
    if (!joystick_task_gate_wait()) {
        vTaskDelete(NULL);
        return;
    }

    joystick_data_lock();
    setup_task_handle = xTaskGetCurrentTaskHandle();
    joystick_data_unlock();

    while (1) {
        joystick_data_t snapshot;
        joystick_data_lock();
        snapshot = *joystick_data;
        joystick_data_unlock();

        if (snapshot.screen_mode == MODE_SETUP) {
            if (snapshot.joystick_available) {
                if (!joystick_read_xy(&snapshot.joyX, &snapshot.joyY)) {
                    snapshot.joyX = X_CENTER;
                    snapshot.joyY = Y_CENTER;
                }
            }
            update_setup_screen(&snapshot);

            joystick_data_lock();
            if (joystick_data->screen_mode == MODE_SETUP) {
                joystick_data->joyX    = snapshot.joyX;
                joystick_data->joyY    = snapshot.joyY;
                joystick_data->channel = snapshot.channel;
                joystick_data->id      = snapshot.id;
            }
            joystick_data_unlock();
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

/**
 * @brief Task to handle joystick running screen, responsible for reading joystick data and sending ESP-NOW control
 * packets
 * @param pvParam Pointer to joystick data, pointing to joystick_data_t structure
 * @note This function runs an infinite loop that reads joystick input, processes data, and sends control packets in
 * running mode
 * @details
 *      1. Reads raw X/Y values from the joystick via I2C
 *      2. Updates the running screen display with current values
 *      3. Applies deadzone correction to center the joystick values
 *      4. Maps raw values to yaw/pitch angle ranges (-1280 to 1280 for yaw, 0 to 900 for pitch)
 *      5. Only sends data when changes exceed threshold (5 units) to reduce network traffic
 *      6. Constructs and sends ESP-NOW packet containing target ID, yaw, pitch, speed and button status
 *      7. Each loop iteration has a 30ms delay when in running mode
 */
void handle_running_screen(void *pvParam)
{
    joystick_data_t *joystick_data = (joystick_data_t *)pvParam;
    if (joystick_data == NULL) {
        vTaskDelete(NULL);
        return;
    }
    if (!joystick_task_gate_wait()) {
        vTaskDelete(NULL);
        return;
    }

    joystick_data_lock();
    running_task_handle = xTaskGetCurrentTaskHandle();
    joystick_data_unlock();

    // communicate packet
    uint8_t pkt[8]              = {0};
    int16_t yaw_angle           = 0;
    int16_t pitch_angle         = 0;
    int16_t last_yaw            = 0;
    int16_t last_pitch          = 0;
    int16_t speed_val           = 600;
    uint8_t last_id             = 0;
    bool last_btnB_status       = false;
    bool packet_sent_in_session = false;

    while (1) {
        joystick_data_t snapshot;
        joystick_data_lock();
        snapshot = *joystick_data;
        joystick_data_unlock();

        // update screen and send packet when in running mode
        if (snapshot.screen_mode == MODE_RUNNING) {
            if (snapshot.joystick_available) {
                if (!joystick_read_xy(&snapshot.joyX, &snapshot.joyY)) {
                    snapshot.joyX = X_CENTER;
                    snapshot.joyY = Y_CENTER;
                }
            }

            update_running_screen(snapshot.joyX, snapshot.joyY, snapshot.channel, snapshot.id, snapshot.bat);

            // handle data from joystick
            if ((snapshot.joyX < X_CENTER + DEAD_ZONE) && (snapshot.joyX > X_CENTER - DEAD_ZONE)) {
                snapshot.joyX = X_CENTER;
            }
            if ((snapshot.joyY < Y_CENTER + DEAD_ZONE) && (snapshot.joyY > Y_CENTER - DEAD_ZONE)) {
                snapshot.joyY = Y_CENTER;
            }

            yaw_angle   = (int16_t)map(snapshot.joyX, X_MIN, X_MAX, 1280, -1280);
            pitch_angle = (int16_t)map(snapshot.joyY, Y_MIN, Y_MAX, 0, 900);

            bool still_active;
            joystick_data_lock();
            still_active = joystick_data->screen_mode == MODE_RUNNING;
            if (still_active) {
                joystick_data->joyX = snapshot.joyX;
                joystick_data->joyY = snapshot.joyY;
                snapshot.id         = joystick_data->id;
                snapshot.btnB_status = joystick_data->btnB_status;
            }
            joystick_data_unlock();
            if (!still_active) {
                packet_sent_in_session = false;
                continue;
            }

            uint8_t current_id       = (uint8_t)snapshot.id;
            bool current_btnB_status = snapshot.btnB_status;
            bool should_send         = !packet_sent_in_session || abs(yaw_angle - last_yaw) >= 5 ||
                               abs(pitch_angle - last_pitch) >= 5 || current_id != last_id ||
                               current_btnB_status != last_btnB_status;

            if (should_send) {
                pkt[0] = current_id;
                memcpy(&pkt[1], &yaw_angle, sizeof(int16_t));
                memcpy(&pkt[3], &pitch_angle, sizeof(int16_t));
                memcpy(&pkt[5], &speed_val, sizeof(int16_t));
                pkt[7] = current_btnB_status;

#if 0
                ESP_LOGI("handle_running_screen", "Yaw: %d, Pitch: %d, Speed: %d, id: %u, Button: %u",
                         yaw_angle, pitch_angle, speed_val, current_id, current_btnB_status);
#endif

                if (espnow_send_data(pkt, sizeof(pkt)) != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(30));
                    continue;
                }
                last_yaw               = yaw_angle;
                last_pitch             = pitch_angle;
                last_id                = current_id;
                last_btnB_status       = current_btnB_status;
                packet_sent_in_session = true;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        } else {
            packet_sent_in_session = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

/**
 * @brief Task to handle joystick IMU screen functionality, processing IMU sensor data and sending ESP-NOW control
 * packets
 *
 * This function runs an infinite loop that reads IMU sensor data (accelerometer and gyroscope),
 * updates the IMU visualization screen, processes the angle data to control remote devices,
 * and sends ESP-NOW packets with the processed control information.
 * The function maps the IMU pitch and roll angles to yaw and pitch values for remote control,
 * applies filtering to reduce unnecessary transmissions, and sends control packets at regular intervals.
 *
 * @param pvParam Pointer to joystick data structure containing IMU sensor values, battery level,
 *                device ID, communication channel, and other control parameters
 * @details
 *      1. Continuously reads IMU data (acceleration and gyro values) from the joystick_data structure
 *      2. Updates the IMU screen visualization with current sensor values
 *      3. Limits roll values to range [-1.5, 1.5] and pitch values to range [0, 1.5]
 *      4. Maps limited angle values to appropriate yaw/pitch ranges for remote control (-1280 to 1280 for yaw, 900 to 0
 * for pitch)
 *      5. Only sends control packets when changes exceed threshold (10 units) to minimize network traffic
 *      6. Constructs and transmits ESP-NOW packet with device ID, yaw, pitch, speed, and button status
 *      7. Includes a 30ms delay between active iterations and sleeps on a notification while inactive
 */
void handle_imu_screen(void *pvParam)
{
    joystick_data_t *joystick_data = (joystick_data_t *)pvParam;
    if (joystick_data == NULL) {
        vTaskDelete(NULL);
        return;
    }
    if (!joystick_task_gate_wait()) {
        vTaskDelete(NULL);
        return;
    }

    joystick_data_lock();
    imu_task_handle = xTaskGetCurrentTaskHandle();
    joystick_data_unlock();

    // communicate packet
    uint8_t pkt[8]              = {0};
    int16_t yaw_angle           = 0;
    int16_t pitch_angle         = 0;
    int16_t last_yaw            = 0;
    int16_t last_pitch          = 0;
    int16_t speed_val           = 600;
    uint8_t last_id             = 0;
    bool last_btnB_status       = false;
    bool packet_sent_in_session = false;

    while (1) {
        joystick_data_t snapshot;
        joystick_data_lock();
        snapshot = *joystick_data;
        joystick_data_unlock();

        // update screen and send packet when in running mode
        if (snapshot.screen_mode == MODE_IMU) {
            IMU_Angle_t imu_angle =
                update_imu_screen(snapshot.accel_x, snapshot.accel_y, snapshot.accel_z,
                                  snapshot.bat, snapshot.id, snapshot.channel);

            // Limit the roll value to the range of -1.5 to 1.5
            float limited_roll = fmaxf(-1.5f, fminf(1.5f, imu_angle.roll));
            // Limit the pitch value to the range of 0 to 1.5
            float limited_pitch = fmaxf(0.0f, fminf(1.5f, imu_angle.pitch));

            yaw_angle   = (int16_t)map(limited_roll, -1.5, 1.5, -1280, 1280);
            pitch_angle = (int16_t)map(limited_pitch, 0, 1.5, 900, 0);

            bool still_active;
            joystick_data_lock();
            still_active = joystick_data->screen_mode == MODE_IMU;
            if (still_active) {
                snapshot.id          = joystick_data->id;
                snapshot.btnB_status = joystick_data->btnB_status;
            }
            joystick_data_unlock();
            if (!still_active) {
                packet_sent_in_session = false;
                continue;
            }

            uint8_t current_id       = (uint8_t)snapshot.id;
            bool current_btnB_status = snapshot.btnB_status;
            bool should_send         = !packet_sent_in_session || abs(yaw_angle - last_yaw) >= 10 ||
                               abs(pitch_angle - last_pitch) >= 10 || current_id != last_id ||
                               current_btnB_status != last_btnB_status;

            if (should_send) {
                pkt[0] = current_id;
                memcpy(&pkt[1], &yaw_angle, sizeof(int16_t));
                memcpy(&pkt[3], &pitch_angle, sizeof(int16_t));
                memcpy(&pkt[5], &speed_val, sizeof(int16_t));
                pkt[7] = current_btnB_status;
                if (espnow_send_data(pkt, sizeof(pkt)) != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(30));
                    continue;
                }

                last_yaw               = yaw_angle;
                last_pitch             = pitch_angle;
                last_id                = current_id;
                last_btnB_status       = current_btnB_status;
                packet_sent_in_session = true;
            }

#if 0
            ESP_LOGI("handle_imu_screen", "yaw_angle: %.2f, pitch_angle: %.2f, yaw: %d, pitch: %d",
                     imu_angle.roll, imu_angle.pitch, yaw_angle, pitch_angle);
#endif

            vTaskDelay(pdMS_TO_TICKS(30));
        } else {
            packet_sent_in_session = false;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}
