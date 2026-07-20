#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <img_converters.h>

#include "esp32_camera.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "mcp_server.h"
#include "system_info.h"
#include "jpg/image_to_jpeg.h"
#include "esp_timer.h"

#define TAG "Esp32Camera"

Esp32Camera::Esp32Camera(const camera_config_t &config) {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0); // Control camera mirror: 1 for mirror, 0 for normal
        }
        ESP_LOGI(TAG, "Camera initialized: format=%d", config.pixel_format);
    }

    streaming_on_ = true;
}

Esp32Camera::~Esp32Camera() {
    if (streaming_on_) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
        if (encode_buf_) {
            heap_caps_free(encode_buf_);
            encode_buf_ = nullptr;
            encode_buf_size_ = 0;
        }
        esp_camera_deinit();
        streaming_on_ = false;
    }
}

void Esp32Camera::SetExplainUrl(const std::string &url, const std::string &token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    return CaptureInternal(true);
}

bool Esp32Camera::CaptureSilent() {
    return CaptureInternal(false);
}

bool Esp32Camera::CaptureInternal(bool show_preview) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    if (!streaming_on_) {
        return false;
    }

    // Get the latest frame, discard old frames for real-time performance
    for (int i = 0; i < 2; i++) {
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
        }
        current_fb_ = esp_camera_fb_get();
        if (!current_fb_) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // Prepare encode buffer for RGB565 format (with optional byte swapping)
    if (current_fb_->format == PIXFORMAT_RGB565) {
        size_t pixel_count = current_fb_->width * current_fb_->height;
        size_t data_size = pixel_count * 2;

        // Allocate or reallocate encode buffer if needed
        if (encode_buf_size_ < data_size) {
            if (encode_buf_) {
                heap_caps_free(encode_buf_);
            }
            encode_buf_ = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (encode_buf_ == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate memory for encode buffer");
                encode_buf_size_ = 0;
                return false;
            }
            encode_buf_size_ = data_size;
        }

        // Copy data to encode buffer with optional byte swapping
        uint16_t *src = (uint16_t *)current_fb_->buf;
        uint16_t *dst = (uint16_t *)encode_buf_;
        if (swap_bytes_enabled_) {
            for (size_t i = 0; i < pixel_count; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
        } else {
            memcpy(encode_buf_, current_fb_->buf, data_size);
        }

        if (show_preview) {
            uint8_t *preview_data = (uint8_t *)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (preview_data != nullptr) {
                memcpy(preview_data, encode_buf_, data_size);
                auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
                if (display != nullptr) {
                    display->SetPreviewImage(std::make_unique<LvglAllocatedImage>(preview_data, data_size, current_fb_->width, current_fb_->height, current_fb_->width * 2, LV_COLOR_FORMAT_RGB565));
                } else {
                    heap_caps_free(preview_data);
                }
            }
        }
    } else if (current_fb_->format == PIXFORMAT_JPEG) {
        // JPEG format preview usually requires decoding, skip preview display for now, just log
        ESP_LOGW(TAG, "JPEG capture success, len=%u, but not supported for preview",
                 static_cast<unsigned>(current_fb_->len));
    }

    ESP_LOGI(TAG, "Captured frame: %dx%d, len=%u, format=%d",
             current_fb_->width, current_fb_->height,
             static_cast<unsigned>(current_fb_->len), current_fb_->format);

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_hmirror(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return false;
    }
    s->set_vflip(s, enabled ? 1 : 0);
    return true;
}

bool Esp32Camera::SetSwapBytes(bool enabled) {
    swap_bytes_enabled_ = enabled;
    return true;
}

