#pragma once
// Passive CPU event reconstruction only. Never implies GPU descriptor contents or completion.
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <chrono>
namespace UavObserve {
inline uint64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct View { uint64_t res=0, generation=0, offset=0, size=0; uint32_t format=0; bool observed=false; };
struct SlotKey { uint64_t heap=0; uint32_t off=0; bool operator==(const SlotKey& o) const { return heap==o.heap && off==o.off; } };
struct KeyHash { size_t operator()(SlotKey k) const { return std::hash<uint64_t>{}(k.heap ^ (uint64_t(k.off)<<32)); } };
struct Event { uint64_t time=0, a=0,b=0,c=0,d=0,e=0,f=0; uint32_t kind=0, frame=0, tid=0; int32_t owner=-1; uint64_t instance=0, instance_generation=0; };
class Probe {
 std::mutex mu; std::unordered_map<uint64_t,uint64_t> generations; std::unordered_map<uint64_t,View> sources; std::unordered_map<uint64_t,uint64_t> source_heaps;
 std::unordered_map<SlotKey,View,KeyHash> slots; std::vector<Event> events;
 uint64_t nextgen=0,start=0; unsigned over=0; bool started=false, stopped=false, drained=false;
 static constexpr size_t cap=8192, max_events=512;
 void expire(uint64_t now) { if(started && !stopped && (now-start>=10000 || events.size()>=max_events)) stopped=true; }
 void put(uint64_t now,Event e) { expire(now); if(!started || stopped) return; e.time=now; events.push_back(e); expire(now); }
 public:
 std::atomic<bool> enabled{false};
 void configure(bool on) { enabled.store(on); }
 void resource_create(uint64_t r) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped)return; if(generations.size()>=cap && !generations.count(r)){++over;return;} generations[r]=++nextgen; }
 void resource_destroy(uint64_t r) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped)return; generations.erase(r); }
 uint64_t generation(uint64_t r) { if(!enabled.load() || !r) return 0; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped) return 0; auto i=generations.find(r); return i==generations.end() ? 0 : i->second; }
 void view(uint64_t handle,uint64_t r,uint64_t offset,uint64_t size,uint32_t format,bool observed) {
  if(!enabled.load()) return;
  std::lock_guard<std::mutex> l(mu);
  expire(now_ms());
  if(stopped) return;
  auto gen=generations.find(r); if(!r || gen==generations.end() || !observed) { sources.erase(handle);source_heaps.erase(handle);return; }
  if(sources.size()>=cap && !sources.count(handle)){ sources.erase(handle);++over;return; }
  sources[handle]=View{r,gen->second,offset,size,format,true};
 }
 void direct(SlotKey dst,uint64_t source,bool same_slot=false) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped)return; auto it=sources.find(source); if(same_slot && it!=sources.end()) source_heaps[source]=dst.heap; bool watched=source_heaps.count(source)!=0; if(!slots.count(dst) && slots.size()>=cap){++over;return;} slots[dst]=(it==sources.end() || !watched ? View{} : it->second); }
 void direct_update(SlotKey dst,uint64_t source,uint64_t heap_start,uint32_t increment,uint32_t count) {
  const bool in_range=increment && heap_start && dst.off<count && uint64_t(dst.off)<=UINT64_MAX/increment;
  const uint64_t delta=in_range ? uint64_t(dst.off)*increment : 0;
  const bool same=in_range && heap_start<=UINT64_MAX-delta && source==heap_start+delta;
  direct(dst,source,same);
 }
 void copy(SlotKey dst,SlotKey src,uint64_t src_handle,bool watched) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped)return; View v{}; auto si=slots.find(src); if(si!=slots.end())v=si->second; else if(watched) { auto it=sources.find(src_handle); if(it!=sources.end() && source_heaps.count(src_handle) && source_heaps[src_handle]==src.heap){v=it->second;} } if(!slots.count(dst) && slots.size()>=cap){++over;return;} slots[dst]=v; }
 void invalidate(SlotKey k) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(!stopped) { if(!slots.count(k) && slots.size()>=cap) ++over; else slots[k]=View{}; } }
 void heap_destroy(uint64_t h) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped)return; for(auto it=slots.begin();it!=slots.end();) if(it->first.heap==h)it=slots.erase(it); else ++it; for(auto it=source_heaps.begin();it!=source_heaps.end();) if(it->second==h){sources.erase(it->first); it=source_heaps.erase(it);} else ++it; }
 View read(SlotKey k,uint64_t r) { if(!enabled.load())return {}; std::lock_guard<std::mutex> l(mu); expire(now_ms()); if(stopped)return {}; auto i=slots.find(k); if(i==slots.end()||i->second.res!=r)return {}; auto g=generations.find(r); if(g==generations.end()||g->second!=i->second.generation)return {}; return i->second; }
 void target(uint64_t now,Event e) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); expire(now); if(stopped)return; if(!started){started=true;start=now;} put(now,e); }
 void record(uint64_t now,Event e) { if(!enabled.load())return; std::lock_guard<std::mutex> l(mu); put(now,e); }
 bool take(uint64_t now,std::vector<Event>& out,unsigned& overflow) { if(!enabled.load())return false; std::lock_guard<std::mutex> l(mu); expire(now); if(!stopped||drained)return false; drained=true; out=events; overflow=over; return true; }
};
inline Probe g_probe;
struct ScopeContext { bool active=false; int32_t owner=-1; uint64_t instance=0, generation=0; };
inline thread_local ScopeContext target_scope{};
inline void apply_scope(Event& e) { if(target_scope.active) { e.owner=target_scope.owner; e.instance=target_scope.instance; e.instance_generation=target_scope.generation; } }
struct TargetScope {
 ScopeContext previous;
 TargetScope(bool target,int32_t owner,uint64_t instance,uint64_t generation): previous(target_scope) { target_scope=target ? ScopeContext{true,owner,instance,generation} : ScopeContext{}; }
 ~TargetScope() { target_scope=previous; }
};
}
