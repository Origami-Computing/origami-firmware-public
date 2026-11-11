#pragma once
#include <stdint.h>
#include <string.h>
namespace apps { namespace onboarding {
struct DemoSpeech {
 static constexpr unsigned kQuestionMs=20,kReplyAt=3100,kReplyMs=20;
 static const char* question(){return "Kami, what can I cook for dinner?";}
 static const char* reply(){return "Try a quick rice bowl.Cook rice, add vegetablesand a fried egg. Enjoy!";}
 struct Frame {const char* text;unsigned count,phase;};
 static Frame at(unsigned ms){
  const char* text=ms<2100?question():ms>=kReplyAt?reply():"";
  unsigned count=ms<2100?ms/kQuestionMs:ms>=kReplyAt?(ms-kReplyAt)/kReplyMs:0;
  if(count>strlen(text))count=strlen(text);
  return {text,count,ms<2100?1u:ms>=kReplyAt?2u:0u};
 }
};
}}
