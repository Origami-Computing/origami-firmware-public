bool parseBackendUrl() {
    const char* raw = core::secrets::backendUrl();
    if (!raw || !raw[0]) return false;

    const char* host = raw;
    if (!strncmp(raw, "https://", 8)) {
        s_wsSecure = true;
        s_wsPort = 443;
        host = raw + 8;
    } else if (!strncmp(raw, "http://", 7)) {
        s_wsSecure = false;
        s_wsPort = 80;
        host = raw + 7;
    } else {
        return false;
    }

    const char* slash = strchr(host, '/');
    const char* hostEnd = slash ? slash : host + strlen(host);
    const char* colon = nullptr;
    for (const char* p = host; p < hostEnd; ++p) {
        if (*p == ':') colon = p;
    }
    const char* nameEnd = colon ? colon : hostEnd;
    const size_t hostLen = (size_t)(nameEnd - host);
    if (hostLen == 0 || hostLen >= sizeof(s_wsHost)) return false;
    memcpy(s_wsHost, host, hostLen);
    s_wsHost[hostLen] = '\0';
    if (colon) s_wsPort = (uint16_t)atoi(colon + 1);

    if (slash && slash[1]) {
        const size_t baseLen = strlen(slash);
        const bool trailingSlash = baseLen > 0 && slash[baseLen - 1] == '/';
        snprintf(s_wsPath, sizeof(s_wsPath), "%s%s%s", slash,
                 trailingSlash ? "" : "/", cfg::agent::kWebSocketPath + 1);
    } else {
        snprintf(s_wsPath, sizeof(s_wsPath), "%s", cfg::agent::kWebSocketPath);
    }
    snprintf(s_wsHeaders, sizeof(s_wsHeaders),
             "X-Device-Id: %s\r\nX-Api-Key: %s",
             core::device::id(), core::secrets::apiKey());
    return s_wsPort != 0;
}
