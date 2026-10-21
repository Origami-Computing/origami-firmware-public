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
    void close();
private:
    uint16_t* portrait_ = nullptr;
};
}
