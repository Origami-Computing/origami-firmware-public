#pragma once
#include <stdint.h>
namespace apps { namespace onboarding {
enum class Input { A, B, C, Rotate };
enum class WifiResult { None, Back, Skipped, Failed, Joined };
// Stable IDs retain the existing camera/menu/service handoff contracts.
struct Flow {
 static constexpr unsigned kSelect=36,kInterest=37,kAccount=38,kWifiChoice=39,kLogin=40,kWifiWebsite=41,kProjects=42,kSending=43,kFinished=44,kBuildIntro=45,kBuildSetup=46,kBuildConnect=47,kBuildBrowse=48,kBuildDone=49;
 unsigned page=0,origin=kProjects,selection=0,interest=0;
 bool account=false,joinWifi=false,practiced=false,wifiOnly=false,connected=false,setupPublished=false,installed=false;
 void reset(){*this=Flow{};}
 void beginWifiSetup(){reset();wifiOnly=true;page=kWifiWebsite;}
 void setPage(unsigned next){page=next;selection=0;}
 unsigned afterNav()const{return interest==1?125:kBuildIntro;}
 unsigned afterPractice()const{return account&&connected?25:122;}
 bool turn(int delta){
  if(!delta)return false;
  if(page==18)return input(Input::Rotate);
  if(page!=kSelect&&page!=kInterest&&page!=kAccount&&page!=kWifiChoice&&page!=126&&page!=131)return false;
  const int max=page==kInterest?3:1,next=int(selection)+(delta>0?1:-1);
  selection=unsigned(next<0?0:next>max?max:next);return true;
 }
 bool input(Input i,bool ready=true){
  if(!ready)return false;const unsigned before=page;
  if(page==0&&i!=Input::Rotate){reset();setPage(3);}
  else if(page==3&&i==Input::A)setPage(4);
  else if(page==4&&i==Input::B)setPage(6);
  else if(page==6&&i==Input::Rotate)setPage(5);
  else if(page==5&&i==Input::C)setPage(kSelect);
  else switch(page){
   case kSelect:if(i==Input::C&&selection==1)setPage(kInterest);break;
   case kInterest:if(i==Input::C){if(selection==3){reset();break;}interest=selection;account=false;connected=false;joinWifi=false;setupPublished=false;installed=false;setPage(interest?141:kWifiChoice);}break;
   case kAccount:if(i==Input::A)setPage(kInterest);else if(i==Input::C){setPage(selection? kWifiChoice:141);}break;
   case 141:if(i==Input::A)setPage(kInterest);else if(i==Input::B)setPage(kLogin);break;
   case kWifiChoice:if(i==Input::A)setPage(kInterest);else if(i==Input::C){joinWifi=selection==0;origin=kWifiChoice;setPage(joinWifi?kWifiWebsite:120);}break;
   case kLogin:if(i==Input::A)setPage(setupPublished?126:141);else if(i==Input::B){origin=kLogin;setPage(setupPublished&&connected&&account?127:12);}break;
   case kWifiWebsite:if(i==Input::A)setPage(wifiOnly?kFinished:setupPublished?126:kWifiChoice);else if(i==Input::B)setPage(12);break;
   case kBuildIntro:if(i==Input::A)setPage(140);else if(i==Input::B)setPage(kBuildConnect);break;
   case kProjects:if(i==Input::A)setPage(kBuildBrowse);else if(i==Input::B)setPage(kBuildSetup);break;
   case kBuildConnect:if(i==Input::A)setPage(kBuildIntro);else if(i==Input::B)setPage(kBuildBrowse);break;
   case kBuildSetup:if(i==Input::A)setPage(kProjects);else if(i==Input::B)setPage(kBuildDone);break;
   case kBuildBrowse:if(i==Input::A)setPage(kBuildConnect);else if(i==Input::B)setPage(kProjects);break;
   case kBuildDone:if(i==Input::A)setPage(kBuildSetup);else if(i==Input::B)setPage(125);break;
   case 120:if(i==Input::B)setPage(18);else if(i==Input::A)setPage(interest?31:joinWifi?kWifiWebsite:kWifiChoice);break;
   case 18:if(i==Input::Rotate){practiced=false;setPage(19);}else if(i==Input::A)setPage(120);break;
   case 31:if(i==Input::A)setPage(30);else if(i==Input::B)setPage(kSending);break;
   case 21:if(i==Input::B)setPage(22);break;
   case 22:if(i==Input::B)setPage(23);else if(i==Input::A)setPage(21);break;
   case 25:if(i==Input::A)setPage(18);break;
   case 122:if(i==Input::B)setPage(123);break;
   case 123:if(i==Input::A)setPage(122);break;
   case 124:if(i==Input::A)setPage(afterPractice());else if(i==Input::B)setPage(140);break;
   case 140:if(i==Input::B)setPage(afterNav());else if(i==Input::A)setPage(afterPractice());break;
   case 125:if(i==Input::B)setPage(connected?127:126);else if(i==Input::A)setPage(interest==1?140:kBuildDone);break;
   case 126:if(i==Input::A)setPage(125);else if(i==Input::C){if(selection)setPage(135);else{setupPublished=true;setPage(kWifiWebsite);}}break;
   case 135:if(i==Input::A)setPage(126);else if(i==Input::B)setPage(133);break;
   case 127:if(i==Input::A)setPage(125);else if(i==Input::B)setPage(128);break;
   case 128:if(i==Input::A)setPage(127);else if(i==Input::B)setPage(136);break;
   case 136:if(i==Input::A)setPage(128);else if(i==Input::B)setPage(129);break;
   case 133:if(i==Input::A)setPage(installed?132:connected?129:135);else if(i==Input::B)setPage(134);break;
   case 134:if(i==Input::A)setPage(133);else if(i==Input::B)setPage(21);else if(i==Input::C)setPage(connected?128:126);break;
   case 132:if(i==Input::B)setPage(133);break;
  }
  return page!=before;
 }
 void wifiReturned(WifiResult r){
  if(r==WifiResult::Joined){connected=true;joinWifi=true;origin=kWifiWebsite;setPage(wifiOnly?kFinished:setupPublished?127:interest?30:120);}
  else setPage(setupPublished?kWifiWebsite:interest?kLogin:kWifiWebsite);
 }
 void photoDelivered(){origin=kLogin;setPage(120);}
 void menuReturned(){setPage(practiced?afterPractice():18);}
 bool beginTalk(){if(page==123){setPage(124);return false;}if(page!=25||!account||!connected)return false;setPage(27);return true;}
 void agentReturned(){setPage(140);}
 void outroFinished(){setPage(23);}
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