uint32_t Esp32Camera::GetFrameSignature() const {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

    if (current_fb_ == nullptr || current_fb_->buf == nullptr) {
        return 0;
    }

    if (current_fb_->format != PIXFORMAT_RGB565 || current_fb_->width == 0 || current_fb_->height == 0) {
        const uint8_t* data = current_fb_->buf;
        size_t len = current_fb_->len;
        size_t step = len > 512 ? len / 512 : 1;
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < len; i += step) {
            hash ^= data[i];
            hash *= 16777619u;
        }
        hash ^= static_cast<uint32_t>(len);
        return hash;
    }

    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(
        encode_buf_ != nullptr ? encode_buf_ : current_fb_->buf);
    const int width = current_fb_->width;
    const int height = current_fb_->height;
    uint16_t samples[32] = {};
    int sample_count = 0;
    uint32_t total_luma = 0;

    for (int gy = 0; gy < 4; ++gy) {
        int y = (height * (gy * 2 + 1)) / 8;
        if (y >= height) {
            y = height - 1;
        }
        for (int gx = 0; gx < 8; ++gx) {
            int x = (width * (gx * 2 + 1)) / 16;
            if (x >= width) {
                x = width - 1;
            }
            uint16_t pixel = pixels[y * width + x];
            uint8_t r = (pixel >> 11) & 0x1f;
            uint8_t g = (pixel >> 5) & 0x3f;
            uint8_t b = pixel & 0x1f;
            uint16_t luma = static_cast<uint16_t>(r * 3 + g * 6 + b);
            samples[sample_count++] = luma;
            total_luma += luma;
        }
    }

    if (sample_count == 0) {
        return 0;
    }

    uint32_t average_luma = total_luma / sample_count;
    uint32_t signature = 0;
    for (int i = 0; i < sample_count; ++i) {
        if (samples[i] > average_luma) {
            signature |= (1u << i);
        }
    }
    return signature;
}

