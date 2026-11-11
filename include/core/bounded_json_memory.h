#pragma once
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif
namespace core {
class BoundedJsonMemory : public ArduinoJson::Allocator {
    size_t used=0,limit_;
    struct alignas(max_align_t) Header {size_t size;};
public:
    explicit BoundedJsonMemory(size_t limit):limit_(limit) {}
    void* allocate(size_t n) override {
        if(n>limit_-used)return nullptr;
#ifdef ARDUINO
        auto* h=(Header*)heap_caps_malloc(n+sizeof(Header),MALLOC_CAP_SPIRAM);
#else
        auto* h=(Header*)malloc(n+sizeof(Header));
#endif
        if(!h)return nullptr;h->size=n;used+=n;return h+1;
    }
    void deallocate(void* p) override {if(p){auto* h=(Header*)p-1;used-=h->size;free(h);}}
    void* reallocate(void* p,size_t n) override {
        if(!p)return allocate(n);
        auto* h=(Header*)p-1;void* next=allocate(n);if(!next)return nullptr;
        memcpy(next,p,n<h->size?n:h->size);deallocate(p);return next;
    }
};
}
