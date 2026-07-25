#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "esp_imgfx_color_convert.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"

#include "board.h"
#include "display.h"
#include "stackchan_camera.h"
#include "esp_jpeg_common.h"
#include "jpg/image_to_jpeg.h"
#include "jpg/jpeg_to_image.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"

// #include "application.h"
// #include "audio_service.h"
#include <hal/board/hal_bridge.h>
#include "assets/assets.h"

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL MAX(CONFIG_LOG_DEFAULT_LEVEL, ESP_LOG_DEBUG)
#endif                // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#include <esp_log.h>  // should be after LOCAL_LOG_LEVEL definition

#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "driver/ppa.h"
#if defined(CONFIG_XIAOZHI_CAMERA_IMAGE_ROTATION_ANGLE_90)
#define IMAGE_ROTATION_ANGLE (PPA_SRM_ROTATION_ANGLE_270)
#elif defined(CONFIG_XIAOZHI_CAMERA_IMAGE_ROTATION_ANGLE_270)
#define IMAGE_ROTATION_ANGLE (PPA_SRM_ROTATION_ANGLE_90)
#else
#error "CONFIG_XIAOZHI_CAMERA_IMAGE_ROTATION_ANGLE is not set"
#endif  // angle
#else   // target
#include "esp_imgfx_rotate.h"
#if defined(CONFIG_XIAOZHI_CAMERA_IMAGE_ROTATION_ANGLE_90)
#define IMAGE_ROTATION_ANGLE (90)
#elif defined(CONFIG_XIAOZHI_CAMERA_IMAGE_ROTATION_ANGLE_270)
#define IMAGE_ROTATION_ANGLE (270)
#else
#error "CONFIG_XIAOZHI_CAMERA_IMAGE_ROTATION_ANGLE is not set"
#endif  // angle
#endif  // target
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE

#define TAG "StackChanCamera"

namespace {

#ifdef CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
constexpr uint32_t kCameraWarmupMs = 5000;
#endif
constexpr uint32_t kInitTaskStopWaitMs        = 1500;
constexpr uint32_t kInitTaskCompletionGraceMs = 250;
constexpr uint32_t kJpegQueueSendTimeoutMs    = 1000;
constexpr uint32_t kJpegQueueReceiveTimeoutMs = 15000;
constexpr int kExplainHttpTimeoutMs           = 30000;

}  // namespace

#if defined(CONFIG_CAMERA_SENSOR_SWAP_PIXEL_BYTE_ORDER) || defined(CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP)
#warning \
    "CAMERA_SENSOR_SWAP_PIXEL_BYTE_ORDER or CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP is enabled, which may cause image corruption in YUV422 format!"
#endif

#if CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#define CAM_PRINT_FOURCC(pixelformat)       \
    char fourcc[5];                         \
    fourcc[0] = pixelformat & 0xFF;         \
    fourcc[1] = (pixelformat >> 8) & 0xFF;  \
    fourcc[2] = (pixelformat >> 16) & 0xFF; \
    fourcc[3] = (pixelformat >> 24) & 0xFF; \
    fourcc[4] = '\0';                       \
    ESP_LOGD(TAG, "FOURCC: '%c%c%c%c'", fourcc[0], fourcc[1], fourcc[2], fourcc[3]);

// for compatibility with old esp_video version
#ifndef MAP_FAILED
#define MAP_FAILED nullptr
#endif

__attribute__((weak)) esp_err_t esp_video_deinit(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
// end of for compatibility with old esp_video version

static void log_available_video_devices()
{
    for (int i = 0; i < 50; i++) {
        char path[16];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ESP_LOGD(TAG, "found video device: %s", path);
            close(fd);
        }
    }
}
#else
#define CAM_PRINT_FOURCC(pixelformat) (void)0;
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE

void StackChanCamera::StopInitTask()
{
    shutting_down_.store(true, std::memory_order_release);
    streaming_on_.store(false, std::memory_order_release);

    // DQBUF may be sleeping in the video driver. Stop the stream first so the
    // driver can release the waiter and let the task leave cooperatively.
    // Deleting a task from inside ioctl risks leaving driver locks behind.
    if (stream_started_ && video_fd_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(video_fd_, VIDIOC_STREAMOFF, &type) == 0) {
            stream_started_ = false;
        } else {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed while stopping init task, errno=%d(%s)", errno, strerror(errno));
        }
    }

    SemaphoreHandle_t done = init_task_done_;
    if (done == nullptr) {
        return;
    }

    bool completed = xSemaphoreTake(done, pdMS_TO_TICKS(kInitTaskStopWaitMs)) == pdTRUE;
    TaskHandle_t task_to_delete = nullptr;
    bool completion_in_progress = false;

    if (!completed) {
        portENTER_CRITICAL(&init_task_state_lock_);
        if (init_task_state_ == InitTaskState::kRunning) {
            // The task will observe kStopping before it can self-delete, so this handle
            // cannot become stale while vTaskDelete() is called below.
            init_task_state_ = InitTaskState::kStopping;
            task_to_delete   = init_task_handle_;
        } else if (init_task_state_ == InitTaskState::kCompleted) {
            // The task no longer accesses this object, but may not have given the
            // completion semaphore yet.
            completion_in_progress = true;
        }
        portEXIT_CRITICAL(&init_task_state_lock_);

        if (task_to_delete != nullptr) {
            ESP_LOGE(TAG, "Camera init task remained blocked after STREAMOFF; forcing final task cleanup");
            vTaskDelete(task_to_delete);
            completed = true;
        } else if (completion_in_progress) {
            completed = xSemaphoreTake(done, pdMS_TO_TICKS(kInitTaskCompletionGraceMs)) == pdTRUE;
        } else {
            // No running task owns the semaphore.
            completed = true;
        }
    }

    portENTER_CRITICAL(&init_task_state_lock_);
    init_task_handle_ = nullptr;
    init_task_state_  = InitTaskState::kIdle;
    portEXIT_CRITICAL(&init_task_state_lock_);
    init_task_done_ = nullptr;

    if (completed) {
        vSemaphoreDelete(done);
    } else {
        // A task in kCompleted only retains this local semaphore handle and never
        // accesses StackChanCamera again. Keep the semaphore alive rather than race
        // its final xSemaphoreGive(); this exceptional path leaks only the semaphore.
        ESP_LOGE(TAG, "Camera init completion signal was delayed; preserving its semaphore");
    }
}

