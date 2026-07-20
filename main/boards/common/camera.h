#ifndef CAMERA_H
#define CAMERA_H

#include <string>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

struct CameraPhoto {
    uint8_t* data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;

    CameraPhoto() = default;
    ~CameraPhoto() { Reset(); }
    CameraPhoto(const CameraPhoto&) = delete;
    CameraPhoto& operator=(const CameraPhoto&) = delete;
    CameraPhoto(CameraPhoto&& other) noexcept {
        data = other.data;
        size = other.size;
        width = other.width;
        height = other.height;
        other.data = nullptr;
        other.size = 0;
    }
    CameraPhoto& operator=(CameraPhoto&& other) noexcept {
        if (this != &other) {
            Reset();
            data = other.data;
            size = other.size;
            width = other.width;
            height = other.height;
            other.data = nullptr;
            other.size = 0;
        }
        return *this;
    }
    void Reset(uint8_t* new_data = nullptr, size_t new_size = 0, int new_width = 0, int new_height = 0) {
        if (data != nullptr) {
            free(data);
        }
        data = new_data;
        size = new_size;
        width = new_width;
        height = new_height;
    }
};

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool CaptureSilent() { return Capture(); }
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;
    virtual bool GetCapturedJpeg(CameraPhoto& photo, uint8_t quality = 80) {
        (void)photo;
        (void)quality;
        return false;
    }
    virtual bool CaptureAndGetJpeg(CameraPhoto& photo, uint8_t quality = 80, bool show_preview = true) {
        bool captured = show_preview ? Capture() : CaptureSilent();
        return captured && GetCapturedJpeg(photo, quality);
    }
    virtual std::string CaptureAndExplain(const std::string& question, bool show_preview = true) {
        bool captured = show_preview ? Capture() : CaptureSilent();
        if (!captured) {
            throw std::runtime_error("Failed to capture photo");
        }
        return Explain(question);
    }
    virtual std::string CaptureExplainAndGetJpeg(const std::string& question, CameraPhoto& photo,
                                                 uint8_t quality = 80, bool show_preview = true) {
        auto result = CaptureAndExplain(question, show_preview);
        GetCapturedJpeg(photo, quality);
        return result;
    }
};

#endif // CAMERA_H
