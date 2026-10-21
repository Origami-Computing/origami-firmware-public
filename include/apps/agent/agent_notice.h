#pragma once
#include <stdint.h>
#include <cstring>
namespace apps { namespace agent { namespace notice {
enum class Kind : uint8_t { Offline, Reconnecting, Ready, Lost, Service, Timeout, Mic, Empty, Busy, Overflow, Auth };
constexpr uint32_t kReconnectMs=4000;
inline bool needsWifi(Kind k){return k==Kind::Offline||k==Kind::Reconnecting||k==Kind::Lost;}
inline bool connectionNotice(Kind k){return needsWifi(k)||k==Kind::Ready;}
inline const char* message(Kind k){
 switch(k){
 case Kind::Offline:return "I need Wi-Fi to chat. Let's connect.";
 case Kind::Reconnecting:return "Reconnecting to Wi-Fi...";
 case Kind::Ready:return "I'm connected. Hold A+B to talk.";
 case Kind::Lost:return "Wi-Fi dropped. Please say that again when I'm connected.";
 case Kind::Timeout:return "It's taking too long. Please try again.";
 case Kind::Mic:return "I couldn't start the mic. Please try again.";
 case Kind::Empty:return "I didn't catch anything. Hold A+B while you speak.";
 case Kind::Busy:return "I need a moment. Please try again.";
 case Kind::Overflow:return "I couldn't send all of that. Please try again.";
 case Kind::Auth:return "I couldn't sign in. Check my setup in Settings.";
 default:return "I couldn't reach the chat service. Please try again.";
 }
}
inline Kind classify(const char* error,bool wifi){
 const char* e=error?error:"";
 if(!strcmp(e,"Reconnecting Wi-Fi"))return Kind::Reconnecting;
 if(!strcmp(e,"WiFi not connected"))return Kind::Offline;
 if(strstr(e,"Mic"))return Kind::Mic;
 if(strstr(e,"No audio"))return Kind::Empty;
 if(strstr(e,"memory")||strstr(e,"PSRAM")||strstr(e,"System busy"))return Kind::Busy;
 if(strstr(e,"401")||strstr(e,"403")||strstr(e,"Unauthorized")||strstr(e,"unauthorized"))return Kind::Auth;
 if(!wifi)return Kind::Lost;
 if(strstr(e,"overflow"))return Kind::Overflow;
 if(strstr(e,"timed out")||strstr(e,"timeout"))return Kind::Timeout;
 return Kind::Service;
}
inline Kind connectionTick(Kind k,bool wifi,uint32_t age){
 if(!connectionNotice(k))return k;
 if(wifi)return Kind::Ready;
 if(k==Kind::Ready)return Kind::Offline;
 if(k==Kind::Reconnecting&&age>=kReconnectMs)return Kind::Offline;
 return k;
}
// Ignore releases from the gesture that produced the notice. Actions require
// a new tap/chord after both buttons have been released at least once.
struct InputGate {
 bool armed=false;
 void reset(){armed=false;}
 void sample(bool a,bool b){if(!a&&!b)armed=true;}
};
}}}