std::string Esp32Camera::Explain(const std::string &question) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);

    if (explain_url_.empty()) {
        throw std::runtime_error("Image explain URL or token is not set");
    }

    if (current_fb_ == nullptr) {
        throw std::runtime_error("No camera frame captured");
    }

    // Create local JPEG queue
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        throw std::runtime_error("Failed to create JPEG queue");
    }

    // Start encoding thread
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        int64_t start_time = esp_timer_get_time();
        uint16_t w = current_fb_->width;
        uint16_t h = current_fb_->height;
        v4l2_pix_fmt_t enc_fmt;
        switch (current_fb_->format) {
            case PIXFORMAT_RGB565:
                enc_fmt = V4L2_PIX_FMT_RGB565;
                break;
            case PIXFORMAT_YUV422:
                enc_fmt = V4L2_PIX_FMT_YUYV;  // YUV422 is actually YUYV format
                break;
            case PIXFORMAT_YUV420:
                enc_fmt = V4L2_PIX_FMT_YUV420;
                break;
            case PIXFORMAT_GRAYSCALE:
                enc_fmt = V4L2_PIX_FMT_GREY;
                break;
            case PIXFORMAT_JPEG:
                enc_fmt = V4L2_PIX_FMT_JPEG;
                break;
            case PIXFORMAT_RGB888:
                enc_fmt = V4L2_PIX_FMT_RGB24;
                break;
            default:
                ESP_LOGE(TAG, "Unsupported pixel format: %d", current_fb_->format);
                return;
        }

        // Use encode buffer for RGB565, otherwise use original frame buffer
        uint8_t *jpeg_src_buf = current_fb_->buf;
        size_t jpeg_src_len = current_fb_->len;
        if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_ != nullptr) {
            jpeg_src_buf = encode_buf_;
            jpeg_src_len = encode_buf_size_;
        }

        uint8_t *analysis_buf = nullptr;
        if (enc_fmt == V4L2_PIX_FMT_RGB565 && (w > 320 || h > 240)) {
            uint16_t analysis_w = w;
            uint16_t analysis_h = h;
            if (analysis_w > 320) {
                analysis_h = static_cast<uint16_t>((static_cast<uint32_t>(analysis_h) * 320) / analysis_w);
                analysis_w = 320;
            }
            if (analysis_h > 240) {
                analysis_w = static_cast<uint16_t>((static_cast<uint32_t>(analysis_w) * 240) / analysis_h);
                analysis_h = 240;
            }

            const size_t analysis_len = static_cast<size_t>(analysis_w) * analysis_h * 2;
            analysis_buf = static_cast<uint8_t *>(
                heap_caps_malloc(analysis_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (analysis_buf != nullptr) {
                const uint16_t *src = reinterpret_cast<const uint16_t *>(jpeg_src_buf);
                uint16_t *dst = reinterpret_cast<uint16_t *>(analysis_buf);
                for (uint16_t y = 0; y < analysis_h; ++y) {
                    const uint16_t src_y = static_cast<uint16_t>((static_cast<uint32_t>(y) * h) / analysis_h);
                    for (uint16_t x = 0; x < analysis_w; ++x) {
                        const uint16_t src_x = static_cast<uint16_t>((static_cast<uint32_t>(x) * w) / analysis_w);
                        dst[static_cast<size_t>(y) * analysis_w + x] =
                            src[static_cast<size_t>(src_y) * w + src_x];
                    }
                }
                jpeg_src_buf = analysis_buf;
                jpeg_src_len = analysis_len;
                w = analysis_w;
                h = analysis_h;
            } else {
                ESP_LOGW(TAG, "Failed to allocate analysis buffer; using full-resolution upload");
            }
        }

        bool ok = image_to_jpeg_cb(jpeg_src_buf, jpeg_src_len, w, h, enc_fmt, 70,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto jpeg_queue = static_cast<QueueHandle_t>(arg);
                JpegChunk chunk = {.data = nullptr, .len = len};
                if (index == 0 && data != nullptr && len > 0) {
                    chunk.data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (chunk.data == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate %u bytes for JPEG chunk",
                                 static_cast<unsigned>(len));
                        chunk.len = 0;
                    } else {
                        memcpy(chunk.data, data, len);
                    }
                } else {
                    chunk.len = 0;  // Sentinel or error
                }
                xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
                return len;
            }, jpeg_queue);

        if (analysis_buf != nullptr) {
            heap_caps_free(analysis_buf);
        }

        if (!ok) {
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        }
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "JPEG encoding time: %ld ms", int((end_time - start_time) / 1000));
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        throw std::runtime_error("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        http->Write((const char *)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        throw std::runtime_error("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        throw std::runtime_error("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             current_fb_->width, current_fb_->height, (int)total_sent, (int)remain_stack_size, question.c_str(), result.c_str());
    return result;
}

std::string Esp32Camera::CaptureAndExplain(const std::string &question, bool show_preview) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    if (!CaptureInternal(show_preview)) {
        throw std::runtime_error("Failed to capture photo");
    }
    return Explain(question);
}

bool Esp32Camera::CaptureAndGetJpeg(CameraPhoto& photo, uint8_t quality, bool show_preview) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    return CaptureInternal(show_preview) && GetCapturedJpeg(photo, quality);
}

std::string Esp32Camera::CaptureExplainAndGetJpeg(const std::string& question, CameraPhoto& photo,
                                                  uint8_t quality, bool show_preview) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    if (!CaptureInternal(show_preview)) {
        throw std::runtime_error("Failed to capture photo");
    }

    // Prepare the archive copy before the network explanation consumes heap.
    if (!GetCapturedJpeg(photo, quality)) {
        ESP_LOGE(TAG, "Failed to prepare the captured frame for cloud archive");
    }
    return Explain(question);
}

