#include "tooling/custom_app_commands.h"
#include "widgets/storage.h"
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <new>

namespace tooling {
namespace {
namespace apps=services::custom_apps;
namespace jobs=widgets::storage;
apps::Package input,outputPackage;
apps::Catalog* outputCatalog=nullptr;
uint32_t token=0,job=0,lastActivity=0;
size_t received=0,expected=0;
apps::Identity target;
apps::Result result=apps::Result::Ok;
bool receiving=false,complete=false,list=false;
void reset(){input.reset();outputPackage.reset();heap_caps_free(outputCatalog);outputCatalog=nullptr;received=expected=0;receiving=complete=list=false;job=0;}
int digit(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
bool medium(const char* text,apps::Medium& m){if(!strcmp(text,"sd")){m=apps::Medium::Sd;return true;}if(!strcmp(text,"flash")){m=apps::Medium::Flash;return true;}return false;}
void state(Print& out){out.printf("APPS STATUS %08lx %s %s %u\n",(unsigned long)token,
    receiving?"receiving":job?"pending":complete?"complete":"idle",apps::resultName(result),unsigned(receiving?received:outputPackage.size));}
void poll(){if(job && jobs::take(job,result,&outputPackage,outputCatalog)){job=0;complete=true;input.reset();}}
}
void serviceCustomAppCommands(uint32_t now){poll();if(receiving&&int32_t(now-lastActivity)>30000){input.reset();receiving=false;complete=true;result=apps::Result::Cancelled;}}
bool handleCustomAppCommand(const char* line,Print& out){
    if(strncmp(line,"apps ",5))return false;line+=5;poll();
    char action[16]={},arg[96]={},extra[256]={};unsigned long supplied=0,offset=0;int consumed=0;
    if(sscanf(line,"%15s %lx %n",action,&supplied,&consumed)<2 || supplied>UINT32_MAX || !supplied){out.println("APPS ERR usage");return true;}
    const uint32_t request=uint32_t(supplied);const char* rest=line+consumed;
    if(!strcmp(action,"status")){
        if(request!=token){out.println("APPS ERR unknown_session");return true;}state(out);
        if(complete&&list&&outputCatalog&&result==apps::Result::Ok){for(size_t i=0;i<outputCatalog->count;++i){const auto& e=outputCatalog->entries[i];
            out.printf("APPS ENTRY %s %s %s %s %s\n",e.identity.medium==apps::Medium::Flash?"flash":"sd",e.identity.origin,e.identity.id,e.identity.legacy?"legacy":"managed",e.name);}
            out.printf("APPS END overflow=%u\n",outputCatalog->overflow);}
        return true;
    }
    if(!strcmp(action,"cancel")){
        if(request!=token){out.println("APPS ERR unknown_session");return true;}
        if(job){jobs::cancel(job);state(out);return true;}
        if(receiving){input.reset();receiving=false;complete=true;result=apps::Result::Cancelled;}
        state(out);return true;
    }
    if(!strcmp(action,"chunk")){
        char trailing; if(request!=token||!receiving||sscanf(rest,"%lu %255s %c",&offset,extra,&trailing)!=2||strlen(extra)%2){out.println("APPS ERR chunk");return true;}
        const size_t n=strlen(extra)/2;
        if(!n||offset>received||offset>expected||n>expected-offset){out.println("APPS ERR offset");return true;}
        for(size_t i=0;i<n;++i)if(digit(extra[i*2])<0||digit(extra[i*2+1])<0){out.println("APPS ERR hex");return true;}
        if(offset<received){if(n>received-offset){out.println("APPS ERR overlap");return true;}
            for(size_t i=0;i<n;++i)if(input.bytes[offset+i]!=uint8_t(digit(extra[i*2])*16+digit(extra[i*2+1]))){out.println("APPS ERR replay");return true;}}
        else {for(size_t i=0;i<n;++i)input.bytes[offset+i]=uint8_t(digit(extra[i*2])*16+digit(extra[i*2+1]));received+=n;}
        lastActivity=millis();state(out);return true;
    }
    if(!strcmp(action,"commit")){
        if(request!=token){out.println("APPS ERR unknown_session");return true;}
        if(!receiving){state(out);return true;} // do not repeat a submitted install
        if(received!=expected){out.println("APPS ERR incomplete");return true;}
        apps::View view;result=apps::inspect(input.bytes,received,view);
        if(result!=apps::Result::Ok){receiving=false;complete=true;input.reset();state(out);return true;}
        strcpy(target.id,view.id);job=jobs::request(jobs::Operation::Install,target,input.bytes,received);
        if(!job){out.println("APPS ERR busy");return true;}
        receiving=false;input.reset();state(out);return true;
    }
    if(!strcmp(action,"get")){
        if(request!=token||!complete||result!=apps::Result::Ok||sscanf(rest,"%lu",&offset)!=1||offset>=outputPackage.size){out.println("APPS ERR get");return true;}
        const size_t n=std::min(size_t(96),outputPackage.size-size_t(offset));char hex[193];const char* digits="0123456789abcdef";
        for(size_t i=0;i<n;++i){uint8_t b=outputPackage.bytes[offset+i];hex[i*2]=digits[b>>4];hex[i*2+1]=digits[b&15];}hex[n*2]=0;
        out.printf("APPS DATA %08lx %lu %s\n",(unsigned long)token,offset,hex);return true;
    }
    if(job||receiving){out.println("APPS ERR busy");return true;}
    // Same-session start replay returns its receipt; new operation uses new token.
    if(request==token&&complete){state(out);return true;}
    if(!strcmp(action,"begin")){
        char trailing;if(sscanf(rest,"%95s %lu %c",arg,&offset,&trailing)!=2||!medium(arg,target.medium)||offset<12||offset>apps::kPackageBytes){out.println("APPS ERR begin");return true;}
        const auto selectedMedium=target.medium;reset();target={};target.medium=selectedMedium;
        input.bytes=static_cast<uint8_t*>(heap_caps_malloc(offset,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!input.bytes){out.println("APPS ERR no_memory");return true;}
        token=request;expected=offset;receiving=true;lastActivity=millis();result=apps::Result::Ok;state(out);return true;
    }
    if(!strcmp(action,"download")){
        apps::Medium selected;char trailing;
        if(sscanf(rest,"%95s %255s %c",arg,extra,&trailing)!=2||!medium(arg,selected)){out.println("APPS ERR download");return true;}
        const uint32_t queued=jobs::requestDownload(extra,selected==apps::Medium::Flash?jobs::DownloadTarget::Flash:jobs::DownloadTarget::Sd);
        if(!queued){out.println("APPS ERR busy_or_invalid_qr");return true;}
        reset();token=request;job=queued;result=apps::Result::Ok;state(out);return true;
    }
    jobs::Operation op;apps::Identity identity;bool listing=false;
    if(!strcmp(action,"list")){op=jobs::Operation::Catalog;listing=true;}
    else if(!strcmp(action,"init")){if(strcmp(rest,"flash")){out.println("APPS ERR init");return true;}op=jobs::Operation::InitializeFlash;}
    else if(!strcmp(action,"load")||!strcmp(action,"remove")||!strcmp(action,"import")){
        char id[32],origin[65],trailing;
        if(sscanf(rest,"%95s %64s %31s %c",arg,origin,id,&trailing)!=3||!medium(arg,identity.medium)){out.println("APPS ERR identity");return true;}
        strcpy(identity.origin,origin);strcpy(identity.id,id);if(!apps::validIdentity(identity)){out.println("APPS ERR identity");return true;}
        if(!strcmp(action,"import")&&strcmp(origin,"local")){out.println("APPS ERR origin");return true;}
        op=!strcmp(action,"load")?jobs::Operation::Load:!strcmp(action,"import")?jobs::Operation::Import:jobs::Operation::Remove;
    }else{out.println("APPS ERR command");return true;}
    apps::Catalog* nextCatalog=nullptr;
    if(listing){
        void* memory=heap_caps_malloc(sizeof(apps::Catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!memory){out.println("APPS ERR no_memory");return true;}
        nextCatalog=new(memory) apps::Catalog{};
    }
    uint32_t queued=jobs::request(op,identity);
    if(!queued){heap_caps_free(nextCatalog);out.println("APPS ERR busy");return true;}
    reset();outputCatalog=nextCatalog;token=request;job=queued;list=listing;result=apps::Result::Ok;state(out);return true;
}
}
