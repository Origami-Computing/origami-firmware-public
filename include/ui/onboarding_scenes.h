#pragma once
#include "apps/onboarding_demo.h"
#include "ui/onboarding_flow_view.h"
#include "ui/onboarding_connect_view.h"
#include "ui/qr_scanner_view.h"
#include "ui/list_view.h"
#include "ui/scene.h"
#include "ui/onboarding_menu_scene.h"
#include "assets/menu_icons.h"
#include "ui/agent_transition_motion.h"
#include "ui/listening_styles_generated.h"
// Studio adapters call the production painters. No HTML redraw of device UI.
namespace ui { namespace onboarding_scenes {
template<class C> void backPrompt(C& c,bool demo=false){
 ui::onboarding_approved::menuInstruction(c,true);
 c.fillRect(66,172,148,20,0xffff);c.fillRect(30,196,220,22,0xffff);
 ui::design::center(c,demo?"Hold A+B":"A+B",175);
 ui::design::center(c,demo?"to see a demo":"Continue back to home",199,16);
}
// Smooth coverage circles and strokes at the native framebuffer resolution.
template<class C> void circle(C& c,float cx,float cy,float r,uint16_t ink,uint16_t bg){
 for(int y=int(cy-r)-1;y<=int(cy+r)+1;++y)for(int x=int(cx-r)-1;x<=int(cx+r)+1;++x){
  int n=0;for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){float dx=x+(sx+.5f)/4-cx,dy=y+(sy+.5f)/4-cy;if(dx*dx+dy*dy<=r*r)++n;}
  if(n)c.fillRect(x,y,1,1,ui::proportional::blend(ink,bg,n*255/16));
 }
}
template<class C> void loading(C& c,unsigned ms,int cy=86){
 for(int i=0;i<3;++i){float phase=ms*6.2831853f/1200-i*.9f;circle(c,124+i*16,cy-4*std::max(0.f,std::sin(phase)),3.4f,0x2104,0xffff);}
}
template<class C> void scannerLoading(C& canvas,unsigned elapsed){
 canvas.fillRect(0,0,280,240,0xffff);
 loading(canvas,elapsed);
 ui::design::center(canvas,"Loading",124,24);
}
template<class C> void customMenu(C& c,bool installed,bool more,const char* const* names=nullptr,unsigned count=0){
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);ui::design::center(c,"Custom Apps",16);
 struct Rows {const char* const* names;unsigned count;};
 const char* example[]={"Recipe Timer"};if(!names){names=example;count=installed?1:0;}Rows rows{names,count};
 ui::ListView list;list.totalItems=count+1;list.selected=more?list.totalItems-1:0;list.dialogueFrame=false;
 list.rowH=30;list.rowPitch=34;list.topPad=0;list.leftX=24;
 auto item=[](int i,void* ctx){auto& rows=*static_cast<Rows*>(ctx);ui::ListItem v;v.label=unsigned(i)<rows.count?rows.names[i]:"App Downloader";v.icon=assets::menu_icons::kTools;v.fg=kUiFg;return v;};
#ifdef ESP32
 auto& scene=ui::scene::scratchScene();scene.clear();
#else
 ui::scene::Scene scene;
#endif
 list.buildScene(scene,0,48,280,108,item,&rows,kUiSelBg,kUiSelFg,kUiBg,kUiMuted,kUiFg);ui::onboarding_menu::render(c,scene);
 ui::pixel_dialogue::draw(c,{24,164,232,60},0,0xffff);
 if(more){ui::design::center(c,"Download more apps by",170,16);ui::design::center(c,"visiting the App Downloader",187,16);ui::design::center(c,"in settings",204,16);}
 else{ui::design::center(c,"Installed apps appear",179,16);ui::design::center(c,"here in navigation",201,16);}
}
template<class C> void listeningBorder(C& c,unsigned ms){
 const auto& v=ui::listening_style::kStyles[0];
 float level=.3f+.25f*std::sin(ms*.006f)+.12f*std::sin(ms*.019f),width=v.minimum+(v.maximum-v.minimum)*level;
 float low=std::min(0.f,v.ax)+std::min(0.f,v.ay),span=std::abs(v.ax)+std::abs(v.ay);
 for(int y=0;y<240;++y)for(int x=0;x<280;++x){
  float qx=std::abs(x+.5f-141)-99,qy=std::abs(y+.5f-122)-78;
  float distance=std::max(0.f,-(std::hypot(std::max(qx,0.f),std::max(qy,0.f))+std::min(std::max(qx,qy),0.f)-39));
  float alpha=v.opacity*(1-ui::agent_motion::ease((distance-width*v.solid)/(width*(1-v.solid))));
  float t=ui::agent_motion::ease((v.ax*(x+.5f)/280+v.ay*(y+.5f)/240-low)/span);
  int r=std::lround(v.colors[0][0]+(v.colors[1][0]-v.colors[0][0])*t),g=std::lround(v.colors[0][1]+(v.colors[1][1]-v.colors[0][1])*t),b=std::lround(v.colors[0][2]+(v.colors[1][2]-v.colors[0][2])*t);
  c.fillRect(x,y,1,1,ui::proportional::blend(uint16_t((r>>3)<<11|(g>>2)<<5|(b>>3)),0xffff,uint8_t(alpha*255)));
 }
}
template<class C> void render(C& c,unsigned page,unsigned ms,unsigned selection,const char* detail){
#ifdef SCREEN_STUDIO_HOST
 if(page==129 && !strcmp(detail,"scanner-startup") && ms<3000){scannerLoading(c,ms);return;}
#endif
 if(page==0)ui::onboarding_flow::welcome(c,ms);
 else if(page>=3&&page<=6){float angle=0;int held=0,success=0;sscanf(detail,"%f,%d,%d",&angle,&held,&success);ui::onboarding_flow::control(c,page,ms,angle,held,success);}
 else if(page==36)ui::onboarding_flow::selection(c,selection);
 else if(page>=37&&page<=39)ui::onboarding_flow::menu(c,page,selection);
#ifdef STUDIO_QR_VARIANTS
 else if(page>=40&&page<=42)ui::onboarding_flow::qr(c,page,selection==0);
#else
 else if(page>=40&&page<=42)ui::onboarding_flow::qr(c,page);
#endif
#ifdef STUDIO_KAMI_NOD
 else if(page>=45&&page<=49)ui::onboarding_flow::build(c,page,ms,selection==1);
 else if(page==21||page==22)ui::onboarding_flow::speaking(c,page,ms);
#else
 else if(page>=45&&page<=49)ui::onboarding_flow::build(c,page);
#endif

#ifdef SCREEN_STUDIO_HOST
 else if(page==121){
 c.fillRect(0,0,280,240,kUiBg);onboarding_navigation::chrome(c,true);
 navigation_existing::s_page=0;navigation_existing::s_selection[0]=selection;
 navigation_existing::lesson=true;navigation_existing::practiced=true;
 ui::scene::Scene scene;navigation_existing::buildMenuScene(scene,280,240);scene.render(c);
 backPrompt(c);
 }
 else if(page==137||page==139){agent_transition_preview::render(c,ms,page==137?2:3,0);}
 else if(page==138){agent_transition_preview::render(c,800,3,0);c.fillRect(112,120,60,32,0xffff);loading(c,ms,136);}

#endif
 else if(page==140){c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);ui::onboarding_flow::nod(c,110,36,ms);
 ui::design::center(c,"With navigation complete",132);ui::design::center(c,"lets move on",156);}
 else if(page==126){c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);ui::onboarding_flow::nod(c,110,24,ms);
 ui::design::center(c,"Public apps only need Wi-Fi.",104,18);ui::design::center(c,"Set up now?",128,18);
 const char* labels[]={"Set up Wi-Fi","Skip for now"};const uint8_t* icons[]={assets::menu_icons::kWifi,ui::onboarding_proposals::kSkip};
 ui::onboarding_proposals::rows(c,labels,icons,2,selection,148,36);
 }
 else if(page==135||page==136){
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);ui::onboarding_flow::nod(c,110,36,ms);
 if(page==135){ui::design::center(c,"That's okay, we can",132);ui::design::center(c,"set this up later.",156);}
 else{ui::design::center(c,"Scan a QR code on the",120);ui::design::center(c,"Published Apps website",144);ui::design::center(c,"to download an app.",168);}
 }
 else if(page==133||page==134){customMenu(c,selection!=0,page==134);}
 else if(page==124){
 using namespace ui::onboarding_flow;
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);
 if(ms<2100){
  listeningBorder(c,ms);onboarding_wifi::arrows(c,true,true);
  ui::design::center(c,"Listening...",26);
  for(int i=0;i<19;++i){int h=6+int(26*(1+std::sin(ms*.011+i*.8))*.5);c.fillRect(65+i*8,86-h/2,3,h,0xf945);}
  const auto speech=apps::onboarding::DemoSpeech::at(ms);
  const std::string words(speech.text,speech.count);
  const char* remaining=words.c_str();for(int line=0;*remaining&&line<3;++line){auto wrap=ui::proportional::line(remaining,232,ui::design::standard);std::string text(remaining,wrap.bytes);ui::design::center(c,text.c_str(),136+line*24);if(!wrap.next)break;remaining+=wrap.next;}
 }else if(ms<3100){
  nod(c,110,42,ms);ui::design::center(c,"Thinking",138);
  for(int i=0;i<3;++i)ui::pixel_dialogue::fillStepped(c,{120+i*16,173,7,7},2,((ms/300)%3==unsigned(i))?0:0xc618);
 }else{
  nod(c,110,32,ms);
  const auto speech=apps::onboarding::DemoSpeech::at(ms);
  const unsigned lengths[]={22,25,23};unsigned offset=0;
  for(unsigned line=0;line<3;++line){unsigned n=speech.count>offset?std::min(lengths[line],speech.count-offset):0;
   const std::string text(speech.text+offset,n);ui::design::center(c,text.c_str(),120+line*24);offset+=lengths[line];}
 }
 }
 else if(page>=120&&page<=134){
  using namespace ui::onboarding_flow;
  c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);
  if(page==128){qr(c,page,selection==0);}
  else if(page==129){c.fillRect(0,0,280,240,0);qr_scanner_view::overlay(c);c.fillRect(0,0,280,35,0xffff);ui::design::center(c,"Hold steady over app QR",14,18);c.fillRect(0,194,280,46,0xffff);ui::design::choice(c,"A back",24,194,112,30,false);ui::design::choice(c,"B skip",144,194,112,30,false);}
  else if(page==121||page==133||page==134){
   ui::design::center(c,page==121?"Menu":"Custom Apps",18);
   if(page==121){pill(c,56,"Camera",true);pill(c,96,"Photos",false);ui::design::instruction(c,"Continue back to Home",24,160,232,32);ui::design::center(c,"Press A+B together",202);}
   else{if(selection)pill(c,56,"Recipe Timer",false);pill(c,102,"App Downloader",page==134);ui::design::center(c,page==134?"Install more apps here":"Installed apps appear here",160,18);ui::design::center(c,page==134?"C: Open   B: Continue":"B: Continue",192,18);}
  }else if(page==131){wifiError(c,"App could not install",detail&&detail[0]?detail:"Check the app QR and connection.",false,selection);
 c.fillRect(0,148,280,92,0xffff);const char* labels[]={"Try again","Skip"};const uint8_t* icons[]={ui::onboarding_proposals::kRestart,ui::onboarding_proposals::kSkip};ui::onboarding_proposals::rows(c,labels,icons,2,selection,148,36);
 }
  else if(page==130||page==132){ui::design::center(c,page==130?"Installing app...":"App installed!",92,24);ui::design::center(c,detail&&detail[0]?detail:"Recipe Timer",134);if(page==132)ui::design::center(c,"B: Continue",190);}
  else if(page==123){nod(c,110,48,ms,60);backPrompt(c,true);}
  else{
   const int kamiX=page==127?140+(ui::proportional::width("Follow this link to",ui::design::standard)+1)/2-60:page==120?180:110;
   nod(c,kamiX,42,ms,60);
   if(page==120||page==127){
    struct HandPosition {C& c;int dx;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x+dx,y,w,h,ink);}} positionedHand{c,kamiX-180};
    hand(positionedHand,ms);
   }
   const char* l1=page==120?"Let's get started with":page==122?"You'll need an account":page==124?"Try a quick rice bowl.":page==125?"You can also download":page==126?"You need Wi-Fi and":"Follow this link to";
   const char* l2=page==120?"a tour of my navigation.":page==122?"to speak with me.":page==124?"Cook rice, add vegetables":page==125?"apps published on the web.":page==126?"an account. Set up now?":"view public apps.";
   ui::design::center(c,l1,120);ui::design::center(c,l2,144);
   if(page==122)ui::design::center(c,"Let's start with a demo.",172);
   if(page==124)ui::design::center(c,"and a fried egg. Enjoy!",168);
   if(page==126){ui::design::choice(c,"Set up",24,184,112,36,selection==0);ui::design::choice(c,"Skip",144,184,112,36,selection==1);}
  }
 }
 else if(page==141){
 c.fillRect(0,0,280,240,kUiBg);onboarding_wifi::arrows(c,true,true);
 ui::onboarding_flow::nod(c,110,36,ms,60);
 ui::design::center(c,"To get started, please",120);
 ui::design::center(c,"follow the link on the",144);
 ui::design::center(c,"next page to create",168);
 ui::design::center(c,"an account",192);
 }
 else if(page==43){c.fillRect(0,0,280,240,kUiBg);loading(c,ms);ui::design::center(c,"Sending photo...",122,24);ui::design::center(c,"Waiting for web upload",160,18);}
 else if(page==100)ui::onboarding_connect::listening(c,false,0,0,"",ms);
 else if(page==101)ui::onboarding_connect::listening(c,true,selection+1,4,"",ms);
 else if(page==102)ui::onboarding_connect::listening(c,false,0,0,detail,ms);
 else if(page==103){c.fillRect(0,0,280,240,0);qr_scanner_view::overlay(c);}
 else if(page==150){ui::design::instruction(c,"Press B to take photo");}
 else if(page==151){ui::design::actions(c,"A retake","B send");}
 else if(page==113){c.fillRect(0,160,280,80,kUiBg);backPrompt(c);}
 else if(page==112){c.fillRect(0,160,280,80,kUiBg);ui::onboarding_approved::menuInstruction(c,true);}

