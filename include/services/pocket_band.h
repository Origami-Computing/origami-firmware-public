#pragma once
#include <stdint.h>
namespace services { namespace pocket_band {
// Single UI-task session. Service owns PCM, microphone reservation and playback;
// no native-module callback is retained. Playback cleanup may finish later in
// the existing retirement queue; another session is rejected until then.
enum class State:uint8_t {Closed,Editing,Recording,Rendering,Playing,Error};
struct Snapshot {State state;uint16_t pattern[4];uint16_t bpm;uint8_t progress;const char* error;};
bool open();
void close();
void poll();
bool record(unsigned track);
bool toggle(unsigned track,unsigned step);
bool tempo(unsigned bpm);
bool play();
void stop();
Snapshot snapshot(); // UI task, error is a static string.
}}
