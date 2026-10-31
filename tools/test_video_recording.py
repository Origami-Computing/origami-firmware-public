"""Compile the production AVI writer/service with threaded HAL fakes and ASan/UBSan.
No device access. Validates playable MJPEG with ffprobe/ffmpeg when installed.
"""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    def write(name, source):
        path = tmp/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
    write('Arduino.h', '#pragma once\n#include <stdint.h>\n#include <thread>\n#include <chrono>\n#define pdMS_TO_TICKS(x) (x)\ninline uint32_t millis(){return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}\ninline void vTaskDelay(unsigned n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}\n')
    write('esp_heap_caps.h', '#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nvoid* heap_caps_malloc(size_t,int);void heap_caps_free(void*);\n')
    write('esp_system.h', '#include <stdint.h>\nuint32_t esp_random();\n')
    write('hal/hal_tasks.h', '#pragma once\nnamespace hal {namespace tasks {enum class Id {VideoWrite,VideoMic,VideoCapture}; void* spawn(Id,void(*)(void*),void*);void deleteCurrent(Id);void recordCurrentStack(Id);}}\n')
    write('hal/hal_sd.h', '''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hal {namespace sd {
struct StreamWriter {void* handle=nullptr;};
bool mounted();bool ensureDir(const char*);bool exists(const char*);
bool openWriter(const char*,StreamWriter*);bool writeStream(StreamWriter*,const uint8_t*,size_t);
bool seekWriter(StreamWriter*,uint32_t);bool closeWriter(StreamWriter*);
bool replaceFile(const char*,const char*);bool removeFile(const char*);
}}
''')
    write('services/agent_coordination.h',r'''#pragma once
extern bool failMicAdmission;extern int micLeases;
namespace services {namespace agent_coordination {
class TransportPause {bool held_=false;public:
bool acquire(unsigned){if(failMicAdmission)return false;held_=true;++micLeases;return true;}
void release(){if(held_)--micLeases;held_=false;}
};}}
''')
    write('esp_camera.h', '#pragma once\nconstexpr int PIXFORMAT_JPEG=4;\n')
    write('esp_timer.h', '#pragma once\n#include <Arduino.h>\ninline int64_t esp_timer_get_time(){return int64_t(millis())*1000;}\n')
    write('services/camera_session.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>
namespace hal {namespace camera {struct CameraFrame {
const uint8_t* buf=nullptr;size_t len=0;uint16_t width=640,height=480;int format=4;uint64_t dmaStartUs=0;
};}}
namespace services {namespace camera {class Session {public:
bool active=true;bool running()const{return active;}bool backgroundHeld()const{return active;}
class FrameReader {bool held=false;std::vector<uint8_t> bytes;public:
bool acquire(Session&);bool read(hal::camera::CameraFrame*);void returnFrame();void close();
};
};}}
''')
    write('core/mic_tuning.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
namespace core {namespace audio {namespace mic_tuning {
struct Settings{};inline Settings settings(){return {};}
class Processor {public:void resetFromSnapshot(Settings,unsigned){}void processRaw(const int32_t* raw,int16_t* out,size_t count){for(size_t i=0;i<count;++i)out[i]=int16_t(raw[i]>>16);}};
}}}
''')
    write('hal/hal_audio.h',r'''#pragma once
#include <stddef.h>
constexpr int ESP_OK=0;
namespace hal {namespace audio {bool initMic(bool);void endMicSession();int readMic(void*,size_t,size_t*,unsigned);}}
''')
    exe = tmp/'check'
    subprocess.run(['c++', '-std=c++17', '-g', '-fsanitize=address,undefined', '-pthread',
                    '-I'+str(tmp), '-I'+str(ROOT/'include'),
                    str(ROOT/'tests/video/recording.cpp'), str(ROOT/'src/services/video_recording.cpp'), str(ROOT/'src/services/video_recording_audio.cpp'), str(ROOT/'src/services/video_recording_camera.cpp'),
                    '-o', str(exe)], check=True)
    jpeg = tmp/'frame.jpg'
    alternate = tmp/'alternate.jpg'
    if shutil.which('ffmpeg'):
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=c=blue:s=640x480',
                        '-frames:v','1',str(jpeg)],check=True)
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','color=c=red:s=640x480',
                        '-frames:v','1',str(alternate)],check=True)
    hd=tmp/'hd.jpg'
    if shutil.which('ffmpeg'):
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=s=1280x720','-frames:v','1',str(hd)],check=True)
    else: hd.write_bytes(bytes.fromhex('ffd8 ffc0 000b 08 02d0 0500 01 01 11 00 ffda 0008 01 01 00 00 3f 00 42 ffd9'))
    subprocess.run([str(exe), str(tmp/'clip.avi'), str(jpeg), str(alternate), str(hd)], check=True)
    if shutil.which('ffmpeg'):
        subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(tmp/'clip.avi.hd.avi'),'-f','null','-'],check=True)

    if jpeg.exists() and shutil.which('ffprobe'):
        data=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(tmp/'clip.avi')]))
        stream=data['streams'][0]
        assert stream['codec_name']=='mjpeg' and stream['width']==640 and stream['height']==480
        assert stream['nb_frames']=='4' and abs(float(stream['duration'])-0.8)<0.001, stream
        subprocess.run(['ffmpeg','-v','error','-i',str(tmp/'clip.avi'),'-f','null','-'],check=True)
        print('PASS: ffprobe metadata and ffmpeg decoded all four frames')
        av=Path(str(tmp/'clip.avi')+'.audio.avi')
        streams=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(av)]))['streams']
        assert [x['codec_name'] for x in streams]==['mjpeg','pcm_s16le'],streams
        assert streams[0]['r_frame_rate']=='30/1' and streams[1]['sample_rate']=='48000'
        subprocess.run(['ffmpeg','-v','error','-i',str(av),'-f','null','-'],check=True)
        pcm=subprocess.check_output(['ffmpeg','-v','error','-i',str(av),'-map','0:a','-f','s16le','-'])
        import struct
        assert list(struct.unpack('<'+'h'*(len(pcm)//2),pcm))==[i%1000-500 for i in range(len(pcm)//2)]
        wrapped=Path(str(tmp/'clip.avi')+'.wrapped.avi')
        pcm=subprocess.check_output(['ffmpeg','-v','error','-xerror','-i',str(wrapped),'-map','0:a','-f','s16le','-'])
        assert len(pcm)>192000
        assert list(struct.unpack('<'+'h'*(len(pcm)//2),pcm))==[i%1000-500 for i in range(len(pcm)//2)]
        for source in [av,wrapped]:
            metadata=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v','-show_entries','stream=nb_frames','-of','json',str(source)]))
            rendered=subprocess.check_output(['ffmpeg','-v','error','-xerror','-i',str(source),'-map','0:v','-vf',f"fps=30,trim=end_frame={int(metadata['streams'][0]['nb_frames'])}",'-f','framemd5','-']).decode().splitlines()
            assert len([line for line in rendered if line and not line.startswith('#')])==int(metadata['streams'][0]['nb_frames']), (str(source), len([line for line in rendered if line and not line.startswith('#')]), metadata)
        # Independently expand the actual writer's JPEG/hold timeline. Compare
        # every RGB frame, including color changes after gaps and final padding.
        import hashlib
        b=wrapped.read_bytes();movie_end=320+int.from_bytes(b[316:320],'little')
        at=324;expected=[];last=None;decoded={}
        while at<movie_end:
            tag=b[at:at+4];n=int.from_bytes(b[at+4:at+8],'little');payload=b[at+8:at+8+n]
            at+=8+n+(n&1)
            if tag!=b'00dc':continue
            if n:
                key=hashlib.sha256(payload).digest()
                if key not in decoded:
                    rgb=subprocess.check_output(['ffmpeg','-v','error','-xerror','-threads','1',
                        '-f','image2pipe','-i','-','-frames:v','1','-pix_fmt','rgb24','-f','rawvideo','-'],input=payload)
                    decoded[key]=hashlib.md5(rgb).hexdigest()
                last=decoded[key]
            assert last is not None
            expected.append(last)
        assert len(set(expected))>=2
        rendered=subprocess.check_output(['ffmpeg','-v','error','-xerror','-threads','1','-i',str(wrapped),
            '-map','0:v','-vf',f'fps=source_fps,trim=end_frame={len(expected)},format=rgb24','-f','framemd5','-']).decode()
        actual=[line.split(',')[-1].strip() for line in rendered.splitlines() if line and not line.startswith('#')]
        assert actual==expected,'Sparse timeline changed picture positions'
        print('PASS: exact multi-color RGB timeline across sparse holds and final padding')
        print('PASS: real combined recordings decoded by FFmpeg, exact PCM through ring wrap and fixed 30 Hz timeline')
    else:
        print('SKIP: ffmpeg/ffprobe playback validation (not installed)')
