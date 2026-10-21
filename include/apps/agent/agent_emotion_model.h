#pragma once
#include <ArduinoJson.h>
#include "ui/eye_rig_math.h"
#include "ui/kami_expression.h"

namespace apps { namespace agent { namespace emotion {
struct Selection {
  ui::eye_rig::Pose pose=ui::eye_rig::kPresets[0].pose;
  const char* name="neutral";
  float speed=.8f;
  uint32_t revision=0;
  bool received=false;
  ui::kami_expression::Shape shape=ui::kami_expression::Shape::Open;
};
inline bool number(JsonVariantConst v,float lo,float hi,float& dest) {
  if(v.isNull())return true;
  if(!v.is<float>()||!std::isfinite(v.as<float>()))return false;
  dest=ui::eye_rig::clamp(v.as<float>(),lo,hi);return true;
}
inline bool lid(JsonVariantConst v,ui::eye_rig::Lid& out) {
  if(v.isNull())return true;
  if(!v.is<JsonObjectConst>())return false;
  return number(v["topY"],0,1,out.topY)&&number(v["topX"],-.7f,.7f,out.topX)&&
    number(v["bottomY"],0,1,out.bottomY)&&number(v["bottomX"],-.7f,.7f,out.bottomX)&&
    number(v["size"],20,400,out.size);
}
inline bool parse(JsonVariantConst raw,Selection& result) {
  if(!raw.is<JsonObjectConst>())return false;
  Selection next;
  const char* name=raw["preset"].as<const char*>();
  if(!raw["preset"].isNull()) {
    const auto* p=ui::eye_rig::preset(name);
    if(p){next.pose=p->pose;next.name=p->name;next.speed=p->speed;}
    else if(ui::kami_expression::namedShape(name,next.shape)){
      // Stable string storage; the JSON event is destroyed after dispatch.
      next.name=next.shape==ui::kami_expression::Shape::Bowl?"sleeping":next.shape==ui::kami_expression::Shape::Arc?"closed_smile":next.shape==ui::kami_expression::Shape::Spiral?"dizzy":"squeezed";
    }else return false;
  }else{
    if(raw["left"].isNull()&&raw["right"].isNull()&&raw["eyeScale"].isNull())return false;
    next.name="custom";
  }
  if(!lid(raw["left"],next.pose.left)||!lid(raw["right"],next.pose.right)||
     !number(raw["eyeScale"],.5f,1.5f,next.pose.scale)||
     !number(raw["springSpeed"],.1f,1,next.speed))return false;
  next.received=true;result=next;return true;
}

// Caller serializes this bounded mailbox. No rendering or heap ownership crosses
// cores. Each reply segment retains the expression that accompanied its text.
class Turn {
  Selection pending_,slots_[16];
  bool valid_[16]={};
  bool cleanup_=false,messageTool_=false;
  int last_=-1;
  uint32_t serial_=0;
public:
  void reset(){pending_=Selection{};for(bool& v:valid_)v=false;cleanup_=messageTool_=false;last_=-1;pending_.revision=++serial_;}
  void tool(const char* name,bool start){
    if(start){cleanup_=!strcmp(name,"stay_silent");messageTool_=!strcmp(name,"message")||!strcmp(name,"send_message");}
    else {cleanup_=false;messageTool_=false;}
  }
  bool accept(Selection value){
    if(cleanup_)return false;
    value.revision=++serial_;pending_=value;
    // Explicit standalone updates affect the current reply; emotion bundled
    // with a new message must wait for that message's segment instead.
    if(last_>=0&&!messageTool_){slots_[last_]=value;valid_[last_]=true;}
    return true;
  }
  void bind(int index){if(index<0||index>=16)return;slots_[index]=pending_;valid_[index]=true;last_=index;}
  Selection get(int index)const{return index>=0&&index<16&&valid_[index]?slots_[index]:pending_;}
};
}}}
