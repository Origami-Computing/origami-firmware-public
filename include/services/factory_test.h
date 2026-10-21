#pragma once
#include <stdint.h>
#include "services/camera_session.h"
#include "services/microphone_stream.h"
#include "services/peripheral_reservation.h"
#include "services/firmware_update.h"
#include "services/wav_capture.h"
#include "services/wav_player.h"
namespace services { namespace factory_test {
enum class Check : uint8_t { Memory, Storage, Power, Buttons, Motion, Display, Speaker, Vibration, Microphone, Camera, Wifi, InternetTls, Backend, Ota, CameraQuality, AudioQuality, Count };
constexpr unsigned kCount = static_cast<unsigned>(Check::Count);
constexpr uint32_t kAll = (1u << kCount) - 1;
const char* name(Check check);
bool completed();
bool saveReport(uint32_t passed, uint32_t failed, bool accepted);
// UI-task only. Adds the office network without replacing unrelated networks.
// Stores the test SSID separately; the password stays in the secrets owner.
bool configureWifi(const char* ssid, const char* password);
// UI-task hardware session. close() releases mic/camera/gyro/reservations on
// failure, retry, cover or exit. No photos or audio recordings are saved.
class Session {
public:
    ~Session() { close(); }
    void start(Check check, uint32_t now);
    void tick(uint32_t now);
    void close();
    bool passed() const { return passed_; }
    bool failed() const { return failed_; }
    const char* detail() const { return detail_; }
    bool operatorReady() const { return operatorReady_; }
    // UI-task controls for the temporary recording-quality take. No upload.
    bool audioRecord(uint32_t now); // toggle record/stop; new take clears listening evidence
    bool audioPlay();
    unsigned audioPhase() const { return phase_; } // 0 ready, 1 recording, 2 saved, 3 playing
    unsigned audioSeconds(uint32_t now) const;
    void audioWave(char* out) const; // caller provides 28 bytes
private:
    static bool samples(const int16_t*, uint32_t, void*);
    Check check_ = Check::Count;
    uint32_t started_ = 0, sampled_ = 0, samples_ = 0, frames_ = 0;
    int peak_ = 0, movingSamples_ = 0;
    int32_t low_[3] = {}, high_[3] = {};
    bool baseline_ = false, gyro_ = false, cameraHeld_ = false;
    bool passed_ = false, failed_ = false;
    char detail_[80] = {}, ssid_[33] = {};
    camera::Session camera_;
    audio::MicrophoneStream mic_;
    background_network::PeripheralReservation audioReservation_;
    firmware_update::Session network_;
    audio::WavCapture recording_;
    audio::WavPlayer playback_;
    char recordingPath_[128] = {};
    uint8_t phase_ = 0;
    bool operatorReady_ = false;
    uint32_t elapsed_ = 0;
};
} }
