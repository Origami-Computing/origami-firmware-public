#include "core/recording_waveform.h"

#ifdef ORIGAMI_DEV_SERIAL_INPUT
namespace core {
namespace audio {
namespace {
void feedConstant(RecordingWaveform& meter, int16_t value, uint32_t count) {
    int32_t raw[173];
    for (int32_t& sample : raw) sample = (int32_t)value * 65536;
    while (count) {
        const uint32_t n = count < 173 ? count : 173;
        meter.ingestRaw(raw, n);
        count -= n;
    }
}
void feedSignal(RecordingWaveform& meter, uint32_t chunk) {
    int32_t raw[512];
    const uint32_t total = 32 * RecordingWaveform::kSamplesPerBar + 37;
    for (uint32_t offset=0; offset<total; offset+=chunk) {
        const uint32_t n = total-offset < chunk ? total-offset : chunk;
        for (uint32_t i=0; i<n; ++i) {
            const uint32_t sample = offset+i;
            const int32_t amplitude = (sample / 4800 % 8) * 900;
            // Brief peaks near window boundaries must survive smaller chunks.
            raw[i] = (sample % 4800 < 3 ? amplitude : -amplitude/8) * 65536;
        }
        meter.ingestRaw(raw,n);
    }
}
}

bool checkRecordingWaveform() {
    RecordingWaveform morning, recorder, agent;
    feedSignal(morning,256);
    feedSignal(recorder,173);
    feedSignal(agent,512);
    if (morning.revision()!=32 || recorder.revision()!=32 || agent.revision()!=32) return false;
    if (memcmp(morning.data(),recorder.data(),RecordingWaveform::kBars) ||
        memcmp(morning.data(),agent.data(),RecordingWaveform::kBars)) return false;
    agent.reset();
    feedConstant(agent,4096,1);
    feedConstant(agent,0,4798);
    if (agent.revision()!=0) return false;
    feedConstant(agent,0,1);
    if (agent.level()!=57 || agent.revision()!=1) return false;
    feedConstant(agent,0,4800);
    if (agent.level()!=0 || agent.data()[25]!=57) return false;
    feedConstant(agent,INT16_MIN,4800);
    if (agent.level()!=255) return false;
    feedConstant(agent,100,4800);
    if (agent.level()!=0) return false;
    feedConstant(agent,2000,4799);
    agent.reset();
    feedConstant(agent,0,4800);
    for (int i=0; i<RecordingWaveform::kBars; ++i) if (agent.data()[i]!=0) return false;
    return agent.revision()==1;
}
}
}
#endif
