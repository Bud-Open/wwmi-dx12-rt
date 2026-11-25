#pragma once
#include <cstdint>
struct InjectFrame;
struct SectionDraw;
unsigned RtPixelColorOutputs(uint64_t);
bool RtPositionOnlyPass(uint64_t pipeline);
bool RtHasPrivateSection();
bool RtHasPrivateDepthSection();
void RtRememberPrivatePsFormat(unsigned slot,unsigned format);
void RtCarryPrivatePsFormats(uint64_t recording,unsigned reads,const unsigned* formats,unsigned count);
void RtBeginPrivateSection(uint64_t section,uint64_t frame);
bool RtQueuePrivateSubdraw(SectionDraw&,unsigned count,unsigned first,int32_t base);
bool RtFinishPrivateSection(uint64_t section,bool completed,InjectFrame& fr);
void RtDiscardPrivatePlan();
void RtPublishWorldSkeletonInputs();
int RtPrimitiveRegister(uint64_t pipeline);