void StackChanCamera::CleanupVideoResources()
{
    streaming_on_.store(false, std::memory_order_release);

    if (stream_started_ && video_fd_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(video_fd_, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed during cleanup, errno=%d(%s)", errno, strerror(errno));
        }
    }
    stream_started_ = false;

    for (auto& buffer : mmap_buffers_) {
        if (buffer.start != nullptr && buffer.length != 0) {
            munmap(buffer.start, buffer.length);
            buffer.start  = nullptr;
            buffer.length = 0;
        }
    }
    mmap_buffers_.clear();

    if (video_fd_ >= 0) {
        close(video_fd_);
        video_fd_ = -1;
    }

    if (frame_.data != nullptr) {
        heap_caps_free(frame_.data);
        frame_.data = nullptr;
    }
    frame_.len     = 0;
    frame_.width   = 0;
    frame_.height  = 0;
    frame_.format  = 0;
    sensor_format_ = 0;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    sensor_width_  = 0;
    sensor_height_ = 0;
#endif

    if (video_initialized_) {
        esp_err_t ret = esp_video_deinit();
        if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "esp_video_deinit failed: %d (%s)", static_cast<int>(ret), esp_err_to_name(ret));
        }
        video_initialized_ = false;
    }
}

StackChanCamera::StackChanCamera(const esp_video_init_config_t& config)
{
    if (esp_video_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed");
        return;
    }
    video_initialized_ = true;

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE

    const char* video_device_name = nullptr;

    if (false) { /* 用于构建 else if */
    }
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
    else if (config.csi != nullptr) {
        video_device_name = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
    }
#endif
#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
    else if (config.dvp != nullptr) {
        video_device_name = ESP_VIDEO_DVP_DEVICE_NAME;
    }
#endif
#if CONFIG_ESP_VIDEO_ENABLE_HW_JPEG_VIDEO_DEVICE
    else if (config.jpeg != nullptr) {
        video_device_name = ESP_VIDEO_JPEG_DEVICE_NAME;
    }
#endif
#if CONFIG_ESP_VIDEO_ENABLE_SPI_VIDEO_DEVICE
    else if (config.spi != nullptr) {
        video_device_name = ESP_VIDEO_SPI_DEVICE_NAME;
    }
#endif
#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
    else if (config.usb_uvc != nullptr) {
        video_device_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(0);
    }
#endif

    if (video_device_name == nullptr) {
        ESP_LOGE(TAG, "no video device is enabled");
        CleanupVideoResources();
        return;
    }

    video_fd_ = open(video_device_name, O_RDWR);

    if (video_fd_ < 0) {
        ESP_LOGE(TAG, "open %s failed, errno=%d(%s)", video_device_name, errno, strerror(errno));
#if CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
        log_available_video_devices();
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
        CleanupVideoResources();
        return;
    }

    struct v4l2_capability cap = {};
    if (ioctl(video_fd_, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed, errno=%d(%s)", errno, strerror(errno));
        CleanupVideoResources();
        return;
    }

    ESP_LOGD(
        TAG,
        "VIDIOC_QUERYCAP: driver=%s, card=%s, bus_info=%s, version=0x%08lx, capabilities=0x%08lx, device_caps=0x%08lx",
        cap.driver, cap.card, cap.bus_info, cap.version, cap.capabilities, cap.device_caps);

    struct v4l2_format format = {};
    format.type               = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd_, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed, errno=%d(%s)", errno, strerror(errno));
        CleanupVideoResources();
        return;
    }
    ESP_LOGD(TAG, "VIDIOC_G_FMT: pixelformat=0x%08lx, width=%ld, height=%ld", format.fmt.pix.pixelformat,
             format.fmt.pix.width, format.fmt.pix.height);
    CAM_PRINT_FOURCC(format.fmt.pix.pixelformat);

    struct v4l2_format setformat = {};
    setformat.type               = V4L2_BUF_TYPE_VIDEO_CAPTURE;
#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    sensor_width_  = format.fmt.pix.width;
    sensor_height_ = format.fmt.pix.height;
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    setformat.fmt.pix.width  = format.fmt.pix.width;
    setformat.fmt.pix.height = format.fmt.pix.height;

    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index               = 0;
    uint32_t best_fmt           = 0;
    int best_rank               = 1 << 30;  // large number

    // 注: 当前版本 esp_video 中 YUV422P 实际输出为 YUYV。
#if defined(CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE) && defined(CONFIG_SOC_PPA_SUPPORTED)
    auto get_rank = [](uint32_t fmt) -> int {
        switch (fmt) {
            case V4L2_PIX_FMT_RGB24:
                return 0;
            case V4L2_PIX_FMT_RGB565:
                return 1;
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
            case V4L2_PIX_FMT_YUV420:  // 软件 JPEG 编码器不支持 YUV420 格式
                return 2;
#endif  // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
            case V4L2_PIX_FMT_GREY:
            case V4L2_PIX_FMT_YUV422P:
            default:
                return 1 << 29;  // unsupported
        }
    };
#else
    auto get_rank = [](uint32_t fmt) -> int {
        switch (fmt) {
            case V4L2_PIX_FMT_YUV422P:
                return 10;
            case V4L2_PIX_FMT_RGB565:
                return 11;
            case V4L2_PIX_FMT_RGB24:
                return 12;
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
            case V4L2_PIX_FMT_YUV420:
                return 13;
#endif  // CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_ENCODER
#ifdef CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
            case V4L2_PIX_FMT_JPEG:
                return 5;
#endif  // CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
            case V4L2_PIX_FMT_GREY:
                return 20;
            default:
                return 1 << 29;  // unsupported
        }
    };
#endif
    while (ioctl(video_fd_, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        ESP_LOGD(TAG, "VIDIOC_ENUM_FMT: pixelformat=0x%08lx, description=%s", fmtdesc.pixelformat, fmtdesc.description);
        CAM_PRINT_FOURCC(fmtdesc.pixelformat);
        int rank = get_rank(fmtdesc.pixelformat);
        if (rank < best_rank) {
            best_rank = rank;
            best_fmt  = fmtdesc.pixelformat;
        }
        fmtdesc.index++;
    }
    if (best_rank < (1 << 29)) {
        setformat.fmt.pix.pixelformat = best_fmt;
        sensor_format_                = best_fmt;
    }

    if (!setformat.fmt.pix.pixelformat) {
        ESP_LOGE(TAG, "no supported pixel format found");
        CleanupVideoResources();
        return;
    }

    ESP_LOGD(TAG, "selected pixel format: 0x%08lx", setformat.fmt.pix.pixelformat);

    if (ioctl(video_fd_, VIDIOC_S_FMT, &setformat) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed, errno=%d(%s)", errno, strerror(errno));
        CleanupVideoResources();
        return;
    }

#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
    frame_.width  = setformat.fmt.pix.height;
    frame_.height = setformat.fmt.pix.width;
#else
    frame_.width  = setformat.fmt.pix.width;
    frame_.height = setformat.fmt.pix.height;
#endif

    // 申请缓冲并mmap
    struct v4l2_requestbuffers req = {};
    req.count                      = strcmp(video_device_name, ESP_VIDEO_MIPI_CSI_DEVICE_NAME) == 0 ? 2 : 1;
    req.type                       = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory                     = V4L2_MEMORY_MMAP;
    if (ioctl(video_fd_, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        CleanupVideoResources();
        return;
    }
    mmap_buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {};
        buf.type               = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory             = V4L2_MEMORY_MMAP;
        buf.index              = i;
        if (ioctl(video_fd_, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed");
            CleanupVideoResources();
            return;
        }
        void* start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, video_fd_, buf.m.offset);
        if (start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap failed");
            CleanupVideoResources();
            return;
        }
        mmap_buffers_[i].start  = start;
        mmap_buffers_[i].length = buf.length;

        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
            CleanupVideoResources();
            return;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd_, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        CleanupVideoResources();
        return;
    }
    stream_started_ = true;

#ifdef CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
    // 当启用 ISP 时，ISP 需要一些照片来初始化参数，因此开启后后台拍摄5s照片并丢弃
    init_task_done_ = xSemaphoreCreateBinary();
    if (init_task_done_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create camera init completion semaphore");
        CleanupVideoResources();
        return;
    }

    portENTER_CRITICAL(&init_task_state_lock_);
    init_task_state_ = InitTaskState::kRunning;
    portEXIT_CRITICAL(&init_task_state_lock_);

    BaseType_t task_created = xTaskCreate(
        [](void* arg) {
            StackChanCamera* self  = static_cast<StackChanCamera*>(arg);
            uint16_t capture_count = 0;
            TickType_t start       = xTaskGetTickCount();
            TickType_t duration    = pdMS_TO_TICKS(kCameraWarmupMs);
            while (!self->shutting_down_.load(std::memory_order_acquire) &&
                   (xTaskGetTickCount() - start) < duration) {
                struct v4l2_buffer buf = {};
                buf.type               = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory             = V4L2_MEMORY_MMAP;
                if (ioctl(self->video_fd_, VIDIOC_DQBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "VIDIOC_DQBUF failed during init");
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                if (ioctl(self->video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "VIDIOC_QBUF failed during init");
                }
                capture_count++;
            }

            SemaphoreHandle_t done = self->init_task_done_;
            bool complete_normally = false;
            portENTER_CRITICAL(&self->init_task_state_lock_);
            if (self->init_task_state_ == InitTaskState::kRunning) {
                if (!self->shutting_down_.load(std::memory_order_acquire)) {
                    self->streaming_on_.store(true, std::memory_order_release);
                }
                self->init_task_state_  = InitTaskState::kCompleted;
                self->init_task_handle_ = nullptr;
                complete_normally       = true;
            }
            portEXIT_CRITICAL(&self->init_task_state_lock_);

            if (complete_normally) {
                ESP_LOGI(TAG, "Camera init success, captured %d frames in %dms", capture_count,
                         (xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
                // All accesses through self are complete before this signal.
                xSemaphoreGive(done);
                vTaskDelete(nullptr);
            }

            // StopInitTask() owns and will delete this task. Do not access self
            // after observing kStopping.
            vTaskSuspend(nullptr);
        },
        "CameraInitTask", 4096, this, 5, &init_task_handle_);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CameraInitTask");
        portENTER_CRITICAL(&init_task_state_lock_);
        init_task_handle_ = nullptr;
        init_task_state_  = InitTaskState::kIdle;
        portEXIT_CRITICAL(&init_task_state_lock_);
        vSemaphoreDelete(init_task_done_);
        init_task_done_ = nullptr;
        CleanupVideoResources();
        return;
    }
#else
    ESP_LOGI(TAG, "Camera init success");
    streaming_on_.store(true, std::memory_order_release);
#endif  // CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
}

StackChanCamera::~StackChanCamera()
{
    StopInitTask();

    {
        std::lock_guard<std::mutex> lock(encoder_mutex_);
        if (encoder_thread_.joinable()) {
            encoder_thread_.join();
        }
    }

    CleanupVideoResources();
}

void StackChanCamera::SetExplainUrl(const std::string& url, const std::string& token)
{
    explain_url_   = url;
    explain_token_ = token;
}

bool StackChanCamera::Capture()
{
    if (!streaming_on_.load(std::memory_order_acquire) || video_fd_ < 0) {
        return false;
    }

    // Play shutter sfx
    hal_bridge::app_play_sound(OGG_CAMERA_SHUTTER);

    std::unique_lock<std::mutex> frame_lock;
    for (int i = 0; i < 3; i++) {
        struct v4l2_buffer buf = {};
        buf.type               = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory             = V4L2_MEMORY_MMAP;
        if (ioctl(video_fd_, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed");
            return false;
        }
        if (i == 2) {
            // DQBUF can block in the driver, so only lock once a frame is ready
            // and immediately before replacing memory used by the encoder.
            frame_lock = std::unique_lock<std::mutex>(encoder_mutex_);
            if (encoder_thread_.joinable()) {
                encoder_thread_.join();
            }

            // 保存帧副本到PSRAM
            if (frame_.data) {
                heap_caps_free(frame_.data);
                frame_.data   = nullptr;
                frame_.format = 0;
            }
            frame_.len  = buf.bytesused;
            frame_.data = (uint8_t*)heap_caps_malloc(frame_.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!frame_.data) {
                ESP_LOGE(TAG, "alloc frame copy failed: need allocate %lu bytes", buf.bytesused);
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }

#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
            ESP_LOGW(TAG, "mmap_buffers_[buf.index].length = %d, sensor_width = %d, sensor_height = %d",
                     mmap_buffers_[buf.index].length, sensor_width_, sensor_height_);
#else
            ESP_LOGW(TAG, "mmap_buffers_[buf.index].length = %d, frame.width = %d, frame.height = %d",
                     mmap_buffers_[buf.index].length, frame_.width, frame_.height);
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
            ESP_LOG_BUFFER_HEXDUMP(TAG, mmap_buffers_[buf.index].start, MIN(mmap_buffers_[buf.index].length, 256),
                                   ESP_LOG_DEBUG);

            switch (sensor_format_) {
                case V4L2_PIX_FMT_RGB565:
                case V4L2_PIX_FMT_RGB24:
                case V4L2_PIX_FMT_YUYV:
                case V4L2_PIX_FMT_YUV420:
                case V4L2_PIX_FMT_GREY:
#ifdef CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
                case V4L2_PIX_FMT_JPEG:
#endif  // CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                {
                    auto src16   = (uint16_t*)mmap_buffers_[buf.index].start;
                    auto dst16   = (uint16_t*)frame_.data;
                    size_t count = (size_t)mmap_buffers_[buf.index].length / 2;
                    for (size_t i = 0; i < count; i++) {
                        dst16[i] = __builtin_bswap16(src16[i]);
                    }
                }
#else
                    memcpy(frame_.data, mmap_buffers_[buf.index].start,
                           MIN(mmap_buffers_[buf.index].length, frame_.len));
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                    frame_.format = sensor_format_;
                    break;
                case V4L2_PIX_FMT_YUV422P: {
                    // 这个格式是 422 YUYV，不是 planer
                    frame_.format = V4L2_PIX_FMT_YUYV;
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                    {
                        auto src16   = (uint16_t*)mmap_buffers_[buf.index].start;
                        auto dst16   = (uint16_t*)frame_.data;
                        size_t count = (size_t)mmap_buffers_[buf.index].length / 2;
                        for (size_t i = 0; i < count; i++) {
                            dst16[i] = __builtin_bswap16(src16[i]);
                        }
                    }
#else
                    memcpy(frame_.data, mmap_buffers_[buf.index].start,
                           MIN(mmap_buffers_[buf.index].length, frame_.len));
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                    break;
                }
                case V4L2_PIX_FMT_RGB565X: {
                    // 大端序的 RGB565 需要转换为小端序
                    // 目前 esp_video 的大小端都会返回格式为 RGB565，不会返回格式为 RGB565X，此 case 用于未来版本兼容
                    auto src16         = (uint16_t*)mmap_buffers_[buf.index].start;
                    auto dst16         = (uint16_t*)frame_.data;
                    size_t pixel_count = (size_t)frame_.width * (size_t)frame_.height;
                    for (size_t i = 0; i < pixel_count; i++) {
                        dst16[i] = __builtin_bswap16(src16[i]);
                    }
                    frame_.format = V4L2_PIX_FMT_RGB565;
                    break;
                }
                default:
                    ESP_LOGE(TAG, "unsupported sensor format: 0x%08lx", sensor_format_);
                    if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                        ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                    }
                    return false;
            }

#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
#ifndef CONFIG_SOC_PPA_SUPPORTED
            uint8_t* rotate_dst =
                (uint8_t*)heap_caps_aligned_alloc(64, frame_.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (rotate_dst == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for rotate image");
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }
            uint8_t* rotate_src = (uint8_t*)frame_.data;

            esp_imgfx_rotate_cfg_t rotate_cfg = {
                .in_res =
                    {
                        .width  = static_cast<int16_t>(sensor_width_),
                        .height = static_cast<int16_t>(sensor_height_),
                    },
                .degree = IMAGE_ROTATION_ANGLE,
            };
            switch (frame_.format) {
                case V4L2_PIX_FMT_RGB565:
                    rotate_cfg.in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_LE;
                    break;
                case V4L2_PIX_FMT_YUYV:
                    rotate_cfg.in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB565_LE;
                    break;
                case V4L2_PIX_FMT_GREY:
                    rotate_cfg.in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_Y;
                    break;
                case V4L2_PIX_FMT_RGB24:
                    rotate_cfg.in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB888;
                    break;
                default:
                    ESP_LOGE(TAG, "unsupported sensor format: 0x%08lx", sensor_format_);
                    if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                        ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                    }
                    return false;
            }
            esp_imgfx_rotate_handle_t rotate_handle = nullptr;
            esp_imgfx_err_t imgfx_err               = esp_imgfx_rotate_open(&rotate_cfg, &rotate_handle);
            if (imgfx_err != ESP_IMGFX_ERR_OK || rotate_handle == nullptr) {
                ESP_LOGE(TAG, "esp_imgfx_rotate_create failed");
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }

            esp_imgfx_data_t rotate_input_data = {
                .data     = rotate_src,
                .data_len = frame_.len,
            };
            esp_imgfx_data_t rotate_output_data = {
                .data     = rotate_dst,
                .data_len = frame_.len,
            };

            imgfx_err = esp_imgfx_rotate_process(rotate_handle, &rotate_input_data, &rotate_output_data);
            if (imgfx_err != ESP_IMGFX_ERR_OK) {
                ESP_LOGE(TAG, "esp_imgfx_rotate_process failed");
                heap_caps_free(rotate_dst);
                rotate_dst = nullptr;
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                esp_imgfx_rotate_close(rotate_handle);
                rotate_handle = nullptr;
                return false;
            }

            frame_.data = rotate_dst;

            heap_caps_free(rotate_src);
            rotate_src = nullptr;

            esp_imgfx_rotate_close(rotate_handle);
            rotate_handle = nullptr;
#else   // CONFIG_SOC_PPA_SUPPORTED
            uint8_t* rotate_src = nullptr;

            ppa_srm_color_mode_t ppa_color_mode;
            switch (frame_.format) {
                case V4L2_PIX_FMT_RGB565:
                    rotate_src     = (uint8_t*)frame_.data;
                    ppa_color_mode = PPA_SRM_COLOR_MODE_RGB565;
                    break;
                case V4L2_PIX_FMT_RGB24:
                    rotate_src     = (uint8_t*)frame_.data;
                    ppa_color_mode = PPA_SRM_COLOR_MODE_RGB888;
                    break;
                case V4L2_PIX_FMT_YUYV: {
                    ESP_LOGW(TAG, "YUYV format is not supported for PPA rotation, using software conversion to RGB888");
                    rotate_src = (uint8_t*)heap_caps_malloc(frame_.width * frame_.height * 3,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (rotate_src == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate memory for rotate image");
                        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                            ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                        }
                        return false;
                    }
                    esp_imgfx_color_convert_cfg_t convert_cfg = {
                        .in_res        = {.width  = static_cast<int16_t>(frame_.width),
                                          .height = static_cast<int16_t>(frame_.height)},
                        .in_pixel_fmt  = ESP_IMGFX_PIXEL_FMT_YUYV,
                        .out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_RGB888,
                    };
                    esp_imgfx_color_convert_handle_t convert_handle = nullptr;
                    esp_imgfx_err_t err = esp_imgfx_color_convert_open(&convert_cfg, &convert_handle);
                    if (err != ESP_IMGFX_ERR_OK || convert_handle == nullptr) {
                        ESP_LOGE(TAG, "esp_imgfx_color_convert_open failed");
                        heap_caps_free(rotate_src);
                        rotate_src = nullptr;
                        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                            ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                        }
                        return false;
                    }
                    esp_imgfx_data_t convert_input_data = {
                        .data     = frame_.data,
                        .data_len = frame_.len,
                    };
                    esp_imgfx_data_t convert_output_data = {
                        .data     = rotate_src,
                        .data_len = static_cast<uint32_t>(frame_.width * frame_.height * 3),
                    };
                    err = esp_imgfx_color_convert_process(convert_handle, &convert_input_data, &convert_output_data);
                    if (err != ESP_IMGFX_ERR_OK) {
                        ESP_LOGE(TAG, "esp_imgfx_color_convert_process failed");
                        heap_caps_free(rotate_src);
                        rotate_src = nullptr;
                        esp_imgfx_color_convert_close(convert_handle);
                        convert_handle = nullptr;
                        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                            ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                        }
                        return false;
                    }
                    esp_imgfx_color_convert_close(convert_handle);
                    convert_handle = nullptr;
                    ppa_color_mode = PPA_SRM_COLOR_MODE_RGB888;
                    heap_caps_free(frame_.data);
                    frame_.data = rotate_src;
                    frame_.len  = frame_.width * frame_.height * 3;
                    break;
                }
                default:
                    ESP_LOGE(TAG, "unsupported sensor format for PPA rotation: 0x%08lx", sensor_format_);
                    if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                        ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                    }
                    return false;
            }

            uint8_t* rotate_dst = (uint8_t*)heap_caps_malloc(
                frame_.width * frame_.height * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
            if (rotate_dst == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for rotate image");
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }

            ppa_client_handle_t ppa_client = nullptr;
            ppa_client_config_t client_cfg = {
                .oper_type             = PPA_OPERATION_SRM,
                .max_pending_trans_num = 1,
            };
            esp_err_t err = ppa_register_client(&client_cfg, &ppa_client);
            if (err != ESP_OK || ppa_client == nullptr) {
                ESP_LOGE(TAG, "ppa_register_client failed: %d", (int)err);
                heap_caps_free(rotate_dst);
                rotate_dst = nullptr;
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }

            ppa_srm_rotation_angle_t ppa_angle = IMAGE_ROTATION_ANGLE;

            ppa_srm_oper_config_t srm_cfg = {};
            srm_cfg.in.buffer             = (void*)rotate_src;
            srm_cfg.in.pic_w              = sensor_width_;
            srm_cfg.in.pic_h              = sensor_height_;
            srm_cfg.in.block_w            = sensor_width_;
            srm_cfg.in.block_h            = sensor_height_;
            srm_cfg.in.block_offset_x     = 0;
            srm_cfg.in.block_offset_y     = 0;
            srm_cfg.in.srm_cm             = ppa_color_mode;

            srm_cfg.out.buffer         = (void*)rotate_dst;
            srm_cfg.out.buffer_size    = frame_.len;
            srm_cfg.out.pic_w          = frame_.width;
            srm_cfg.out.pic_h          = frame_.height;
            srm_cfg.out.block_offset_x = 0;
            srm_cfg.out.block_offset_y = 0;
            srm_cfg.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

            // 等比例缩放 1.0
            srm_cfg.scale_x        = 1.0f;
            srm_cfg.scale_y        = 1.0f;
            srm_cfg.rotation_angle = ppa_angle;
            srm_cfg.mode           = PPA_TRANS_MODE_BLOCKING;
            srm_cfg.user_data      = nullptr;

            err = ppa_do_scale_rotate_mirror(ppa_client, &srm_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ppa_do_scale_rotate_mirror failed: %d", (int)err);
                heap_caps_free(rotate_dst);
                rotate_dst = nullptr;
                (void)ppa_unregister_client(ppa_client);
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }

            (void)ppa_unregister_client(ppa_client);

            frame_.data   = rotate_dst;
            frame_.len    = frame_.width * frame_.height * 2;
            frame_.format = V4L2_PIX_FMT_RGB565;
            heap_caps_free(rotate_src);
            rotate_src = nullptr;
#endif  // CONFIG_SOC_PPA_SUPPORTED
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
        }

        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
        }
    }

    // 显示预览图片
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display != nullptr) {
        if (!frame_.data) {
            ESP_LOGE(TAG, "frame.data is null");
            return false;
        }
        uint16_t w                     = frame_.width;
        uint16_t h                     = frame_.height;
        size_t lvgl_image_size         = frame_.len;
        size_t stride                  = ((w * 2) + 3) & ~3;  // 4字节对齐
        lv_color_format_t color_format = LV_COLOR_FORMAT_RGB565;
        uint8_t* data                  = nullptr;

        switch (frame_.format) {
            // LVGL 显示 YUV 系的图像似乎都有问题，暂时转换为 RGB565 显示
            case V4L2_PIX_FMT_YUYV:
            case V4L2_PIX_FMT_YUV420:
            case V4L2_PIX_FMT_RGB24: {
                color_format = LV_COLOR_FORMAT_RGB565;
                data         = (uint8_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    ESP_LOGE(TAG, "Failed to allocate memory for preview image");
                    return false;
                }
                esp_imgfx_color_convert_cfg_t convert_cfg = {
                    .in_res          = {.width  = static_cast<int16_t>(frame_.width),
                                        .height = static_cast<int16_t>(frame_.height)},
                    .in_pixel_fmt    = static_cast<esp_imgfx_pixel_fmt_t>(frame_.format),
                    .out_pixel_fmt   = ESP_IMGFX_PIXEL_FMT_RGB565_LE,
                    .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
                };
                esp_imgfx_color_convert_handle_t convert_handle = nullptr;
                esp_imgfx_err_t err = esp_imgfx_color_convert_open(&convert_cfg, &convert_handle);
                if (err != ESP_IMGFX_ERR_OK || convert_handle == nullptr) {
                    ESP_LOGE(TAG, "esp_imgfx_color_convert_open failed");
                    heap_caps_free(data);
                    data = nullptr;
                    return false;
                }
                esp_imgfx_data_t convert_input_data = {
                    .data     = frame_.data,
                    .data_len = frame_.len,
                };
                esp_imgfx_data_t convert_output_data = {
                    .data     = data,
                    .data_len = static_cast<uint32_t>(w * h * 2),
                };
                err = esp_imgfx_color_convert_process(convert_handle, &convert_input_data, &convert_output_data);
                if (err != ESP_IMGFX_ERR_OK) {
                    ESP_LOGE(TAG, "esp_imgfx_color_convert_process failed");
                    heap_caps_free(data);
                    data = nullptr;
                    esp_imgfx_color_convert_close(convert_handle);
                    convert_handle = nullptr;
                    return false;
                }
                esp_imgfx_color_convert_close(convert_handle);
                convert_handle  = nullptr;
                lvgl_image_size = w * h * 2;
                break;
            }

            case V4L2_PIX_FMT_RGB565:
                data = (uint8_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    ESP_LOGE(TAG, "Failed to allocate memory for preview image");
                    return false;
                }
                memcpy(data, frame_.data, frame_.len);
                lvgl_image_size = frame_.len;  // fallthrough 时兼顾 YUYV 与 RGB565
                break;

#ifdef CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
            case V4L2_PIX_FMT_JPEG: {
                uint8_t* out_data = nullptr;  // out data is allocated by jpeg_to_image
                size_t out_len    = 0;
                size_t out_width  = 0;
                size_t out_height = 0;
                size_t out_stride = 0;

                esp_err_t ret =
                    jpeg_to_image(frame_.data, frame_.len, &out_data, &out_len, &out_width, &out_height, &out_stride);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to decode JPEG image: %d (%s)", (int)ret, esp_err_to_name(ret));
                    if (out_data) {
                        heap_caps_free(out_data);
                        out_data = nullptr;
                    }
                    return false;
                }

                data            = out_data;
                w               = out_width;
                h               = out_height;
                lvgl_image_size = out_len;
                stride          = out_stride;
                break;
            }
#endif
            default:
                ESP_LOGE(TAG, "unsupported frame format: 0x%08lx", frame_.format);
                return false;
        }

        auto image = std::make_unique<LvglAllocatedImage>(data, lvgl_image_size, w, h, stride, color_format);
        display->SetPreviewImage(std::move(image));
    }
    return true;
}

