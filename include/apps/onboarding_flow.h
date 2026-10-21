#pragma once
#include <stdint.h>
namespace apps { namespace onboarding {
enum class Input { A, B, C, Rotate };
enum class WifiResult { None, Back, Skipped, Failed, Joined };
struct Flow {
 unsigned page=0, origin=11;
 static constexpr unsigned kFinished=29;
 bool practiced=false, wifiOnly=false;
 void reset(){page=0;origin=11;practiced=false;wifiOnly=false;}
 void beginWifiSetup(){reset();wifiOnly=true;page=8;}
 bool input(Input i, bool ready=true){
  const unsigned before=page;
  if(!ready)return false;
  if(wifiOnly && page==8 && i==Input::A){page=kFinished;return true;}
  if(page<3 && i!=Input::Rotate)++page;
  else if((page==3&&i==Input::A)||(page==4&&i==Input::B)||(page==5&&i==Input::C)||(page==6&&i==Input::Rotate))++page;
  else switch(page){
   case 7: if(i==Input::A)page=6;else if(i==Input::B)page=8;break;
   case 8: if(i==Input::A)page=7;else if(i==Input::B)page=9;break;
   case 9: if(i==Input::A)page=8;else if(i==Input::B)page=12;break;
   case 28:if(i==Input::A)page=9;else if(i==Input::B)page=10;break;
   case 10:if(i==Input::A)page=9;else if(i==Input::B)page=11;break;
   case 11:if(i==Input::A)page=10;else if(i==Input::B){origin=11;page=17;}break;
   case 14:if(i==Input::B){origin=14;page=17;}break;
   case 16:if(i==Input::A)page=12;else if(i==Input::B){origin=16;page=17;}break;
   case 17:if(i==Input::A){page=origin;}else if(i==Input::B)page=18;break;
   case 18:if(i==Input::Rotate){page=19;practiced=false;}break;
   case 21:if(i==Input::B)page=22;break;
   case 22:if(i==Input::B)page=23;break;
   case 24:if(i==Input::B)page=25;break;
   case 25:if(i==Input::A)page=24;break;
  }
  if(wifiOnly && page==17)page=kFinished;
  return before!=page;
 }
 void wifiReturned(WifiResult r){
  switch(r){
   case WifiResult::Skipped:page=10;break;
   case WifiResult::Failed:page=16;break;
   case WifiResult::Joined:page=17;origin=14;break;
   default:page=9;break;
  }
  if(wifiOnly && page==17)page=kFinished;
 }
 void menuReturned(){page=practiced?24:18;}
 bool beginTalk(){if(page!=25)return false;page=27;return true;}
 void agentReturned(){if(page==27)page=21;}
};
// A reveal tap only reveals text; it cannot also advance to another page.
struct TextPlayback {
 unsigned offset=0,scroll=999;
 void reset(){offset=0;scroll=999;}
 bool reveal(unsigned elapsed,unsigned starts,unsigned ready){
  if(elapsed<starts||elapsed+offset>=ready)return false;
  offset=ready-elapsed;return true;
 }
 unsigned follow(const char* text,unsigned count,unsigned rows=4) const {
  unsigned line=0;for(;*text&&count;++text,--count)if(*text=='\n')++line;
  return line>=rows?line-rows+1:0;
 }
 bool turn(int delta,unsigned lines,unsigned rows=4,unsigned automatic=0){
  if(lines<=rows||!delta)return false;
  const int next=int(scroll==999?automatic:scroll)+delta,limit=int(lines-rows);
  scroll=unsigned(next<0?0:next>limit?limit:next);return true;
 }
};
}} // namespace apps::onboarding
