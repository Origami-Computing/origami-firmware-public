/**
 * @file launch.cpp
 * @brief Implementation of the talk->loading->launch state bridge (see launch.h).
 *
 * Single-task (ui_task) state: plain statics, no synchronization. Each event is
 * a one-shot flag consumed by exactly one reader; the title persists so the
 * loading screen can display it across frames.
 */

#include "widgets/launch.h"

#include <stdio.h>
#include <string.h>

namespace widgets {
namespace launch {

namespace {

char s_title[40]    = {};
char s_readyId[40]  = {};
char s_readySlug[40] = {};
char s_error[80]    = {};

bool s_loadingPending = false;  // consumed by the main loop to navigate
bool s_ready          = false;  // consumed by the loading screen
bool s_errorPending   = false;  // consumed by the loading screen

void cp(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

}  // namespace

void notifyLoading(const char* t) {
    cp(s_title, sizeof(s_title), t && t[0] ? t : "widget");
    s_loadingPending = true;
}

void notifyReady(const char* id, const char* slug) {
    cp(s_readyId, sizeof(s_readyId), id);
    cp(s_readySlug, sizeof(s_readySlug), slug);
    s_ready = true;
}

void notifyError(const char* msg) {
    cp(s_error, sizeof(s_error), msg && msg[0] ? msg : "generation failed");
    s_errorPending = true;
}

bool takeLoadingRequest() {
    if (!s_loadingPending) return false;
    s_loadingPending = false;
    return true;
}

const char* title() { return s_title; }

bool takeReady(char* idOut, size_t idCap, char* slugOut, size_t slugCap) {
    if (!s_ready) return false;
    s_ready = false;
    cp(idOut, idCap, s_readyId);
    cp(slugOut, slugCap, s_readySlug);
    return true;
}

bool takeError(char* msgOut, size_t msgCap) {
    if (!s_errorPending) return false;
    s_errorPending = false;
    cp(msgOut, msgCap, s_error);
    return true;
}

}  // namespace launch
}  // namespace widgets