bool StackChanCamera::StreamCaptures()
{
    if (!streaming_on_.load(std::memory_order_acquire) || video_fd_ < 0) {
        return false;
    }

    {
        struct v4l2_buffer buf = {};
        buf.type               = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory             = V4L2_MEMORY_MMAP;
        if (ioctl(video_fd_, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed");
            return false;
        }

        // Do not hold this lock across the driver's potentially blocking DQBUF.
        std::lock_guard<std::mutex> frame_lock(encoder_mutex_);
        if (encoder_thread_.joinable()) {
            encoder_thread_.join();
        }

        {
            // 保存帧副本到PSRAM
            if (frame_.data) {
                heap_caps_free(frame_.data);
                frame_.data   = nullptr;
                frame_.format = 0;
            }
            frame_.len  = buf.bytesused;
            frame_.data = (uint8_t*)heap_caps_malloc(frame_.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!frame_.data) {
                ESP_LOGE(TAG, "alloc frame copy failed: need allocate %lu bytes", buf.bytesused);
                if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                    ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                }
                return false;
            }

#ifdef CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
            ESP_LOGW(TAG, "mmap_buffers_[buf.index].length = %d, sensor_width = %d, sensor_height = %d",
                     mmap_buffers_[buf.index].length, sensor_width_, sensor_height_);
#else
            ESP_LOGW(TAG, "mmap_buffers_[buf.index].length = %d, frame.width = %d, frame.height = %d",
                     mmap_buffers_[buf.index].length, frame_.width, frame_.height);
#endif  // CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE
            ESP_LOG_BUFFER_HEXDUMP(TAG, mmap_buffers_[buf.index].start, MIN(mmap_buffers_[buf.index].length, 256),
                                   ESP_LOG_DEBUG);

            switch (sensor_format_) {
                case V4L2_PIX_FMT_RGB565:
                case V4L2_PIX_FMT_RGB24:
                case V4L2_PIX_FMT_YUYV:
                case V4L2_PIX_FMT_YUV420:
                case V4L2_PIX_FMT_GREY:
#ifdef CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
                case V4L2_PIX_FMT_JPEG:
#endif  // CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                {
                    auto src16   = (uint16_t*)mmap_buffers_[buf.index].start;
                    auto dst16   = (uint16_t*)frame_.data;
                    size_t count = (size_t)mmap_buffers_[buf.index].length / 2;
                    for (size_t i = 0; i < count; i++) {
                        dst16[i] = __builtin_bswap16(src16[i]);
                    }
                }
#else
                    memcpy(frame_.data, mmap_buffers_[buf.index].start,
                           MIN(mmap_buffers_[buf.index].length, frame_.len));
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                    frame_.format = sensor_format_;
                    break;
                case V4L2_PIX_FMT_YUV422P: {
                    // 这个格式是 422 YUYV，不是 planer
                    frame_.format = V4L2_PIX_FMT_YUYV;
#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                    {
                        auto src16   = (uint16_t*)mmap_buffers_[buf.index].start;
                        auto dst16   = (uint16_t*)frame_.data;
                        size_t count = (size_t)mmap_buffers_[buf.index].length / 2;
                        for (size_t i = 0; i < count; i++) {
                            dst16[i] = __builtin_bswap16(src16[i]);
                        }
                    }
#else
                    memcpy(frame_.data, mmap_buffers_[buf.index].start,
                           MIN(mmap_buffers_[buf.index].length, frame_.len));
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_ENDIANNESS_SWAP
                    break;
                }
                case V4L2_PIX_FMT_RGB565X: {
                    // 大端序的 RGB565 需要转换为小端序
                    // 目前 esp_video 的大小端都会返回格式为 RGB565，不会返回格式为 RGB565X，此 case 用于未来版本兼容
                    auto src16         = (uint16_t*)mmap_buffers_[buf.index].start;
                    auto dst16         = (uint16_t*)frame_.data;
                    size_t pixel_count = (size_t)frame_.width * (size_t)frame_.height;
                    for (size_t i = 0; i < pixel_count; i++) {
                        dst16[i] = __builtin_bswap16(src16[i]);
                    }
                    frame_.format = V4L2_PIX_FMT_RGB565;
                    break;
                }
                default:
                    ESP_LOGE(TAG, "unsupported sensor format: 0x%08lx", sensor_format_);
                    if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
                        ESP_LOGE(TAG, "Cleanup: VIDIOC_QBUF failed");
                    }
                    return false;
            }
        }

        if (ioctl(video_fd_, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
        }
    }

    return true;
}