#ifdef SCREEN_STUDIO_HOST
 else if(page==110){
  c.fillRect(0,0,280,240,kUiBg);
  onboarding_navigation::chrome(c,true);
  navigation_existing::s_page=0;navigation_existing::s_selection[0]=selection;
  navigation_existing::lesson=true;navigation_existing::practiced=false;
  ui::scene::Scene scene;navigation_existing::buildMenuScene(scene,280,240);scene.render(c);
  ui::onboarding_approved::menuInstruction(c,false);
 }

#endif
 else if(page==104){const std::string message(detail);const auto split=message.find('\n');ui::onboarding_flow::wifiError(c,message.substr(0,split).c_str(),split==std::string::npos?"":message.c_str()+split+1,false,selection);}
 else if(page==105||page==106){
  const uint16_t bg=page==106?kUiSuccessBg:0xffff;c.fillRect(0,0,280,240,bg);onboarding_wifi::arrows(c,true,page==106);
  if(page==105)loading(c,ms);
  else{
   circle(c,140,86,23,0,bg);circle(c,140,86,21.5f,bg,0);
   for(int y=68;y<104;++y)for(int x=123;x<157;++x){
    float d=100;for(int leg=0;leg<2;++leg){float ax=leg?136:128,ay=leg?95:86,bx=leg?153:136,by=leg?76:95;
     float vx=bx-ax,vy=by-ay,t=std::max(0.f,std::min(1.f,((x+.5f-ax)*vx+(y+.5f-ay)*vy)/(vx*vx+vy*vy)));
     d=std::min(d,std::hypot(x+.5f-ax-t*vx,y+.5f-ay-t*vy));}
    float a=std::max(0.f,std::min(1.f,2.1f-d));if(a)c.fillRect(x,y,1,1,ui::proportional::blend(0,bg,unsigned(a*255)));
   }
  }
  ui::design::center(c,page==105?"Connecting...":"Connected",124,24,0,bg);
  ui::design::center(c,detail,160,18,0,bg);
 }
 else if(page==18){c.fillRect(0,0,280,240,kUiBg);
 // Preserve the canonical eye silhouette, with subpixel gaze and a soft blink.
 struct EyeMask {
  unsigned char* pixels;
  void fillRect(int x,int y,int w,int h,uint16_t ink){
   const unsigned char alpha=255-((ink>>11)&31)*255/31;
   for(int row=std::max(0,y-36);row<std::min(130,y-36+h);++row)
    for(int col=std::max(0,x-45);col<std::min(190,x-45+w);++col)pixels[row*190+col]=alpha;
  }
  float sample(int x,int y)const{return x>=0&&x<190&&y>=0&&y<130?pixels[y*190+x]:0;}
 };auto* mask=static_cast<unsigned char*>(ui::onboarding_flow::rasterScratch());if(!mask)return;std::fill(mask,mask+190*130,0);EyeMask eyes{mask};
 ui::onboarding_baked::eyes(eyes,0);
 auto ease=[](float t){t=std::max(0.f,std::min(1.f,t));return t*t*t*(t*(t*6-15)+10);};
 const float positions[][2]={{0,-6},{12,-11},{-13,-4},{-8,-12},{11,-3},{0,-6},{0,-6}};
 const unsigned time=ms%12000,step=time/2000;
 const float travel=ease((time%2000)/1350.f);
 const float dx=positions[step][0]+(positions[step+1][0]-positions[step][0])*travel;
 const float dy=positions[step][1]+(positions[step+1][1]-positions[step][1])*travel;
 float close=0;
 for(unsigned start:{2800u,7200u,10800u})if(time>=start&&time<start+360){
  unsigned t=time-start;close=t<110?ease(t/110.f):t<150?1:1-ease((t-150)/210.f);
 }
 const float scale=1-.96f*close;
 for(int y=26;y<162;++y){
  int run=24;uint16_t last=0xffff;
  for(int x=24;x<=252;++x){
   uint16_t color=0xffff;
   if(x<252){
    float sx=x-dx-45,sy=(y-dy-96)/scale+96-36;
    int ix=int(std::floor(sx)),iy=int(std::floor(sy));float fx=sx-ix,fy=sy-iy;
    float alpha=(eyes.sample(ix,iy)*(1-fx)+eyes.sample(ix+1,iy)*fx)*(1-fy)+(eyes.sample(ix,iy+1)*(1-fx)+eyes.sample(ix+1,iy+1)*fx)*fy;
    color=ui::proportional::blend(0,kUiBg,unsigned(alpha));
   }
   if(color!=last||x==252){if(last!=kUiBg)c.fillRect(run,y,x-run,1,last);run=x;last=color;}
  }
 }
 // Keep the half-wheel in the clear strip right of both eyes and dialogue.
 struct WheelEdge {C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){
  x+=42;y-=26;const int right=std::min(280,x+w),bottom=std::min(164,y+h);
  if(x<right&&y>=0&&y<bottom)c.fillRect(x,y,right-x,bottom-y,ink);
 }} edge{c};
 ui::onboarding_flow::wheel(edge,0,false,kUiBg);
 ui::pixel_dialogue::draw(c,{24,164,232,60},0,0xffff);ui::onboarding_approved::wheelText(c,120000);}
 else if(page==25)ui::onboarding_approved::navigation(c,25,120000,selection);
 else if(page==21||page==22){
  ui::onboarding_approved::background(c,page,120000,88,120,page==22,true);
  c.fillRect(34,132,212,80,kUiBg);
  ui::onboarding_approved::draftText(c,ui::onboarding_approved::wrap(ui::onboarding_approved::copy(page)),10000,selection,120,true);
 }
 else {c.fillRect(0,0,280,240,kUiBg);}
}
}

}
