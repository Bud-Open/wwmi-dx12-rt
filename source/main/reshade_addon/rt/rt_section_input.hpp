#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
namespace rt_section {
enum class Source : uint8_t { unknown, original_pipeline, applied_override, shim_last_write, port_logical };
struct Element { char semantic[64]{}; uint32_t semantic_index=0, format=0, slot=0, offset=0, slot_class=0, step=0; bool attribute=false; };
struct View { uint64_t resource=0, va=0; uint32_t bytes=0, stride=0; Source source=Source::unknown; };
struct Input {
 int mod=-1, component=-1; uint64_t pso=0, vs_hash=0, original_pso=0;
 uint32_t process_id=0; uint64_t executed_session=0, executed_call=0;
 uint32_t count=0, first=0, instances=0, first_instance=0; int32_t base=0;
 Source layout_source=Source::unknown; Element elements[32]{}; uint32_t element_count=0;
 View vb[8]{}, cb[3]{}; // CB 2/3/4, only logical Port state; no final-binding claim.
 bool instance_known=false, recording_known=false, resource_generation_known=false, deformation_known=false;
};
struct Verdict { bool has_draw=false, has_layout=false, position_semantic_hint=false, blend_semantic_hint=false; bool compute_ready=false; };
inline Verdict Consume(const Input& x) noexcept {
 Verdict v{}; v.has_draw=x.mod>=0 && x.component>=0 && x.pso && x.vs_hash && x.count && x.instances;
 v.has_layout=x.layout_source!=Source::unknown && x.element_count>0 && x.element_count<=32;
 if(v.has_layout) for(uint32_t i=0;i<x.element_count;++i) {const auto &e=x.elements[i];
  if(e.attribute && e.semantic_index==0 && e.slot==0) v.position_semantic_hint=true;
  if(e.attribute && (e.semantic_index==3 || e.semantic_index==4) && e.slot==4) v.blend_semantic_hint=true;
 }
 // Current sources are hints, never final/native proofs; no GPU-ready result is possible yet.
 v.compute_ready=false; return v;
}
inline bool InstallLayout(Input& x, Source source, const Element* elements, uint32_t count) noexcept {
 x.layout_source=Source::unknown; x.element_count=0;
 if (!elements || !count || count>32 || (source!=Source::original_pipeline && source!=Source::applied_override)) return false;
 for (uint32_t i=0;i<count;++i) x.elements[i]=elements[i];
 x.element_count=count; x.layout_source=source; return true;
}
inline Source SelectLayout(bool override_applied, bool original_captured) noexcept {
 return override_applied?Source::applied_override:original_captured?Source::original_pipeline:Source::unknown;
}
inline const char* SourceName(Source s) noexcept {
 switch(s) {case Source::original_pipeline:return "original_pipeline"; case Source::applied_override:return "applied_override";
 case Source::shim_last_write:return "shim_last_write"; case Source::port_logical:return "port_logical"; default:return "unknown";}
}
// Emit bounded lines synchronously. Numeric resource identities are hints, not ownership or final-binding proof.
template<class Emit> void Format(const Input& x, unsigned seq, Emit emit) {
 char line[640]; const auto v=Consume(x);
 std::snprintf(line,sizeof line,"RTINPUT seq=%u pid=%u executed_session=%llx executed_call=%llx mod=%d component=%d original_pso=%llx selected_pso=%llx original_vs=%llx count=%u instances=%u first=%u base=%d first_instance=%u layout_source=%s elements=%u position_semantic_hint=%u blend_semantic_hint=%u instance=unknown recording=unknown resource_generation=unknown deformation=unknown compute_ready=0",seq,x.process_id,(unsigned long long)x.executed_session,(unsigned long long)x.executed_call,x.mod,x.component,(unsigned long long)x.original_pso,(unsigned long long)x.pso,(unsigned long long)x.vs_hash,x.count,x.instances,x.first,x.base,x.first_instance,SourceName(x.layout_source),v.has_layout?x.element_count:0,(unsigned)v.position_semantic_hint,(unsigned)v.blend_semantic_hint);
 emit(line);
 if(v.has_layout) for(uint32_t i=0;i<x.element_count;++i) { const auto& e=x.elements[i];
  std::snprintf(line,sizeof line,"RTINPUT seq=%u layout[%u] source=%s semantic=%s index=%u format=%u slot=%u offset=%u class=%u step=%u",seq,i,SourceName(x.layout_source),e.semantic,e.semantic_index,e.format,e.slot,e.offset,e.slot_class,e.step); emit(line);
 }
 auto view=[&](const char* name,unsigned slot,const View& w) {
  const bool known=w.source==Source::shim_last_write || w.source==Source::port_logical;
  std::snprintf(line,sizeof line,"RTINPUT seq=%u %s%u source=%s resource=%llx va=%llx bytes=%u stride=%u",seq,name,slot,SourceName(known?w.source:Source::unknown),(unsigned long long)(known?w.resource:0),(unsigned long long)(known?w.va:0),known?w.bytes:0,known?w.stride:0); emit(line);
 };
 view("vb",0,x.vb[0]); view("vb",4,x.vb[4]); for(unsigned i=0;i<3;++i) view("cb",i+2,x.cb[i]);
}

}