bool StackChanCamera::SetHMirror(bool enabled)
{
    if (video_fd_ < 0) return false;
    struct v4l2_ext_controls ctrls = {};
    struct v4l2_ext_control ctrl   = {};
    ctrl.id                        = V4L2_CID_HFLIP;
    ctrl.value                     = enabled ? 1 : 0;
    ctrls.ctrl_class               = V4L2_CTRL_CLASS_USER;
    ctrls.count                    = 1;
    ctrls.controls                 = &ctrl;
    if (ioctl(video_fd_, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGE(TAG, "set HFLIP failed");
        return false;
    }
    return true;
}

bool StackChanCamera::SetVFlip(bool enabled)
{
    if (video_fd_ < 0) return false;
    struct v4l2_ext_controls ctrls = {};
    struct v4l2_ext_control ctrl   = {};
    ctrl.id                        = V4L2_CID_VFLIP;
    ctrl.value                     = enabled ? 1 : 0;
    ctrls.ctrl_class               = V4L2_CTRL_CLASS_USER;
    ctrls.count                    = 1;
    ctrls.controls                 = &ctrl;
    if (ioctl(video_fd_, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGE(TAG, "set VFLIP failed");
        return false;
    }
    return true;
}

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 *
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 *
 * 实现特点：
 * - 使用独立线程编码JPEG，与主线程分离
 * - 采用分块传输编码(chunked transfer encoding)优化内存使用
 * - 通过队列机制实现编码线程和发送线程的数据同步
 * - 支持设备ID、客户端ID和认证令牌的HTTP头部配置
 *
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return std::string 服务器返回的JSON格式响应字符串
 *         成功时包含AI分析结果，失败时包含错误信息
 *         格式示例：{"success": true, "result": "分析结果"}
 *                  {"success": false, "message": "错误信息"}
 *
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
std::string StackChanCamera::Explain(const std::string& question)
{
    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        throw std::runtime_error("Network is not available");
    }

    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        throw std::runtime_error("Failed to create HTTP client");
    }
    http->SetTimeout(kExplainHttpTimeoutMs);

    struct HttpCleanup {
        Http* client;

        ~HttpCleanup()
        {
            try {
                Close();
            } catch (...) {
                ESP_LOGE(TAG, "Failed to close the image explain HTTP client");
            }
        }

        void Close()
        {
            if (client != nullptr) {
                Http* client_to_close = client;
                client                = nullptr;
                client_to_close->Close();
            }
        }
    } http_cleanup{http.get()};

    std::unique_lock<std::mutex> frame_lock(encoder_mutex_);
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (frame_.data == nullptr || frame_.len == 0 || frame_.format == 0) {
        throw std::runtime_error("No camera frame is available to explain");
    }

    // 创建局部的 JPEG 队列, 40 entries is about to store 512 * 40 = 20480 bytes of JPEG data
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        throw std::runtime_error("Failed to create JPEG queue");
    }

    struct JpegQueueCleanup {
        QueueHandle_t queue;
        std::mutex& mutex;
        std::thread& thread;
        bool cleaned = false;

        void Drain()
        {
            JpegChunk chunk = {};
            while (queue != nullptr && xQueueReceive(queue, &chunk, 0) == pdPASS) {
                if (chunk.data != nullptr) {
                    heap_caps_free(chunk.data);
                }
            }
        }

        void Cleanup()
        {
            if (cleaned) {
                return;
            }

            // Draining first also releases an encoder that is waiting for queue space.
            Drain();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (thread.joinable()) {
                    thread.join();
                }
            }
            Drain();
            vQueueDelete(queue);
            queue   = nullptr;
            cleaned = true;
        }

        ~JpegQueueCleanup()
        {
            Cleanup();
        }
    } jpeg_cleanup{jpeg_queue, encoder_mutex_, encoder_thread_};

    uint8_t* frame_data         = frame_.data;
    size_t frame_len            = frame_.len;
    uint16_t frame_width        = frame_.width ? frame_.width : 320;
    uint16_t frame_height       = frame_.height ? frame_.height : 240;
    v4l2_pix_fmt_t frame_format = frame_.format;

    // We spawn a thread to encode the image to JPEG using optimized encoder (cost about 500ms and 8KB SRAM)
    try {
        encoder_thread_ =
            std::thread([frame_data, frame_len, frame_width, frame_height, frame_format, jpeg_queue]() {
                bool ok = image_to_jpeg_cb(
                    frame_data, frame_len, frame_width, frame_height, frame_format, 80,
                    [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                        auto jpeg_queue = static_cast<QueueHandle_t>(arg);
                        JpegChunk chunk = {.data = nullptr, .len = len};
                        if (index == 0 && data != nullptr && len > 0) {
                            chunk.data =
                                (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (chunk.data == nullptr) {
                                ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG chunk", len);
                                chunk.len = 0;
                            } else {
                                memcpy(chunk.data, data, len);
                            }
                        } else {
                            chunk.len = 0;  // Sentinel or error
                        }

                        if (xQueueSend(jpeg_queue, &chunk, pdMS_TO_TICKS(kJpegQueueSendTimeoutMs)) != pdPASS) {
                            if (chunk.data != nullptr) {
                                heap_caps_free(chunk.data);
                            }
                            ESP_LOGE(TAG, "Timed out while queueing a JPEG chunk");
                            return 0;
                        }
                        return len;
                    },
                    jpeg_queue);

                if (!ok) {
                    JpegChunk chunk = {.data = nullptr, .len = 0};
                    if (xQueueSend(jpeg_queue, &chunk, pdMS_TO_TICKS(kJpegQueueSendTimeoutMs)) != pdPASS) {
                        ESP_LOGE(TAG, "Timed out while queueing the JPEG error marker");
                    }
                }
            });
    } catch (...) {
        frame_lock.unlock();
        throw;
    }
    frame_lock.unlock();

    // 构造multipart/form-data请求体
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // 配置HTTP客户端，使用分块传输编码
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        throw std::runtime_error("Failed to connect to explain URL");
    }

    auto checked_write = [&http](const char* data, size_t size, const char* description) {
        int written = http->Write(data, size);
        if (written < 0 || static_cast<size_t>(written) < size) {
            ESP_LOGE(TAG, "HTTP write failed while sending %s: wrote %d of %zu bytes", description, written, size);
            throw std::runtime_error("Failed to upload photo");
        }
    };

    {
        // 第一块：question字段
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        checked_write(question_field.c_str(), question_field.size(), "question");
    }
    {
        // 第二块：文件字段头部
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        checked_write(file_header.c_str(), file_header.size(), "file header");
    }

    // 第三块：JPEG数据
    size_t total_sent   = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk = {};
        if (xQueueReceive(jpeg_queue, &chunk, pdMS_TO_TICKS(kJpegQueueReceiveTimeoutMs)) != pdPASS) {
            ESP_LOGE(TAG, "Timed out while waiting for a JPEG chunk");
            throw std::runtime_error("Timed out while encoding image");
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;  // The last chunk
        }
        int written      = http->Write(reinterpret_cast<const char*>(chunk.data), chunk.len);
        size_t chunk_len = chunk.len;
        heap_caps_free(chunk.data);
        if (written < 0 || static_cast<size_t>(written) < chunk_len) {
            ESP_LOGE(TAG, "HTTP write failed while sending JPEG data: wrote %d of %zu bytes", written, chunk_len);
            throw std::runtime_error("Failed to upload photo");
        }
        total_sent += chunk_len;
    }
    jpeg_cleanup.Cleanup();

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        throw std::runtime_error("Failed to encode image to JPEG");
    }

    {
        // 第四块：multipart尾部
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        checked_write(multipart_footer.c_str(), multipart_footer.size(), "multipart footer");
    }
    // 结束块
    checked_write("", 0, "request terminator");

    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", status_code);
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    if (result.empty()) {
        ESP_LOGE(TAG, "Image explain response was empty or timed out");
        throw std::runtime_error("Image explain response was empty");
    }
    http_cleanup.Close();

    // Get remain task stack size
    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%d bytes, compressed size=%d, remain stack size=%d, question=%s\n%s",
             (int)frame_len, (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}
