#pragma once
#include <stdint.h>
namespace games { namespace mines {
struct Board {
    bool bomb[25]={},open[25]={},flag[25]={},seeded=false,lost=false;int opened=0;
    int neighbors(int i)const{int n=0;for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){int a=i%5+x,b=i/5+y;if(a>=0&&a<5&&b>=0&&b<5&&bomb[b*5+a])++n;}return n;}
    void seed(int safe,uint32_t random){int slots[24],n=0;for(int i=0;i<25;++i)if(i!=safe)slots[n++]=i;for(int i=0;i<5;++i){random=random*1664525u+1013904223u;int pick=i+random%(24-i);int swap=slots[i];slots[i]=slots[pick];slots[pick]=swap;bomb[slots[i]]=true;}seeded=true;}
    bool won()const{return opened==20&&!lost;}
    void reveal(int i,uint32_t random){
        if(i<0||i>=25||flag[i]||open[i]||lost||won())return;
        if(!seeded)seed(i,random);
        if(bomb[i]){lost=true;return;}
        int queue[25],head=0,tail=0;queue[tail++]=i;open[i]=true;++opened;
        while(head<tail){int cell=queue[head++];if(neighbors(cell))continue;
            for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){int a=cell%5+x,b=cell/5+y;if(a<0||a>=5||b<0||b>=5)continue;int next=b*5+a;if(!open[next]&&!flag[next]&&!bomb[next]){open[next]=true;++opened;queue[tail++]=next;}}
        }
    }
    void toggle(int i){if(i>=0&&i<25&&!open[i]&&!lost&&!won())flag[i]=!flag[i];}
};
}}
