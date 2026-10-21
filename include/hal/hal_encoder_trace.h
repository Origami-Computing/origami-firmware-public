#pragma once
#include "core/encoder_trace.h"
namespace hal { namespace input {
using TraceKind=core::rotary::TraceKind;
// UI task only, except the HAL's locked ISR writer. No serial I/O in capture.
// Menu observations apply to shared ListView; route absence is not a menu result.
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void encoderTrace(TraceKind kind,uint32_t event=0,int a=0,int b=0,int c=0,int d=0);
void encoderTraceContext(uint32_t event);
void encoderTraceMenu(int before,int after,int total,int delta);
void encoderTraceStart();
void encoderTraceStop();
void encoderTraceService();
struct EncoderTracePage {
  uint32_t generation,count,offset,size;bool active,overflow;
  uint32_t capacity;uint8_t overflowReason;bool storageReady;
  core::rotary::TraceRecord records[16];
};
EncoderTracePage encoderTracePage(uint32_t offset);
#else
inline void encoderTrace(TraceKind,uint32_t=0,int=0,int=0,int=0,int=0){}
inline void encoderTraceContext(uint32_t){}
inline void encoderTraceMenu(int,int,int,int){}
inline void encoderTraceService(){}
#endif
} }
