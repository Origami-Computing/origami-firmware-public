#pragma once
#include <stdint.h>
namespace games { namespace tiles {
struct Board {
    uint8_t cells[16]={};uint32_t score=0;
    // 0 left, 1 up, 2 right, 3 down. Merge each pair only once per move.
    bool move(int direction){
        bool changed=false;
        for(int line=0;line<4;++line){
            int index[4];uint8_t packed[4]={};int count=0;
            for(int i=0;i<4;++i){index[i]=direction==0?line*4+i:direction==1?i*4+line:direction==2?line*4+3-i:(3-i)*4+line;if(cells[index[i]])packed[count++]=cells[index[i]];}
            uint8_t merged[4]={};int out=0;
            for(int i=0;i<count;++i){uint8_t value=packed[i];if(i+1<count&&value==packed[i+1]&&value<17){++value;++i;uint32_t points=1u<<value;score=UINT32_MAX-score<points?UINT32_MAX:score+points;}merged[out++]=value;}
            for(int i=0;i<4;++i){changed|=cells[index[i]]!=merged[i];cells[index[i]]=merged[i];}
        }return changed;
    }
    bool spawn(uint32_t random){int slots[16],n=0;for(int i=0;i<16;++i)if(!cells[i])slots[n++]=i;if(!n)return false;cells[slots[random%n]]=(random/16)%10==0?2:1;return true;}
    bool playable()const{for(int i=0;i<16;++i){if(!cells[i])return true;if(cells[i]<17&&((i%4<3&&cells[i]==cells[i+1])||(i<12&&cells[i]==cells[i+4])))return true;}return false;}
};
}}
