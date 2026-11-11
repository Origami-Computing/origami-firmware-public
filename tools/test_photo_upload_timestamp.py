"""Exercise the production multipart envelope with known/unknown capture times."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/upload_queue.cpp').read_text()
start = source.index('    // Capture time is immutable')
end = source.index('    size_t contentLength', start)
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <string>
std::string envelope(const char* type, uint32_t captured) {
    struct { const char* type; uint32_t capturedAtUnix; } item{type, captured};
    const char* boundary = "----OrigamiBoundary7MA4YWxk";
''' + source[start:end] + r'''
    assert(postambleLen > 0 && size_t(postambleLen) < sizeof(postamble));
    return postamble;
}
int main() {
    const auto known = envelope("image", 1700000001);
    assert(known.find("name=\"capturedAt\"\r\n\r\n2023-11-14T22:13:21Z\r\n") != std::string::npos);
    assert(known.find("name=\"platform\"\r\n\r\ndevice\r\n") != std::string::npos);
    assert(known.find("name=\"sessionId\"\r\n\r\norigami-device\r\n") != std::string::npos);
    assert(known.substr(known.size()-strlen("------OrigamiBoundary7MA4YWxk--\r\n")) == "------OrigamiBoundary7MA4YWxk--\r\n");
    assert(envelope("image", 0).find("capturedAt") == std::string::npos);
    const auto audio = envelope("audio", 1700000001);
    assert(audio.find("capturedAt") == std::string::npos);
    assert(audio.find("timeline_only") != std::string::npos);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined', str(path/'test.cpp'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
print('PASS photo UTC multipart timestamp, unknown-clock omission, and unchanged audio fields')
