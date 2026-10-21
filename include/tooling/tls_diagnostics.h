#pragma once
namespace tooling {
/** Read-only TLS handshake probe; handled on the UI task. No application data. */
bool handleTlsCommand(const char* line);
}
