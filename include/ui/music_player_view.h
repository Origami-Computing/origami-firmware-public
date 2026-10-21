#pragma once
// Prototype renderer. The browser runs this C++ on the host; the approved
// renderer can be called with TFT_eSPI/TFT_eSprite, both supporting fillRect.
// No browser typography, floating-point geometry, or vector rasterization.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "ui/pixel_text.h"
#include "config/ui_theme.h"
#include "config/ui_layout.h"

namespace music_view {
struct State {
    const char* title;
    const char* artist;
    unsigned elapsed, duration;
    bool playing;
    int volume;
    bool showVolume;
    const char* clock;
    int design;
};
// Compatibility names for preview consumers; shared views use ui::pixel_text.
using ui::pixel_text::text;
using ui::pixel_text::centered;
using ui::pixel_text::shorten;
inline void titleLines(const char* source, char (&first)[21], char (&second)[21], unsigned columns=20) {
    second[0]=0;
    if (strlen(source)<=columns) { strcpy(first,source); return; }
    unsigned cut=columns;
    while(cut && source[cut]!=' ') --cut;
    if (!cut) cut=columns;
    memcpy(first,source,cut); first[cut]=0;
    while(source[cut]==' ') ++cut;
    shorten(source+cut,second,columns);
}
template<class Canvas>
void renderQuiet(Canvas& c, const State& s) {
    // System chrome remains owned by firmware; only paint below it.
    c.fillRect(0,kSystemStatusBarH,280,240-kSystemStatusBarH,kUiBg);
    char first[21],second[21],artist[39];
    titleLines(s.title[0]?s.title:"Unknown track",first,second);
    shorten(s.artist[0]?s.artist:"Unknown artist",artist,38);
    centered(c,first,second[0]?66:78,2);
    if(second[0]) centered(c,second,90,2);
    centered(c,artist,120,1,kUiMuted);
    if(s.showVolume) {
        // Replaces the progress band; no persistent extra control on screen.
        char volume[20]; snprintf(volume,sizeof(volume),"Volume %d%%",s.volume);
        centered(c,volume,153,1,kUiMuted);
        c.fillRect(38,173,204,3,kUiPanel);
        c.fillRect(38,173,204*s.volume/100,3,kUiFg);
    } else {
        c.fillRect(26,157,228,2,kUiPanel);
        unsigned elapsed=s.elapsed<s.duration?s.elapsed:s.duration;
        int fill=s.duration?(int)((uint64_t)228*elapsed/s.duration):0;
        if(fill) c.fillRect(26,157,fill,2,kUiFg);
        char left[16],right[16];
        snprintf(left,sizeof(left),"%u:%02u",s.elapsed/60,s.elapsed%60);
        if(s.duration) snprintf(right,sizeof(right),"%u:%02u",s.duration/60,s.duration%60);
        else strcpy(right,"--:--");
        text(c,left,26,169,1,kUiMuted);
        text(c,right,254-(int)strlen(right)*6,169,1,kUiMuted);
    }
    text(c,"A Back",26,211,1,kUiMuted);
    centered(c,s.playing?"C Pause":"C Play",208,2);
    text(c,"B Next",218,211,1,kUiMuted);
}
template<class Canvas>
void disc(Canvas& c,int x,int y,int radius,uint16_t ink) {
    for(int row=-radius;row<=radius;row++) {
        int half=radius;
        while(half*half+row*row>radius*radius) --half;
        c.fillRect(x-half,y+row,half*2+1,1,ink);
    }
}
template<class Canvas>
void renderMixtape(Canvas& c,const State& s) {
    const uint16_t paper=s.design==1?0xFFE0:kUiBg;
    c.fillRect(0,kSystemStatusBarH,280,240-kSystemStatusBarH,paper);
    char first[21],second[21],artist[34];
    titleLines(s.title[0]?s.title:"Unknown track",first,second,17);
    shorten(s.artist[0]?s.artist:"Unknown artist",artist,33);
    text(c,first,22,50,2,kUiFg);
    if(second[0])text(c,second,22,72,2,kUiFg);
    text(c,artist,22,98,1,kUiFg);
    // Stepped cassette silhouette with animated tape reels.
    c.fillRect(52,117,132,59,kUiFg);c.fillRect(48,121,140,51,kUiFg);
    c.fillRect(54,123,128,47,paper);c.fillRect(61,128,114,28,kUiFg);
    c.fillRect(96,138,44,8,paper);
    for(int x: {79,157}) {
        disc(c,x,142,10,paper);disc(c,x,142,3,kUiFg);
        if(s.elapsed%2) {
            c.fillRect(x-2,133,4,5,kUiFg);c.fillRect(x-2,147,4,5,kUiFg);
            c.fillRect(x-9,140,5,4,kUiFg);c.fillRect(x+4,140,5,4,kUiFg);
        } else {
            for(int dx: {-6,4})for(int dy: {-6,4})c.fillRect(x+dx,142+dy,3,3,kUiFg);
        }
    }
    c.fillRect(87,161,62,9,kUiFg);c.fillRect(92,163,52,7,paper);
    c.fillRect(59,162,3,3,kUiFg);c.fillRect(174,162,3,3,kUiFg);
    // Progress band doubles as transient volume feedback.
    c.fillRect(22,191,194,6,kUiFg);c.fillRect(24,193,190,2,paper);
    unsigned elapsed=s.elapsed<s.duration?s.elapsed:s.duration;
    int fill=s.showVolume?190*s.volume/100:(s.duration?(int)((uint64_t)190*elapsed/s.duration):0);
    if(fill)c.fillRect(24,193,fill,2,kUiFg);
    char left[20],right[16];
    if(s.showVolume)snprintf(left,sizeof(left),"VOLUME %d%%",s.volume);
    else snprintf(left,sizeof(left),"%u:%02u",s.elapsed/60,s.elapsed%60);
    if(s.duration)snprintf(right,sizeof(right),"%u:%02u",s.duration/60,s.duration%60);
    else strcpy(right,"--:--");
    text(c,left,22,208,1,kUiFg);
    if(!s.showVolume)text(c,right,216-(int)strlen(right)*6,208,1,kUiFg);
    // Action rail retains the physical button letter mapping.
    c.fillRect(238,38,42,202,kUiFg);
    text(c,"A",256,48,1,kUiBg);
    for(int row=0;row<7;row++)c.fillRect(250+(row<4?3-row:row-3)*2,63+row*2,4,2,kUiBg);
    c.fillRect(255,69,12,3,kUiBg);
    text(c,"B",256,110,1,kUiBg);
    for(int row=0;row<12;row++){int w=row<6?row+1:12-row;c.fillRect(250,126+row,w,1,kUiBg);}
    c.fillRect(263,126,3,12,kUiBg);
    text(c,"C",256,173,1,kUiBg);
    if(s.playing){c.fillRect(251,190,4,14,kUiBg);c.fillRect(261,190,4,14,kUiBg);}
    else for(int row=0;row<14;row++){int w=row<7?row+1:14-row;c.fillRect(254,190+row,w,1,kUiBg);}
}
template<class Canvas>
void renderPhysical(Canvas& c,const State& s) {
    // This direction owns the top chrome: A/B replace Wi-Fi and battery.
    c.fillRect(0,0,280,240,kUiBg);
    centered(c,s.clock && s.clock[0]?s.clock:"--:--",17,2);
    if(s.design==3) {
        for(int x: {0,216}) {
            c.fillRect(x,0,64,38,kUiFg);
            c.fillRect(x+2,2,60,34,kUiBg);
        }
        c.fillRect(216,42,64,36,kUiFg);
        c.fillRect(218,44,60,32,kUiBg);
    }
    constexpr int controlCenterX=249;
    // Next, play/pause and volume share the same horizontal center.
    if(s.design!=5)text(c,"A",51,18,1,kUiMuted);
    // Previous-track symbol, aligned with the opposite next-track control.
    c.fillRect(25,15,3,14,kUiFg);
    for(int row=0;row<14;row++) {
        int w=row<7?row+1:14-row;
        c.fillRect(39-w,15+row,w,1,kUiFg);
    }
    if(s.design!=5)text(c,"B",223,18,1,kUiMuted);
    for(int row=0;row<14;row++) {
        int w=row<7?row+1:14-row;
        c.fillRect(controlCenterX-7,15+row,w,1,kUiFg);
    }
    c.fillRect(controlCenterX+4,15,3,14,kUiFg);
    if(s.design!=5)text(c,"C",223,57,1,kUiMuted);
    if(s.playing) {c.fillRect(controlCenterX-7,53,4,16,kUiFg);c.fillRect(controlCenterX+3,53,4,16,kUiFg);}
    else for(int row=0;row<16;row++) {
        int w=row<8?row+1:16-row;
        c.fillRect(controlCenterX-4,53+row,w,1,kUiFg);
    }
    char first[21],second[21],artist[39];
    const int textCenter=s.design==5?128:140;
    titleLines(s.title[0]?s.title:"Unknown track",first,second,s.design==5?17:20);
    shorten(s.artist[0]?s.artist:"Unknown artist",artist,s.design==5?33:38);
    centered(c,first,second[0]?103:115,2,kUiFg,textCenter);
    if(second[0])centered(c,second,127,2,kUiFg,textCenter);
    centered(c,artist,158,1,kUiMuted,textCenter);
    const int barY=s.design==5?188:193;
    const int barH=s.design==5?9:3;
    const int timeScale=s.design==5?2:1;
    c.fillRect(26,barY,228,barH,kUiPanel);
    unsigned elapsed=s.elapsed<s.duration?s.elapsed:s.duration;
    const bool inlineVolume=s.showVolume && s.design!=5;
    int fill=inlineVolume?228*s.volume/100:(s.duration?(int)((uint64_t)228*elapsed/s.duration):0);
    if(fill)c.fillRect(26,barY,fill,barH,kUiFg);
    char left[20],right[16];
    if(inlineVolume)snprintf(left,sizeof(left),"Volume %d%%",s.volume);
    else snprintf(left,sizeof(left),"%u:%02u",s.elapsed/60,s.elapsed%60);
    text(c,left,26,207,timeScale,kUiFg);
    if(!inlineVolume) {
        if(s.duration)snprintf(right,sizeof(right),"%u:%02u",s.duration/60,s.duration%60);
        else strcpy(right,"--:--");
        text(c,right,254-(int)strlen(right)*6*timeScale,207,timeScale,kUiFg);
    }
    if(s.showVolume && s.design==5) {
        // Fixed text lane stays clear even when the volume indicator is hidden.
        const int w=8,x=controlCenterX-w/2,y=84,h=64;
        c.fillRect(x,y,w,h,kUiPanel);
        const int level=h*s.volume/100;
        if(level)c.fillRect(x,y+h-level,w,level,kUiFg);
    }
}
template<class Canvas>
void render(Canvas& c,const State& s) {
    if(s.design>=3)renderPhysical(c,s);
    else if(s.design)renderMixtape(c,s);else renderQuiet(c,s);
}
}
