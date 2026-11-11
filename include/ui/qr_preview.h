#pragma once
#include <stddef.h>
#include <stdint.h>
namespace ui {
// UI-task scanner renderer. Owns portrait scratch storage and JPEG block
// composition; Wi-Fi QR service retains camera, grayscale and worker ownership.
class QrPreview {
public:
    QrPreview() = default;
    ~QrPreview();
    QrPreview(const QrPreview&) = delete;
    QrPreview& operator=(const QrPreview&) = delete;
    // Borrows JPEG synchronously. True means fresh grayscale is ready to submit.
    // Failed decode or allocation never presents a partial frame.
    bool render(const uint8_t* jpeg, size_t length);
    // UI-task, idempotent. Release scratch and request restoration of the
    // status strip overwritten by preview composition.
    void close();
    void setAppLesson(bool enabled){appLesson_=enabled;}
private:
    bool appLesson_=false;
    uint16_t* portrait_ = nullptr;
};
}