bool Esp32Camera::GetCapturedJpeg(CameraPhoto& photo, uint8_t quality) {
    std::lock_guard<std::recursive_mutex> lock(operation_mutex_);
    photo.Reset();
    if (current_fb_ == nullptr || current_fb_->buf == nullptr) {
        ESP_LOGE(TAG, "Archive JPEG skipped: no captured frame");
        return false;
    }

    v4l2_pix_fmt_t format;
    switch (current_fb_->format) {
        case PIXFORMAT_RGB565: format = V4L2_PIX_FMT_RGB565; break;
        case PIXFORMAT_YUV422: format = V4L2_PIX_FMT_YUYV; break;
        case PIXFORMAT_YUV420: format = V4L2_PIX_FMT_YUV420; break;
        case PIXFORMAT_GRAYSCALE: format = V4L2_PIX_FMT_GREY; break;
        case PIXFORMAT_JPEG: format = V4L2_PIX_FMT_JPEG; break;
        case PIXFORMAT_RGB888: format = V4L2_PIX_FMT_RGB24; break;
        default:
            ESP_LOGE(TAG, "Archive JPEG skipped: unsupported pixel format %d", current_fb_->format);
            return false;
    }

    uint8_t* source = current_fb_->buf;
    size_t source_size = current_fb_->len;
    uint16_t archive_width = current_fb_->width;
    uint16_t archive_height = current_fb_->height;
    uint8_t* archive_source = nullptr;
    if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_ != nullptr) {
        source = encode_buf_;
        source_size = static_cast<size_t>(current_fb_->width) * current_fb_->height * 2;
    }

    // A 320x240 archive remains clear on the web page and uses the same proven
    // encoder size as visual analysis, avoiding the full 640x480 memory peak.
    if (format == V4L2_PIX_FMT_RGB565 && (archive_width > 320 || archive_height > 240)) {
        if (archive_width > 320) {
            archive_height = static_cast<uint16_t>(
                (static_cast<uint32_t>(archive_height) * 320) / archive_width);
            archive_width = 320;
        }
        if (archive_height > 240) {
            archive_width = static_cast<uint16_t>(
                (static_cast<uint32_t>(archive_width) * 240) / archive_height);
            archive_height = 240;
        }

        const size_t archive_size = static_cast<size_t>(archive_width) * archive_height * 2;
        archive_source = static_cast<uint8_t*>(
            heap_caps_malloc(archive_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (archive_source == nullptr) {
            ESP_LOGE(TAG,
                     "Archive resize allocation failed: bytes=%u free_internal=%u free_psram=%u",
                     static_cast<unsigned>(archive_size),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            return false;
        }

        const uint16_t* src = reinterpret_cast<const uint16_t*>(source);
        uint16_t* dst = reinterpret_cast<uint16_t*>(archive_source);
        for (uint16_t y = 0; y < archive_height; ++y) {
            const uint16_t src_y = static_cast<uint16_t>(
                (static_cast<uint32_t>(y) * current_fb_->height) / archive_height);
            for (uint16_t x = 0; x < archive_width; ++x) {
                const uint16_t src_x = static_cast<uint16_t>(
                    (static_cast<uint32_t>(x) * current_fb_->width) / archive_width);
                dst[static_cast<size_t>(y) * archive_width + x] =
                    src[static_cast<size_t>(src_y) * current_fb_->width + src_x];
            }
        }
        source = archive_source;
        source_size = archive_size;
    }

    uint8_t* encoded = nullptr;
    size_t encoded_size = 0;
    const bool encoded_ok = image_to_jpeg(source, source_size, archive_width, archive_height,
                                          format, quality, &encoded, &encoded_size);
    if (archive_source != nullptr) {
        heap_caps_free(archive_source);
    }
    if (!encoded_ok || encoded == nullptr || encoded_size == 0) {
        ESP_LOGE(TAG,
                 "Archive JPEG encoding failed: %ux%u free_internal=%u free_psram=%u",
                 archive_width, archive_height,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }

    uint8_t* compact = static_cast<uint8_t*>(
        heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compact == nullptr) {
        free(encoded);
        ESP_LOGE(TAG,
                 "Archive JPEG compact allocation failed: bytes=%u free_internal=%u free_psram=%u",
                 static_cast<unsigned>(encoded_size),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    memcpy(compact, encoded, encoded_size);
    free(encoded);
    photo.Reset(compact, encoded_size, archive_width, archive_height);
    ESP_LOGI(TAG, "Prepared archive JPEG: %dx%d, size=%u",
             archive_width, archive_height, static_cast<unsigned>(encoded_size));
    return true;
}
