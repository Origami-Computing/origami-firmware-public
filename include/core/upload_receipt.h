#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

namespace core { namespace uploads {
// Parse only a bounded, explicitly framed response. The caller owns transport,
// cancellation/deadlines and scratch. readByte returns -1 on EOF or interruption.
// No socket operations or media ownership live in this parser.
template<size_t LineCapacity=256,class ReadByte>
bool readReceiptBody(ReadByte readByte, char* body, size_t capacity, size_t& length) {
    length=0;
    char line[LineCapacity];
    size_t envelopeBytes=0;
    auto readLine=[&]() {
        size_t n=0;
        for (;;) {
            const int c=readByte();
            if (c<0 || ++envelopeBytes>4096) return false;
            if (c=='\n') {
                if (!n || line[n-1]!='\r') return false;
                line[n-1]=0; return true;
            }
            if (!c || n>=sizeof(line)-1) return false;
            line[n++]=char(c);
        }
    };
    auto number=[](const char* value, unsigned base, size_t limit, size_t& result) {
        result=0; if (!*value) return false;
        for (;*value;++value) {
            unsigned d;
            if (*value>='0' && *value<='9') d=*value-'0';
            else if (*value>='a' && *value<='f') d=*value-'a'+10;
            else if (*value>='A' && *value<='F') d=*value-'A'+10;
            else return false;
            if (d>=base || d>limit || result>(limit-d)/base) return false;
            result=result*base+d;
        }
        return true;
    };
    bool haveLength=false,chunked=false;
    size_t contentLength=0;
    for (;;) {
        if (!readLine()) return false;
        if (!line[0]) break;
        char* colon=strchr(line,':');
        if (!colon || colon==line) return false;
        *colon=0;
        char* value=colon+1;
        while (*value==' ' || *value=='\t') ++value;
        char* end=value+strlen(value);
        while (end>value && (end[-1]==' ' || end[-1]=='\t')) *--end=0;
        if (!strcasecmp(line,"Content-Length")) {
            if (haveLength || chunked || !number(value,10,capacity,contentLength)) return false;
            haveLength=true;
        } else if (!strcasecmp(line,"Transfer-Encoding")) {
            if (chunked || haveLength || strcasecmp(value,"chunked")) return false;
            chunked=true;
        } else if (!strcasecmp(line,"Content-Encoding") && strcasecmp(value,"identity")) return false;
    }
    if (!haveLength && !chunked) return false;
    for (;;) {
        size_t bytes=contentLength;
        if (chunked) {
            if (!readLine() || !number(line,16,capacity-length,bytes)) return false;
            if (!bytes) {
                // Trailer fields cannot replace the receipt or framing headers.
                while (readLine()) { if (!line[0]) return length!=0; }
                return false;
            }
        }
        while (bytes--) {
            const int c=readByte();
            if (c<0 || length>=capacity) return false;
            body[length++]=char(c);
        }
        if (!chunked) return length!=0;
        if (readByte()!='\r' || readByte()!='\n') return false;
    }
}

inline bool validReceiptId(const char* id) {
    if (!id || strlen(id)!=36) return false;
    for (size_t i=0;i<36;++i) {
        if (i==8 || i==13 || i==18 || i==23) { if (id[i]!='-') return false; }
        else if (!((id[i]>='0' && id[i]<='9') || (id[i]>='a' && id[i]<='f') ||
                   (id[i]>='A' && id[i]<='F'))) return false;
    }
    return true;
}

inline bool validReceipt(const char* body, size_t length, const char* expectedKind) {
    struct Reader {
        const char* data; size_t size,pos;
        Reader(const char* d,size_t n):data(d),size(n),pos(0) {}
        int read() {return pos<size?static_cast<unsigned char>(data[pos++]):-1;}
        size_t readBytes(char* dst,size_t n) {
            const size_t got=n<size-pos?n:size-pos;
            memcpy(dst,data+pos,got);pos+=got;return got;
        }
    } reader{body,length};
    JsonDocument doc;
    if (deserializeJson(doc,reader,DeserializationOption::NestingLimit(3))) return false;
    while (reader.pos<length) {
        const char c=body[reader.pos++];
        if (c!=' ' && c!='\r' && c!='\n' && c!='\t') return false;
    }
    if (!doc["uploaded"].is<bool>() || !doc["uploaded"].as<bool>() ||
        !doc["kind"].is<const char*>() || strcmp(doc["kind"].as<const char*>(),expectedKind)) return false;
    const char* id=doc["id"] | "";
    return validReceiptId(id);
}
} }
