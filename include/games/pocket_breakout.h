#pragma once
#include <stdint.h>
namespace games { namespace breakout {
struct Game {
 float x=140,y=196,vx=75,vy=-105,paddle=116;bool bricks[24]={};int lives=3,score=0;bool running=false,won=false;
 void reset(){*this=Game{};for(auto& b:bricks)b=true;}
 void wheel(int delta){paddle+=delta*10.f;if(paddle<8)paddle=8;if(paddle>224)paddle=224;if(!running){x=paddle+24;y=196;}}
 void step(float dt){
  if(!running)return;x+=vx*dt;y+=vy*dt;
  if(x<12){x=12;vx=-vx;}if(x>268){x=268;vx=-vx;}if(y<68){y=68;vy=-vy;}
  if(vy>0&&y>=196&&y<=204&&x>=paddle-3&&x<=paddle+51){y=196;vy=-105;vx=(x-paddle-24)*4;}
  for(int i=0;i<24;++i)if(bricks[i]){int bx=12+(i%6)*44,by=76+(i/6)*20;if(x>=bx-3&&x<=bx+43&&y>=by-3&&y<=by+19){bricks[i]=false;++score;vy=-vy;break;}}
  if(score==24){won=true;running=false;}
  if(y>216){--lives;running=false;x=paddle+24;y=196;vx=75;vy=-105;}
 }
};
}}
