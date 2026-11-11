#pragma once
#include <stdint.h>
namespace games { namespace dungeon {
struct World {
 static constexpr int W=11,H=7;
 bool wall[W*H]={},gem[W*H]={};float x=1.5f,y=1.5f;int collected=0;bool won=false;
 void generate(uint32_t seed){
  *this=World{};for(auto& b:wall)b=true;
  int stack[15],top=0;stack[0]=W+1;wall[W+1]=false;
  while(top>=0){int c=stack[top],options[4],n=0;const int offsets[]={-2,2,-2*W,2*W};
   for(int d:offsets){int next=c+d;if(next/W>0&&next/W<H-1&&next%W>0&&next%W<W-1&&wall[next])options[n++]=next;}
   if(!n){--top;continue;}seed=seed*1664525u+1013904223u;int next=options[seed%n];wall[(c+next)/2]=wall[next]=false;stack[++top]=next;
  }
  gem[W+9]=gem[3*W+5]=gem[5*W+1]=true;
 }
 // Bright regions become open rooms; a guaranteed connected spine keeps even
 // a completely dark photograph playable. No heap or input-dependent recursion.
 void fromPhoto(const uint8_t* light){
  *this=World{};
  for(int y=0;y<H;++y)for(int x=0;x<W;++x)wall[y*W+x]=x==0||y==0||x==W-1||y==H-1||light[y*W+x]<128;
  for(int x=1;x<W-1;++x)wall[W+x]=wall[5*W+x]=false;
  for(int y=1;y<H-1;++y)wall[y*W+5]=wall[y*W+1]=false;
  gem[W+9]=gem[3*W+5]=gem[5*W+1]=true;
 }
 bool free(float px,float py)const{
  constexpr float r=.18f;
  for(int a=-1;a<=1;a+=2)for(int b=-1;b<=1;b+=2){int xx=int(px+a*r),yy=int(py+b*r);if(xx<0||yy<0||xx>=W||yy>=H||wall[yy*W+xx])return false;}
  return true;
 }
 void step(float ax,float ay,float dt){
  if(won)return;if(ax>1)ax=1;if(ax< -1)ax=-1;if(ay>1)ay=1;if(ay< -1)ay=-1;
  float nx=x+ax*2.4f*dt,ny=y+ay*2.4f*dt;if(free(nx,y))x=nx;if(free(x,ny))y=ny;
  int cell=int(y)*W+int(x);if(gem[cell]){gem[cell]=false;++collected;}
  won=collected==3&&int(x)==9&&int(y)==5;
 }
};
}}
