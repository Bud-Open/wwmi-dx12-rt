#include "rt_program_contracts.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

struct RtPsMaterialHint {
    uint64_t uv_srvs = 0;
    uint64_t normal_srvs = 0;
    uint64_t non_cmp_2d = 0;
    uint8_t  first_order[16]{};
    uint8_t  lane_mask[32]{};
    bool     valid = false;
};
namespace {
std::mutex g_ps_material_hint_mtx;
std::unordered_map<uint64_t, RtPsMaterialHint> g_ps_material_hints;
}
bool RtLookupPsMaterialHint(uint64_t ps_hash, RtPsMaterialHint& out) {
    out = {};
    if (!ps_hash) return false;
    std::lock_guard<std::mutex> lk(g_ps_material_hint_mtx);
    const auto it = g_ps_material_hints.find(ps_hash);
    if (it == g_ps_material_hints.end()) return false;
    out = it->second;
    return out.valid;
}

namespace wwmi::program {
namespace {
using Text=std::string;
Text trim(Text s) {
    const auto a=s.find_first_not_of(" \t\r\n"),b=s.find_last_not_of(" \t\r\n");
    return a==Text::npos?Text{}:s.substr(a,b-a+1);
}
bool starts(const Text& s,const Text& p) {return s.compare(0,p.size(),p)==0;}
bool number(const Text& s,uint32_t& out) {
    if(s.empty())return false;
    uint64_t n=0;
    for(const auto c:s){if(c<'0'||c>'9')return false;n=n*10+unsigned(c-'0');if(n>UINT32_MAX)return false;}
    out=uint32_t(n);return true;
}
bool integer(const Text& s,const Text& type,uint32_t& out) {return starts(s,type+" ")&&number(s.substr(type.size()+1),out);}
bool signature_location(const Text& s,const Text& type,int32_t& out) {
    if(s==type+" -1"){out=-1;return true;}
    uint32_t n=0;if(!integer(s,type,n)||n>INT32_MAX)return false;out=int32_t(n);return true;
}
std::vector<Text> split(const Text& s,char separator=',') {
    std::vector<Text> out;size_t begin=0;int depth=0;bool quote=false,escaped=false;
    for(size_t i=0;i<s.size();++i){
        const auto c=s[i];
        if(quote){if(!escaped&&c=='"')quote=false;escaped=!escaped&&c=='\\';continue;}
        if(c=='"'){quote=true;continue;}
        if(c=='('||c=='{'||c=='['||c=='<')++depth;
        else if(c==')'||c=='}'||c==']'||c=='>')--depth;
        else if(c==separator&&!depth){out.push_back(trim(s.substr(begin,i-begin)));begin=i+1;}
    }
    out.push_back(trim(s.substr(begin)));return out;
}
Text uncomment(const Text& s) {
    bool quote=false,escape=false;
    for(size_t i=0;i<s.size();++i){
        if(quote){if(!escape&&s[i]=='"')quote=false;escape=!escape&&s[i]=='\\';}
        else if(s[i]=='"')quote=true;
        else if(s[i]==';')return trim(s.substr(0,i));
    }
    return trim(s);
}
Text strip_inst_metadata(const Text& s) {
    // Strip trailing LLVM instruction metadata like ", !dx.precise !33" or ", !dbg !12".
    bool quote=false,escape=false;int depth=0;
    for(size_t i=0;i<s.size();++i){
        const auto c=s[i];
        if(quote){if(!escape&&c=='"')quote=false;escape=!escape&&c=='\\';continue;}
        if(c=='"'){quote=true;continue;}
        if(c=='('||c=='{'||c=='['||c=='<')++depth;
        else if(c==')'||c=='}'||c==']'||c=='>')--depth;
        else if(!depth&&c==','&&i+2<s.size()&&s[i+1]==' '&&s[i+2]=='!')return trim(s.substr(0,i));
    }
    return trim(s);
}
std::vector<Text> refs(const Text& s,char prefix='%') {
    std::vector<Text> out;bool quote=false,escape=false;
    for(size_t i=0;i<s.size();++i){
        if(quote){if(!escape&&s[i]=='"')quote=false;escape=!escape&&s[i]=='\\';continue;}
        if(s[i]=='"'){quote=true;continue;}
        if(s[i]!=prefix||i+1==s.size()||!std::isdigit(static_cast<unsigned char>(s[i+1])))continue;
        const size_t begin=i++;while(i+1<s.size()&&std::isdigit(static_cast<unsigned char>(s[i+1])))++i;
        out.push_back(s.substr(begin,i-begin+1));
    }
    return out;
}
Text final_value(const Text& s) {const auto at=s.find_last_of(' ');return at==Text::npos?s:s.substr(at+1);}
bool contains_ref(const Text& s,const Text& r) {const auto v=refs(s);return std::find(v.begin(),v.end(),r)!=v.end();}
bool mentions_type(const Text& line,const Text& type) {
    size_t at=0;
    while((at=line.find(type,at))!=Text::npos){
        const auto end=at+type.size();
        if(end==line.size()||(!std::isalnum(static_cast<unsigned char>(line[end]))&&Text("_.$-").find(line[end])==Text::npos))return true;
        at=end;
    }
    return false;
}
bool parse_i32_binop(const Text& raw_rhs,Text& op,Text& a,Text& b) {
    const auto rhs=strip_inst_metadata(raw_rhs);
    const auto sp=rhs.find(' ');if(sp==Text::npos)return false;
    op=rhs.substr(0,sp);
    if(op!="add"&&op!="sub"&&op!="mul"&&op!="shl"&&op!="lshr"&&op!="ashr"&&op!="and"&&op!="or"&&op!="xor")return false;
    size_t pos=sp+1;
    while(pos<rhs.size()){
        while(pos<rhs.size()&&rhs[pos]==' ')++pos;
        if(rhs.compare(pos,4,"nuw ")==0){pos+=4;continue;}
        if(rhs.compare(pos,4,"nsw ")==0){pos+=4;continue;}
        if(rhs.compare(pos,6,"exact ")==0){pos+=6;continue;}
        break;
    }
    if(rhs.compare(pos,4,"i32 ")!=0)return false;
    pos+=4;
    const auto comma=rhs.find(',',pos);if(comma==Text::npos)return false;
    a=trim(rhs.substr(pos,comma-pos));
    b=trim(rhs.substr(comma+1));
    return !a.empty()&&!b.empty();
}
struct Instruction {Text lhs,rhs,block;};
struct CbLoad {Text var,kind;uint32_t reg=0,row=0;bool fixed=false;};
struct Module {
    std::map<Text,Text> metadata,named,attributes;
    std::map<Text,size_t> definitions;
    std::vector<Instruction> instructions;
    std::vector<Text> module_lines;
    std::map<Text,std::vector<size_t>> blocks;
    std::map<Text,std::pair<uint32_t,uint32_t>> handles; // class, resource range id
    std::vector<Resource> resources;
    std::map<uint32_t,uint32_t> srv_shapes; // SRV reg -> DXIL resource shape
    std::vector<Signature> inputs,outputs;
    std::vector<CbLoad> cb_loads;
    Text entry,reason;
    Text view_parameter;
    std::set<Text> opaque_cb_types;
    uint32_t mode_row=308,view_i32_count=0;
    uint32_t view_reg=UINT32_MAX,primitive_reg=UINT32_MAX,local_reg=UINT32_MAX;
    uint32_t view_bytes=0,primitive_bytes=0,local_bytes=0;
    bool resource_reads_modelled=true;

    const Text& node(const Text& name) const {static const Text empty;const auto i=metadata.find(name);return i==metadata.end()?empty:i->second;}
    std::vector<Text> tuple(const Text& value) const {
        Text s=value;
        if(starts(s,"!")&&s.size()>1&&std::isdigit(static_cast<unsigned char>(s[1])))s=node(s);
        if(starts(s,"distinct "))s=s.substr(9);
        if(s.size()<3||!starts(s,"!{")||s.back()!='}')return {};
        return split(s.substr(2,s.size()-3));
    }
    bool call(const Text& rhs,const Text& op,std::vector<Text>& args) const {
        const auto at=rhs.find("@dx.op."+op+"(");if(at==Text::npos)return false;
        const auto start=rhs.find('(',at);const auto end=rhs.find(')',start);
        if(end==Text::npos)return false;
        args=split(rhs.substr(start+1,end-start-1));return true;
    }
    const Instruction* def(const Text& var) const {const auto i=definitions.find(var);return i==definitions.end()?nullptr:&instructions[i->second];}
    const Resource* resource(uint32_t kind,uint32_t id) const {
        for(const auto& r:resources)if(r.resource_class==kind&&r.range_id==id)return &r;
        return nullptr;
    }
    const Resource* resource_for_handle(const Text& handle) const {
        const auto h=handles.find(handle);return h==handles.end()?nullptr:resource(h->second.first,h->second.second);
    }
    bool signatures(const Text& list,std::vector<Signature>& dst) {
        if(list=="null")return true;
        const auto elements=tuple(list);if(elements.empty())return false;
        std::set<uint32_t> ids;
        for(const auto& ref:elements){
            const auto t=tuple(ref);Signature s;
            if(t.size()!=11||!integer(t[0],"i32",s.id)||!starts(t[1],"!\"")||t[1].back()!='"'||
               !integer(t[2],"i8",s.component)||!integer(t[3],"i8",s.kind)||
               !integer(t[5],"i8",s.interpolation)||!integer(t[6],"i32",s.rows)||
               !integer(t[7],"i8",s.columns)||!signature_location(t[8],"i32",s.start_row)||
               !signature_location(t[9],"i8",s.start_column)||!s.rows||!s.columns||s.columns>4||!ids.insert(s.id).second)return false;
            s.name=t[1].substr(2,t[1].size()-3);
            const auto indices=tuple(t[4]);if(indices.size()!=s.rows)return false;
            for(const auto& i:indices){uint32_t n=0;if(!integer(i,"i32",n))return false;s.indices.push_back(n);}
            dst.push_back(std::move(s));
        }
        return true;
    }
    bool parse(const Text& text,Stage expected) {
        if(text.empty()||text.size()>8u*1024u*1024u){reason="disassembly size outside supported bound";return false;}
        std::istringstream stream(text);Text raw,block="0";bool function=false;unsigned functions=0;
        while(std::getline(stream,raw)){
            Text label=trim(raw);
            if(starts(label,"; <label>:")){
                label=label.substr(10);const auto end=label.find_first_not_of("0123456789");label=label.substr(0,end);
                if(!function||label.empty()){reason="invalid basic block label";return false;}block=label;module_lines.push_back("label %"+label);continue;
            }
            auto line=uncomment(raw);if(line.empty())continue;
            if(line[0]=='!'){
                const auto eq=line.find(" = ");if(eq==Text::npos){reason="metadata definition not understood";return false;}
                const auto key=line.substr(0,eq),value=line.substr(eq+3);
                auto& dst=key.size()>1&&std::isdigit(static_cast<unsigned char>(key[1]))?metadata:named;
                if(!dst.emplace(key,value).second){reason="duplicate metadata definition";return false;}continue;
            }
            if(starts(line,"attributes #")){
                const auto eq=line.find(" = ");if(eq==Text::npos){reason="attribute group not understood";return false;}
                attributes.emplace(line.substr(11,eq-11),line.substr(eq+3));continue;
            }
            if(starts(line,"define ")){
                if(function||++functions!=1||!starts(line,"define void @")||line.find("() {")==Text::npos){reason="only one void shader entry is supported";return false;}
                entry=line.substr(line.find('@'),line.find('(')-line.find('@'));function=true;module_lines.push_back(line);continue;
            }
            if(line=="}"){if(!function){reason="unexpected function end";return false;}function=false;module_lines.push_back(line);continue;}
            if(function){
                if(line.back()==':'){
                    auto n=line.substr(0,line.size()-1);uint32_t value=0;
                    if(!number(n,value)){reason="named basic blocks are not in this parser version";return false;}
                    block=n;module_lines.push_back("label %"+n);continue;
                }
                Instruction i;i.block=block;const auto eq=line.find(" = ");
                if(starts(line,"%")&&eq!=Text::npos){i.lhs=line.substr(0,eq);i.rhs=line.substr(eq+3);
                    if(!definitions.emplace(i.lhs,instructions.size()).second){reason="duplicate SSA definition";return false;}}
                else i.rhs=line;
                blocks[block].push_back(instructions.size());instructions.push_back(std::move(i));
            }
            module_lines.push_back(line);
        }
        if(function||functions!=1){reason="missing complete shader entry";return false;}
        auto sm=named.find("!dx.shaderModel"),ep=named.find("!dx.entryPoints"),rr=named.find("!dx.resources");
        if(sm==named.end()||ep==named.end()||rr==named.end()){reason="missing executable DXIL metadata";return false;}
        const auto model=tuple(sm->second);
        if(model.size()!=1){reason="shader model root is not unique";return false;}
        const auto m=tuple(model[0]);
        if(m.size()!=3||m[0]!=(expected==Stage::vertex?"!\"vs\"":"!\"ps\"")||m[1]!="i32 6"||m[2]!="i32 0"){
            reason="unsupported shader stage or shader-model semantics";return false;
        }
        const auto entries=tuple(ep->second);if(entries.size()!=1){reason="shader entry metadata not unique";return false;}
        const auto e=tuple(entries[0]);if(e.size()!=5){reason="entry metadata not understood";return false;}
        const auto sig=tuple(e[2]);
        if(sig.size()!=3||sig[2]!="null"||!signatures(sig[0],inputs)||!signatures(sig[1],outputs)){
            reason="input/output semantic metadata not understood";return false;
        }
        const auto roots=tuple(rr->second);if(roots.size()!=1){reason="resource root not unique";return false;}
        const auto classes=tuple(roots[0]);if(classes.size()!=4){reason="resource classes not understood";return false;}
        std::set<std::pair<uint32_t,uint32_t>> ids,registers;
        for(uint32_t c=0;c<4;++c){
            if(classes[c]=="null")continue;
            for(const auto& ref:tuple(classes[c])){
                const auto t=tuple(ref);Resource r;r.resource_class=c;
                if(t.size()<(c==0||c==1?9u:8u)||!integer(t[0],"i32",r.range_id)||
                   !integer(t[3],"i32",r.space)||!integer(t[4],"i32",r.reg)||!integer(t[5],"i32",r.count)||
                   r.space||r.count!=1||r.reg>=128||!ids.insert({c,r.range_id}).second||!registers.insert({c,r.reg}).second){
                    reason="unsupported, ambiguous or unbounded resource binding";return false;
                }
                r.type=t[1];
                if(c==0){
                    uint32_t shape=0;
                    if(!integer(t[6],"i32",shape)){reason="SRV resource shape metadata not understood";return false;}
                    srv_shapes[r.reg]=shape;
                }
                if(c==2){
                    if(!integer(t[6],"i32",r.bytes)||!r.bytes||r.bytes>65536){reason="constant-buffer declaration out of range";return false;}
                    if(expected==Stage::vertex){
                        const Text suffix="* undef";
                        if(r.type.size()<=suffix.size()||r.type.compare(r.type.size()-suffix.size(),suffix.size(),suffix)!=0){
                            reason="CB resource does not have an unused reflected type";return false;
                        }
                        opaque_cb_types.insert(r.type.substr(0,r.type.size()-suffix.size()));
                    }
                    if(r.type=="%hostlayout.View* undef"||r.type=="%View* undef"){
                        if(view_reg!=UINT32_MAX){reason="ambiguous View role";return false;}view_reg=r.reg;view_bytes=r.bytes;
                    }
                    if(r.type=="%hostlayout.Primitive* undef"||r.type=="%Primitive* undef"){
                        if(primitive_reg!=UINT32_MAX){reason="ambiguous Primitive role";return false;}primitive_reg=r.reg;primitive_bytes=r.bytes;
                    }
                    if(r.type=="%LocalVF* undef"||r.type=="%hostlayout.LocalVF* undef"){
                        if(local_reg!=UINT32_MAX){reason="ambiguous LocalVF role";return false;}local_reg=r.reg;local_bytes=r.bytes;
                    }
                }
                resources.push_back(std::move(r));
            }
        }
        for(const auto& i:instructions){
            std::vector<Text> a;
            if(call(i.rhs,"createHandle",a)){
                uint32_t cls=0,id=0,index=0,opcode=0;
                if(a.size()!=5||!integer(a[0],"i32",opcode)||opcode!=57||!integer(a[1],"i8",cls)||
                   !integer(a[2],"i32",id)||!integer(a[3],"i32",index)||a[4]!="i1 false"){
                    reason="dynamic resource handles require a new program model";return false;
                }
                const auto* r=resource(cls,id);if(!r||r->reg!=index){reason="handle does not resolve to its actual resource declaration";return false;}
                handles.emplace(i.lhs,std::pair{cls,id});
            }
            if(i.rhs.find("@dx.op.createHandleFrom")!=Text::npos||i.rhs.find("@dx.op.annotateHandle")!=Text::npos){
                reason="descriptor-heap or newer handle semantics are unsupported";return false;
            }
        }
        for(const auto& i:instructions){
            const auto at=i.rhs.find("@dx.op.cbufferLoadLegacy.");if(at==Text::npos)continue;
            const auto end=i.rhs.find('(',at);const auto name=i.rhs.substr(at+7,end-at-7);std::vector<Text> a;
            if(!call(i.rhs,name,a)||a.size()!=3){reason="constant-buffer load not understood";return false;}
            const auto* r=resource_for_handle(final_value(a[1]));
            if(!r||r->resource_class!=2){reason="constant-buffer handle is unresolved";return false;}
            CbLoad l;l.var=i.lhs;l.kind=name.substr(Text("cbufferLoadLegacy.").size());l.reg=r->reg;l.fixed=integer(a[2],"i32",l.row);
            if(l.fixed&&(uint64_t(l.row)*16>=r->bytes)){reason="constant-buffer load starts beyond declared extent";return false;}
            cb_loads.push_back(l);
            if(expected==Stage::vertex&&l.reg==view_reg&&l.kind=="i32"&&l.fixed&&l.row!=160){
                if(++view_i32_count==1){view_parameter=l.var;mode_row=l.row;}
                else{view_parameter.clear();mode_row=308;}
            }
        }
        if(expected==Stage::vertex){
            if(view_reg==UINT32_MAX||primitive_reg==UINT32_MAX||local_reg==UINT32_MAX||
               primitive_bytes!=720||local_bytes<24||
               view_reg==primitive_reg||view_reg==local_reg||primitive_reg==local_reg){
                reason="View/Primitive/LocalVF CB roles are missing or unsupported";return false;
            }
            for(const auto& type:opaque_cb_types)for(const auto& l:module_lines)
                if(mentions_type(l,type)&&!starts(l,type+" = type ")){
                    reason="typed constant-buffer access is unsupported";return false;
                }
        }
        return true;
    }
};

std::vector<std::pair<Text,Text>> phi_values(const Text& rhs) {
    std::vector<std::pair<Text,Text>> out;
    for(size_t at=0;(at=rhs.find('[',at))!=Text::npos;){
        const auto end=rhs.find(']',at);if(end==Text::npos)return {};
        const auto pair=split(rhs.substr(at+1,end-at-1));
        if(pair.size()!=2||pair[1].size()<2||pair[1][0]!='%')return {};
        out.emplace_back(pair[0],pair[1].substr(1));at=end+1;
    }
    return out;
}
std::vector<Text> successors(const Instruction& i) {
    std::vector<Text> out;size_t at=0;
    while((at=i.rhs.find("label %",at))!=Text::npos){at+=7;const auto end=i.rhs.find_first_not_of("0123456789",at);out.push_back(i.rhs.substr(at,end-at));at=end;}
    return out;
}
bool pure_region(const Text& rhs) {
    if(starts(rhs,"br "))return true;
    if(starts(rhs,"ret ")||starts(rhs,"store ")||starts(rhs,"load ")||rhs.find("atomic")!=Text::npos)return false;
    if(rhs.find("call ")==Text::npos)return true;
    static const char* pure[]{"unary.","binary.","tertiary.","dot2.","dot3.","dot4.","cbufferLoadLegacy.","loadInput.",
        "evalSnapped.","evalSampleIndex.","evalCentroid.",
        "bufferLoad.","textureLoad.","sample.","sampleLevel.","sampleBias.","sampleGrad.","sampleCmp.","sampleCmpLevelZero."};
    if(starts(rhs,"call void "))return false;
    for(auto name:pure)if(rhs.find(Text("@dx.op.")+name)!=Text::npos)return true;
    return false;
}
struct NativeGateRegion {
    Text view,prim,bit,joined,origin,merge,entry;
    std::set<Text> region;
};
bool find_native_gate_region(const Module& m,uint32_t primitive,const std::map<Text,std::vector<size_t>>& uses,
                             NativeGateRegion& out) {
    auto field=[&](uint32_t reg,uint32_t offset)->Text {
        Text result;
        for(const auto& l:m.cb_loads){
            if(l.reg!=reg)continue;
            if(!l.fixed)return {};
            if(l.row!=offset/16)continue;
            const auto u=uses.find(l.var);if(u==uses.end())continue;
            for(const auto n:u->second){
                const auto& i=m.instructions[n];
                const auto prefix="extractvalue %dx.types.CBufRet."+l.kind+" "+l.var+", ";
                uint32_t lane=0;
                if(!starts(i.rhs,prefix)||!number(i.rhs.substr(prefix.size()),lane))return {};
                if(lane!=offset%16/4)continue;
                if(l.kind!="i32"||!result.empty())return {};
                result=i.lhs;
            }
        }
        return result;
    };
    out.view=field(m.view_reg,2560);out.prim=field(primitive,428);
    if(out.view.empty()||out.prim.empty())return false;
    const auto pu=uses.find(out.prim);if(pu==uses.end())return false;
    for(const auto n:pu->second){
        Text op,a,b;
        if(parse_i32_binop(m.instructions[n].rhs,op,a,b)&&op=="and"&&
           ((a==out.prim&&b=="1")||(b==out.prim&&a=="1"))){
            if(!out.bit.empty())return false;
            out.bit=m.instructions[n].lhs;
        }
    }
    if(out.bit.empty())return false;
    const auto bu=uses.find(out.bit);if(bu==uses.end()||bu->second.size()!=1)return false;
    for(const auto n:bu->second){
        Text op,a,b;
        if(parse_i32_binop(m.instructions[n].rhs,op,a,b)&&op=="or"&&
           ((a==out.bit&&b==out.view)||(b==out.bit&&a==out.view))){
            if(!out.joined.empty())return false;
            out.joined=m.instructions[n].lhs;
        }
    }
    if(out.joined.empty())return false;
    const auto ju=uses.find(out.joined);if(ju==uses.end()||ju->second.size()!=1)return false;
    Text condition;bool inverted=false;
    for(const auto n:ju->second){
        const auto rhs=strip_inst_metadata(m.instructions[n].rhs);
        if(rhs=="icmp eq i32 "+out.joined+", 0"){condition=m.instructions[n].lhs;inverted=false;}
        else if(rhs=="icmp ne i32 "+out.joined+", 0"){condition=m.instructions[n].lhs;inverted=true;}
    }
    if(condition.empty())return false;
    const auto cu=uses.find(condition);if(cu==uses.end()||cu->second.size()!=1)return false;
    const Instruction* branch=nullptr;
    for(const auto n:cu->second)if(starts(m.instructions[n].rhs,"br i1 "+condition+","))branch=&m.instructions[n];
    if(!branch)return false;
    const auto edges=successors(*branch);if(edges.size()!=2||edges[0]==edges[1])return false;
    out.merge=inverted?edges[1]:edges[0];
    out.entry=inverted?edges[0]:edges[1];
    out.origin=branch->block;
    std::vector<Text> pending{out.entry};
    while(!pending.empty()){
        const auto block=pending.back();pending.pop_back();
        if(block==out.merge)continue;
        if(block==out.origin||!m.blocks.count(block))return false;
        if(!out.region.insert(block).second)continue;
        const auto& body=m.blocks.at(block);if(body.empty())return false;
        const auto next=successors(m.instructions[body.back()]);if(next.empty())return false;
        pending.insert(pending.end(),next.begin(),next.end());
        if(out.region.size()>m.blocks.size())return false;
    }
    if(out.region.empty()||!m.blocks.count(out.merge))return false;
    // No edge from outside the velocity region may enter its interior.
    for(const auto& [block,body]:m.blocks){
        for(const auto& next:successors(m.instructions[body.back()]))
            if(out.region.count(next)&&!out.region.count(block)&&!(block==out.origin&&next==out.entry))return false;
    }
    return true;
}
bool gate_proof(const Module& m,uint32_t primitive,const Text& target_name,uint32_t target_index,
                const std::array<Text,4>& closed_output,const std::vector<size_t>& previous_inputs={}) {
    std::map<Text,std::vector<size_t>> uses;
    for(size_t n=0;n<m.instructions.size();++n)for(const auto& r:refs(m.instructions[n].rhs))uses[r].push_back(n);
    NativeGateRegion g;
    if(!find_native_gate_region(m,primitive,uses,g))return false;
    for(const auto& block:g.region)for(const auto n:m.blocks.at(block))
        if(!pure_region(m.instructions[n].rhs))return false;
    for(const auto& scalar:{g.view,g.prim})for(const auto n:uses[scalar]){
        const auto& i=m.instructions[n];
        if(g.region.count(i.block))continue;
        if(i.lhs!=g.bit&&i.lhs!=g.joined)return false;
    }
    std::map<Text,Text> seeds;
    for(const auto n:m.blocks.at(g.merge)){
        const auto& i=m.instructions[n];if(!starts(i.rhs,"phi "))continue;
        const auto values=phi_values(i.rhs);bool from_region=false;Text closed;
        for(const auto& [value,predecessor]:values){
            from_region|=g.region.count(predecessor)!=0;
            if(predecessor==g.origin&&(value=="0.000000e+00"||value=="1.000000e+00"))closed=value;
        }
        if(!from_region)continue;
        if(!starts(i.rhs,"phi float ")||values.size()!=2||closed.empty())return false;
        seeds.emplace(i.lhs,closed);
    }
    if(seeds.size()!=4)return false;
    for(const auto& block:g.region)for(const auto n:m.blocks.at(block)){
        const auto& i=m.instructions[n];if(i.lhs.empty())continue;
        for(const auto use:uses[i.lhs]){
            const auto& u=m.instructions[use];
            if(!g.region.count(u.block)&&!(u.block==g.merge&&seeds.count(u.lhs)))return false;
        }
    }
    std::map<Text,Text> constant_cache;std::set<Text> active;
    std::function<Text(const Text&)> constant=[&](const Text& value)->Text {
        if(value=="0.000000e+00"||value=="1.000000e+00")return value;
        if(const auto s=seeds.find(value);s!=seeds.end())return s->second;
        if(const auto c=constant_cache.find(value);c!=constant_cache.end())return c->second;
        const auto* d=m.def(value);if(!d||!active.insert(value).second)return {};
        std::vector<Text> operands;
        if(starts(d->rhs,"phi float "))for(const auto& p:phi_values(d->rhs))operands.push_back(p.first);
        else if(starts(d->rhs,"select i1 ")){
            const auto a=split(d->rhs);if(a.size()==3&&starts(a[1],"float ")&&starts(a[2],"float ")){operands.push_back(a[1].substr(6));operands.push_back(a[2].substr(6));}
        }
        Text result;
        for(const auto& operand:operands){const auto c=constant(operand);if(c.empty()||(!result.empty()&&result!=c)){result.clear();break;}result=c;}
        active.erase(value);constant_cache[value]=result;return result;
    };
    uint32_t target=UINT32_MAX;
    for(const auto& s:m.outputs)if(s.name==target_name&&s.indices.size()==1&&s.indices[0]==target_index&&s.columns==4)target=s.id;
    if(target==UINT32_MAX)return false;
    std::set<uint32_t> lanes;
    for(const auto& i:m.instructions){
        std::vector<Text> a;if(!m.call(i.rhs,"storeOutput.f32",a)||a.size()!=5)continue;
        uint32_t id=0,row=0,lane=0;
        if(!integer(a[1],"i32",id)||id!=target)continue;
        if(!integer(a[2],"i32",row)||row||!integer(a[3],"i8",lane)||lane>=4||!lanes.insert(lane).second||constant(final_value(a[4]))!=closed_output[lane])return false;
    }
    if(lanes.size()!=4)return false;
    std::set<Text> visited;std::vector<Text> todo;for(const auto& s:seeds)todo.push_back(s.first);
    while(!todo.empty()){
        const auto var=todo.back();todo.pop_back();if(!visited.insert(var).second)continue;
        for(const auto n:uses[var]){
            const auto& i=m.instructions[n];
            if(!i.lhs.empty()){
                if(!starts(i.rhs,"phi float ")&&!starts(i.rhs,"select i1 "))return false;
                todo.push_back(i.lhs);continue;
            }
            std::vector<Text> a;uint32_t id=0;
            if(!m.call(i.rhs,"storeOutput.f32",a)||a.size()!=5||!integer(a[1],"i32",id)||id!=target)return false;
        }
    }
    const auto numbered=[](const Text& value){
        uint32_t n=0;return !value.empty()&&value[0]=='%'&&number(value.substr(1),n);
    };
    visited.clear();
    for(const auto n:previous_inputs){
        if(!numbered(m.instructions[n].lhs))return false;
        todo.push_back(m.instructions[n].lhs);
    }
    while(!todo.empty()){
        const auto var=todo.back();todo.pop_back();if(!visited.insert(var).second)continue;
        for(const auto n:uses[var]){
            const auto& i=m.instructions[n];
            if(g.region.count(i.block))continue;
            if(numbered(i.lhs)&&pure_region(i.rhs)){todo.push_back(i.lhs);continue;}
            std::vector<Text> a;uint32_t id=0;
            if(!m.call(i.rhs,"storeOutput.f32",a)||a.size()!=5||!integer(a[1],"i32",id)||id!=target)return false;
        }
    }
    return true;
}
void bounds(const Module& m,Facts& out) {
    for(const auto& l:m.cb_loads){
        if(!l.fixed){out.dynamic_cb_registers.push_back(l.reg);continue;}
        uint32_t end=(l.row+1)*16;const auto* r=std::find_if(m.resources.begin(),m.resources.end(),[&](const auto& r){return r.resource_class==2&&r.reg==l.reg;}).operator->();
        if(end>r->bytes){
            // LocalVF is 24 bytes: the last DXIL aggregate uses only .xy.
            // Retain the conservative full row everywhere it is available.
            uint32_t lane_end=0;
            for(const auto& i:m.instructions)if(contains_ref(i.rhs,l.var)){
                const auto prefix="extractvalue %dx.types.CBufRet."+l.kind+" "+l.var+", ";uint32_t lane=0;
                if(!starts(i.rhs,prefix)||!number(i.rhs.substr(prefix.size()),lane)||lane>=4){lane_end=UINT32_MAX;break;}
                lane_end=std::max(lane_end,l.row*16+4*(lane+1));
            }
            end=lane_end;
        }
        out.cb_read_end[l.reg]=std::max(out.cb_read_end[l.reg],end);
    }
    std::sort(out.dynamic_cb_registers.begin(),out.dynamic_cb_registers.end());
    out.dynamic_cb_registers.erase(std::unique(out.dynamic_cb_registers.begin(),out.dynamic_cb_registers.end()),out.dynamic_cb_registers.end());
}

// Structural DXIL role extraction for universal UE4 LocalVF vertex shaders.
// Proves every per-vertex input/SRV/CB contract from executable SSA dataflow
// instead of comparing against hardcoded whole-program IR dumps.
bool extract_localvf_vertex(const Module& m,Facts& out) {
    // 1. Static CB bounds on all framework-touched CBs (View, Primitive, LocalVF).
    for(const auto reg:out.dynamic_cb_registers){
        if(reg==m.view_reg||reg==m.primitive_reg||reg==m.local_reg){
            out.reason="dynamic constant-buffer access on View/Primitive/LocalVF";return false;
        }
    }
    for(const auto& r:m.resources)if(r.resource_class==2&&out.cb_read_end[r.reg]>r.bytes){
        out.reason="required constant-buffer bytes exceed declaration";return false;
    }
    if(out.cb_read_end[m.local_reg]>24){
        out.reason="LocalVF CB read exceeds 24-byte contract";return false;
    }
    out.layout.view_reg=m.view_reg;
    out.layout.primitive_reg=m.primitive_reg;
    out.layout.local_reg=m.local_reg;
    out.layout.view_mode_row=m.mode_row;

    // 2. Reject UAVs and write/stream side-effect ops in vertex stage.
    for(const auto& r:m.resources)if(r.resource_class==1){
        out.reason="vertex shader declares UAV resource";return false;
    }
    for(const auto& i:m.instructions){
        if(i.rhs.find("@dx.op.bufferStore")!=Text::npos||i.rhs.find("@dx.op.textureStore")!=Text::npos||
           i.rhs.find("@dx.op.atomic")!=Text::npos||i.rhs.find("@dx.op.emit")!=Text::npos||
           i.rhs.find("@dx.op.cut")!=Text::npos){
            out.reason="vertex shader executes unsupported write/side-effect op";return false;
        }
    }

    // 3. Verify input and output signatures.
    const Signature* pos_sig=nullptr;
    const Signature* vid_sig=nullptr;
    for(const auto& s:m.inputs){
        if(s.name=="ATTRIBUTE"&&s.indices.size()==1&&s.indices[0]==0&&s.rows==1&&s.columns>=3&&s.component==9)pos_sig=&s;
        if(s.name=="SV_VertexID"&&s.kind==1&&s.rows==1&&s.columns==1)vid_sig=&s;
    }
    if(!pos_sig){out.reason="VS missing ATTRIBUTE0 float3 position input";return false;}
    if(!vid_sig){out.reason="VS missing SV_VertexID input";return false;}
    bool has_sv_pos=false;
    for(const auto& s:m.outputs){
        if(s.name=="SV_Position"&&s.kind==3&&s.rows==1&&s.columns==4)has_sv_pos=true;
    }
    if(!has_sv_pos){out.reason="VS missing SV_Position output";return false;}

    // Validate every actual loadInput instruction: only VB0 ATTRIBUTE0 and system values are allowed.
    std::set<Text> vid_vars;
    for(const auto& i:m.instructions){
        const auto at=i.rhs.find("@dx.op.loadInput.");if(at==Text::npos)continue;
        const auto open=i.rhs.find('(',at+7);if(open==Text::npos){out.reason="VS loadInput malformed";return false;}
        const auto op=i.rhs.substr(at+7,open-at-7);
        std::vector<Text> a;uint32_t opcode=0,id=0,row=0,col=0;
        if(!m.call(i.rhs,op,a)||a.size()!=5||!integer(a[0],"i32",opcode)||opcode!=4||
           !integer(a[1],"i32",id)||!integer(a[2],"i32",row)||!integer(a[3],"i8",col)){
            out.reason="VS loadInput operands not understood";return false;
        }
        const auto s=std::find_if(m.inputs.begin(),m.inputs.end(),[&](const auto& sig){return sig.id==id;});
        if(s==m.inputs.end()){out.reason="VS loadInput references unknown signature id";return false;}
        if(s->id==vid_sig->id){
            if(row==0&&col==0&&!i.lhs.empty())vid_vars.insert(i.lhs);
            continue;
        }
        if(s->id==pos_sig->id)continue;
        if(s->kind!=0)continue; // System values (e.g. SV_InstanceID) are rasterizer/IA generated.
        out.reason="VS reads unsupported non-VB0 vertex attribute "+s->name;
        return false;
    }
    if(vid_vars.empty()){out.reason="VS never loads SV_VertexID";return false;}

    // 4. Build SSA use-def index and extract LocalVF[0] lanes + SV_VertexID closure.
    std::map<Text,std::vector<size_t>> uses;
    for(size_t n=0;n<m.instructions.size();++n)
        for(const auto& r:refs(m.instructions[n].rhs))uses[r].push_back(n);

    std::array<std::set<Text>,4> lvf_lane;
    for(const auto& l:m.cb_loads){
        if(l.reg!=m.local_reg||!l.fixed||l.row!=0)continue;
        if(l.kind=="i32"){
            for(const auto n:uses[l.var]){
                const auto& i=m.instructions[n];
                const auto prefix="extractvalue %dx.types.CBufRet.i32 "+l.var+", ";
                uint32_t lane=0;
                if(starts(i.rhs,prefix)&&number(strip_inst_metadata(i.rhs.substr(prefix.size())),lane)&&lane<4&&!i.lhs.empty())
                    lvf_lane[lane].insert(i.lhs);
            }
        }else if(l.kind=="f32"){
            for(const auto n:uses[l.var]){
                const auto& i=m.instructions[n];
                const auto prefix="extractvalue %dx.types.CBufRet.f32 "+l.var+", ";
                uint32_t lane=0;
                if(!starts(i.rhs,prefix)||!number(strip_inst_metadata(i.rhs.substr(prefix.size())),lane)||lane>=4||i.lhs.empty())continue;
                const auto bc_rhs="bitcast float "+i.lhs+" to i32";
                for(const auto un:uses[i.lhs]){
                    const auto& ui=m.instructions[un];
                    if(strip_inst_metadata(ui.rhs)==bc_rhs&&!ui.lhs.empty())lvf_lane[lane].insert(ui.lhs);
                }
            }
        }
    }

    // Propagate SV_VertexID / LocalVF-index dependency strictly along integer address/index dataflow.
    // Never propagate across resource/memory loads (bufferLoad/rawBufferLoad/textureLoad/sample/cbufferLoad)
    // or floating-point math/conversions: in UE4 forward/translucent vertex shaders, LocalVF UV/Tangent/Color
    // floats flow into WPO / WorldPosition / forward light-grid coordinates and then into indirect scene buffers
    // (e.g. CulledLightDataGrid -> light_idx * 3 -> ForwardLocalLightBuffer), which are scene lighting buffers
    // that must remain untouched in the native VS descriptor table rather than being mistaken for LocalVF streams.
    auto is_int_addr_inst=[](const Text& raw_rhs)->bool {
        auto rhs=strip_inst_metadata(raw_rhs);
        auto sp=rhs.find(' ');
        if(sp==Text::npos)return false;
        auto op=rhs.substr(0,sp);
        if(op=="tail"||op=="musttail"){
            rhs=trim(rhs.substr(sp+1));
            sp=rhs.find(' ');
            if(sp==Text::npos)return false;
            op=rhs.substr(0,sp);
        }
        if(op=="add"||op=="sub"||op=="mul"||op=="udiv"||op=="sdiv"||op=="urem"||op=="srem"||
           op=="shl"||op=="lshr"||op=="ashr"||op=="and"||op=="or"||op=="xor"||
           op=="zext"||op=="sext"||op=="trunc"||op=="icmp"){
            return true;
        }
        if(op=="phi"){
            const auto rest=trim(rhs.substr(sp+1));
            return starts(rest,"i1 ")||starts(rest,"i8 ")||starts(rest,"i16 ")||starts(rest,"i32 ")||starts(rest,"i64 ");
        }
        if(op=="select"){
            const auto parts=split(rhs);
            if(parts.size()!=3)return false;
            const auto& v1=parts[1];
            return starts(v1,"i1 ")||starts(v1,"i8 ")||starts(v1,"i16 ")||starts(v1,"i32 ")||starts(v1,"i64 ");
        }
        if(op=="call"){
            const auto rest=trim(rhs.substr(sp+1));
            const bool int_ret=starts(rest,"i1 ")||starts(rest,"i8 ")||starts(rest,"i16 ")||starts(rest,"i32 ")||starts(rest,"i64 ");
            if(!int_ret||rest.find("@dx.op.")==Text::npos)return false;
            return rest.find("bufferLoad")==Text::npos&&rest.find("rawBufferLoad")==Text::npos&&
                   rest.find("textureLoad")==Text::npos&&rest.find("sample")==Text::npos&&
                   rest.find("cbufferLoad")==Text::npos&&rest.find("loadInput")==Text::npos;
        }
        return false;
    };
    auto forward_closure=[&](const std::set<Text>& seeds){
        std::set<Text> dep=seeds;std::vector<Text> q(seeds.begin(),seeds.end());
        while(!q.empty()){
            const auto v=q.back();q.pop_back();
            const auto u=uses.find(v);if(u==uses.end())continue;
            for(const auto n:u->second){
                const auto& inst=m.instructions[n];
                if(inst.lhs.empty()||!is_int_addr_inst(inst.rhs))continue;
                if(dep.insert(inst.lhs).second)q.push_back(inst.lhs);
            }
        }
        return dep;
    };
    const auto vid_dep=forward_closure(vid_vars);
    const auto voff_param_dep=forward_closure(lvf_lane[3]);
    const auto color_mask_dep=forward_closure(lvf_lane[0]);

    // Algebraic matchers for LocalVF index expressions.
    std::function<Text(const Text&,unsigned)> unwrap=[&](const Text& v,unsigned depth)->Text {
        if(depth>=6)return v;
        const auto* d=m.def(v);if(!d)return v;
        Text op,a,b;
        if(parse_i32_binop(d->rhs,op,a,b)){
            if(op=="add"||op=="or"||op=="xor"){
                if(b=="0")return unwrap(a,depth+1);
                if(a=="0")return unwrap(b,depth+1);
            }else if(op=="and"){
                if(b=="-1"||b=="4294967295")return unwrap(a,depth+1);
                if(a=="-1"||a=="4294967295")return unwrap(b,depth+1);
            }else if(op=="mul"){
                if(b=="1")return unwrap(a,depth+1);
                if(a=="1")return unwrap(b,depth+1);
            }else if((op=="shl"||op=="lshr"||op=="ashr")&&b=="0"){
                return unwrap(a,depth+1);
            }
        }
        return v;
    };
    auto is_vid=[&](const Text& raw){return vid_vars.count(unwrap(raw,0))!=0;};
    auto is_voff_param=[&](const Text& raw){return lvf_lane[3].count(unwrap(raw,0))!=0;};
    auto is_uv_stride=[&](const Text& raw){return lvf_lane[1].count(unwrap(raw,0))!=0;};
    auto is_color_mask_expr=[&](const Text& raw){
        const auto u=unwrap(raw,0);
        return !vid_dep.count(u)&&color_mask_dep.count(u)!=0;
    };
    auto parse_umin_i32=[&](const Text& raw_rhs,Text& a,Text& b)->bool {
        std::vector<Text> args;uint32_t opcode=0;
        if(!m.call(raw_rhs,"binary.i32",args)||args.size()!=3||!integer(args[0],"i32",opcode)||opcode!=40)return false;
        if(!starts(args[1],"i32 ")||!starts(args[2],"i32 "))return false;
        a=trim(args[1].substr(4));b=trim(args[2].substr(4));
        return !a.empty()&&!b.empty();
    };

    // Symbolic affine-in-SV_VertexID evaluator for LocalVF TypedBuffer index expressions.
    // Soundly normalizes LLVM InstCombine / Reassociate transformations (e.g. `((vid << 1) | 1) + vid`,
    // `(vid + vid) + vid`, `(vid + voff) * 3 + k`, `((vid << 1) + (voff << 1)) | 1`) without weakening
    // structural safety checks (arbitrary multipliers like `vid * 5` remain strictly rejected).
    auto parse_i32_const=[](const Text& s,int32_t& out)->bool {
        if(s.empty())return false;
        if(s[0]=='-'){
            uint32_t mag=0;
            if(!number(s.substr(1),mag)||mag>2147483648u)return false;
            out=mag==2147483648u?INT32_MIN:-int32_t(mag);
            return true;
        }
        uint32_t mag=0;
        if(!number(s,mag))return false;
        out=static_cast<int32_t>(mag);
        return true;
    };
    auto ctz32=[](uint32_t v)->uint32_t {
        if(!v)return 32u;
        uint32_t n=0;while(!(v&1u)){v>>=1;++n;}return n;
    };
    struct LinExpr {
        bool valid=false;
        int32_t vid=0,voff=0,c=0;
        bool has_non_vid_var=false,non_vid_uses_voff=false;
        uint32_t low_zero_bits=0;
    };
    std::map<Text,LinExpr> lin_cache;
    std::set<Text> lin_active;
    std::function<LinExpr(const Text&,unsigned)> eval_lin=[&](const Text& raw,unsigned depth)->LinExpr {
        const auto v=unwrap(raw,0);
        if(const auto it=lin_cache.find(v);it!=lin_cache.end())return it->second;
        LinExpr res;
        int32_t k=0;
        if(parse_i32_const(v,k)){
            res={true,0,0,k,false,false,ctz32(uint32_t(k))};
            return res;
        }
        if(is_vid(v)){res={true,1,0,0,false,false,0};return res;}
        if(is_voff_param(v)){res={true,0,1,0,false,true,0};return res;}
        const bool non_vid=!vid_dep.count(v);
        if(non_vid&&!voff_param_dep.count(v)){
            res={true,0,0,0,true,false,0};
            return res;
        }
        if(depth>=10||!lin_active.insert(v).second){
            if(non_vid)return {true,0,0,0,true,true,0};
            return res;
        }
        const auto* d=m.def(v);
        if(d){
            Text op,a,b;
            if(parse_i32_binop(d->rhs,op,a,b)){
                const auto la=eval_lin(a,depth+1);
                const auto lb=eval_lin(b,depth+1);
                if(la.valid&&lb.valid){
                    if(op=="add"){
                        res.valid=true;
                        res.vid=la.vid+lb.vid;
                        res.voff=la.voff+lb.voff;
                        res.c=la.c+lb.c;
                        res.has_non_vid_var=la.has_non_vid_var||lb.has_non_vid_var;
                        res.non_vid_uses_voff=la.non_vid_uses_voff||lb.non_vid_uses_voff;
                        res.low_zero_bits=std::min(la.low_zero_bits,lb.low_zero_bits);
                    }else if(op=="sub"&&!lb.has_non_vid_var){
                        res.valid=true;
                        res.vid=la.vid-lb.vid;
                        res.voff=la.voff-lb.voff;
                        res.c=la.c-lb.c;
                        res.has_non_vid_var=la.has_non_vid_var;
                        res.non_vid_uses_voff=la.non_vid_uses_voff;
                        res.low_zero_bits=std::min(la.low_zero_bits,lb.low_zero_bits);
                    }else if(op=="shl"&&!lb.has_non_vid_var&&!lb.vid&&!lb.voff&&lb.c>=0&&lb.c<=4){
                        if(!la.has_non_vid_var||la.vid==0){
                            const unsigned sh=unsigned(lb.c);
                            res.valid=true;
                            res.vid=la.vid<<sh;
                            res.voff=la.voff<<sh;
                            res.c=la.c<<sh;
                            res.has_non_vid_var=la.has_non_vid_var;
                            res.non_vid_uses_voff=la.non_vid_uses_voff;
                            res.low_zero_bits=std::min(32u,la.low_zero_bits+sh);
                        }
                    }else if(op=="mul"){
                        const auto scale=[&](const LinExpr& base,int32_t mult){
                            if(mult<-16||mult>16)return;
                            if(base.has_non_vid_var&&base.vid!=0)return;
                            res.valid=true;
                            res.vid=base.vid*mult;
                            res.voff=base.voff*mult;
                            res.c=base.c*mult;
                            res.has_non_vid_var=base.has_non_vid_var;
                            res.non_vid_uses_voff=base.non_vid_uses_voff;
                            res.low_zero_bits=std::min(32u,base.low_zero_bits+ctz32(uint32_t(mult)));
                        };
                        if(!lb.has_non_vid_var&&!lb.vid&&!lb.voff)scale(la,lb.c);
                        else if(!la.has_non_vid_var&&!la.vid&&!la.voff)scale(lb,la.c);
                    }else if(op=="or"||op=="xor"){
                        // Bitwise OR/XOR equals ADD whenever the constant operand only touches bits known to be 0.
                        const auto try_or_add=[&](const LinExpr& base,const LinExpr& mask){
                            if(mask.has_non_vid_var||mask.vid||mask.voff||mask.c<0)return false;
                            if(base.low_zero_bits==0)return false;
                            const uint32_t limit=base.low_zero_bits>=31?uint32_t(INT32_MAX):(1u<<base.low_zero_bits);
                            if(uint32_t(mask.c)>=limit)return false;
                            res.valid=true;
                            res.vid=base.vid;
                            res.voff=base.voff;
                            res.c=base.c+mask.c;
                            res.has_non_vid_var=base.has_non_vid_var;
                            res.non_vid_uses_voff=base.non_vid_uses_voff;
                            res.low_zero_bits=std::min(base.low_zero_bits,ctz32(uint32_t(mask.c)));
                            return true;
                        };
                        if(!try_or_add(la,lb))try_or_add(lb,la);
                    }
                }
            }
        }
        if(!res.valid&&non_vid){
            res={true,0,0,0,true,true,0};
        }
        lin_active.erase(v);
        lin_cache.emplace(v,res);
        return res;
    };

    auto is_v_off=[&](const Text& raw){
        const auto l=eval_lin(raw,0);
        return l.valid&&!l.has_non_vid_var&&l.vid==1&&l.voff==1&&l.c==0;
    };
    auto is_vid_or_voff=[&](const Text& raw){
        const auto l=eval_lin(raw,0);
        return l.valid&&!l.has_non_vid_var&&l.vid==1&&(l.voff==0||l.voff==1)&&l.c==0;
    };
    auto is_uv_base=[&](const Text& raw){
        const auto v=unwrap(raw,0);
        const auto* d=m.def(v);if(!d)return false;
        Text op,a,b;
        return parse_i32_binop(d->rhs,op,a,b)&&op=="mul"&&
            ((is_vid_or_voff(a)&&is_uv_stride(b))||(is_vid_or_voff(b)&&is_uv_stride(a)));
    };
    auto is_vid_times_stride=[&](const Text& raw){
        const auto v=unwrap(raw,0);
        const auto* d=m.def(v);if(!d)return false;
        Text op,a,b;
        return parse_i32_binop(d->rhs,op,a,b)&&op=="mul"&&
            ((is_vid(a)&&is_uv_stride(b))||(is_vid(b)&&is_uv_stride(a)));
    };
    std::function<bool(const Text&,unsigned)> is_uv_index_rec=[&](const Text& raw,unsigned depth)->bool {
        if(depth>=5)return false;
        const auto v=unwrap(raw,0);
        if(is_uv_base(v))return true;
        // Constant small UV stride (1..4 UV pairs) on (vid + voff) or vid.
        const auto lin=eval_lin(v,0);
        if(lin.valid&&lin.vid>=1&&lin.vid<=4&&(lin.voff==0||lin.voff==lin.vid)){
            if(!lin.has_non_vid_var&&lin.c>=0&&lin.c<lin.vid)return true;
            if(lin.has_non_vid_var&&(lin.non_vid_uses_voff||!lin.voff))return true;
        }
        const auto* d=m.def(v);if(!d)return false;
        Text op,a,b;
        if(!parse_i32_binop(d->rhs,op,a,b)||(op!="add"&&op!="or"))return false;
        if((!vid_dep.count(b)&&is_uv_index_rec(a,depth+1))||(!vid_dep.count(a)&&is_uv_index_rec(b,depth+1)))return true;
        // Handle LLVM distributive reassociation: (vid * stride) + (voff * stride + chan)
        if(op=="add"&&((is_vid_times_stride(a)&&!vid_dep.count(b)&&voff_param_dep.count(b))||
           (is_vid_times_stride(b)&&!vid_dep.count(a)&&voff_param_dep.count(a))))return true;
        return false;
    };
    auto is_uv_index=[&](const Text& raw){return is_uv_index_rec(raw,0);};
    std::function<bool(const Text&,unsigned)> is_tangent_index_rec=[&](const Text& raw,unsigned depth)->bool {
        if(depth>=4)return false;
        const auto v=unwrap(raw,0);
        const auto l=eval_lin(v,0);
        if(l.valid&&l.vid==2){
            if(!l.has_non_vid_var&&(l.voff==0||l.voff==2)&&(l.c==0||l.c==1))return true;
            if(l.has_non_vid_var&&l.non_vid_uses_voff&&(l.c==0||l.c==1))return true;
        }
        const auto* d=m.def(v);if(!d)return false;
        const auto rhs=strip_inst_metadata(d->rhs);
        Text a,b;
        if(parse_umin_i32(rhs,a,b)){
            if((!vid_dep.count(b)&&is_tangent_index_rec(a,depth+1))||(!vid_dep.count(a)&&is_tangent_index_rec(b,depth+1)))return true;
        }
        if(starts(rhs,"select i1 ")){
            const auto parts=split(rhs);
            if(parts.size()==3&&!vid_dep.count(final_value(parts[0]))&&starts(parts[1],"i32 ")&&starts(parts[2],"i32 ")){
                const auto ta=trim(parts[1].substr(4)),tb=trim(parts[2].substr(4));
                const bool a_ok=(ta=="undef"||is_tangent_index_rec(ta,depth+1));
                const bool b_ok=(tb=="undef"||is_tangent_index_rec(tb,depth+1));
                if(a_ok&&b_ok&&(ta!="undef"||tb!="undef"))return true;
            }
        }
        if(starts(rhs,"phi i32 ")){
            const auto vals=phi_values(rhs);
            if(!vals.empty()){
                bool any_ok=false,all_ok=true;
                for(const auto& [val,_]:vals){
                    if(val=="undef")continue;
                    if(is_tangent_index_rec(val,depth+1)){any_ok=true;continue;}
                    all_ok=false;break;
                }
                if(all_ok&&any_ok)return true;
            }
        }
        return false;
    };
    auto is_tangent_index=[&](const Text& raw){return is_tangent_index_rec(raw,0);};
    std::function<bool(const Text&,unsigned)> is_aux_index_rec=[&](const Text& raw,unsigned depth)->bool {
        if(depth>=5)return false;
        const auto v=unwrap(raw,0);
        if(is_vid_or_voff(v))return true;
        const auto* d=m.def(v);if(!d)return false;
        const auto rhs=strip_inst_metadata(d->rhs);
        Text op,a,b;
        if(parse_i32_binop(rhs,op,a,b)&&op=="and"){
            if((is_color_mask_expr(b)&&is_aux_index_rec(a,depth+1))||
               (is_color_mask_expr(a)&&is_aux_index_rec(b,depth+1)))return true;
            int32_t mk=0;
            if((parse_i32_const(b,mk)&&(is_v_off(a)||(uint32_t(mk)>=0xFFFFu&&is_aux_index_rec(a,depth+1))))||
               (parse_i32_const(a,mk)&&(is_v_off(b)||(uint32_t(mk)>=0xFFFFu&&is_aux_index_rec(b,depth+1)))))return true;
        }
        if(parse_umin_i32(rhs,a,b)){
            if((!vid_dep.count(b)&&is_aux_index_rec(a,depth+1))||(!vid_dep.count(a)&&is_aux_index_rec(b,depth+1)))return true;
        }
        if(starts(rhs,"select i1 ")){
            const auto parts=split(rhs);
            if(parts.size()==3&&!vid_dep.count(final_value(parts[0]))&&starts(parts[1],"i32 ")&&starts(parts[2],"i32 ")){
                const auto ta=trim(parts[1].substr(4)),tb=trim(parts[2].substr(4));
                const bool a_idle=(ta=="0"||ta=="undef"),b_idle=(tb=="0"||tb=="undef");
                const bool a_ok=(a_idle||is_aux_index_rec(ta,depth+1));
                const bool b_ok=(b_idle||is_aux_index_rec(tb,depth+1));
                if(a_ok&&b_ok&&(!a_idle||!b_idle))return true;
            }
        }
        if(starts(rhs,"phi i32 ")){
            const auto vals=phi_values(rhs);
            if(!vals.empty()){
                bool any_vid=false,all_ok=true;
                for(const auto& [val,_]:vals){
                    if(val=="0"||val=="undef")continue;
                    if(is_aux_index_rec(val,depth+1)){any_vid=true;continue;}
                    all_ok=false;break;
                }
                if(all_ok&&any_vid)return true;
            }
        }
        return false;
    };
    auto is_aux_index=[&](const Text& raw){return is_aux_index_rec(raw,0);};
    auto is_prev_index=[&](const Text& raw){
        const auto l=eval_lin(raw,0);
        if(!l.valid||l.vid!=3)return false;
        if(!l.has_non_vid_var)return (l.voff==0||l.voff==3)&&(l.c>=0&&l.c<=2);
        return l.non_vid_uses_voff&&(l.c>=0&&l.c<=2);
    };

    // 5. Classify every SRV resource declared in the vertex shader.
    std::vector<size_t> aux_loads,prev_loads;
    for(const auto& r:m.resources){
        if(r.resource_class!=0)continue;
        std::vector<Text> srv_handles;
        for(const auto& [h,pair]:m.handles)if(pair.first==0&&pair.second==r.range_id)srv_handles.push_back(h);
        std::vector<size_t> srv_uses;
        for(const auto& h:srv_handles){
            const auto u=uses.find(h);
            if(u!=uses.end())srv_uses.insert(srv_uses.end(),u->second.begin(),u->second.end());
        }
        std::sort(srv_uses.begin(),srv_uses.end());
        srv_uses.erase(std::unique(srv_uses.begin(),srv_uses.end()),srv_uses.end());
        if(srv_uses.empty())continue; // Unused SRV declaration.

        const auto shape_it=m.srv_shapes.find(r.reg);
        const uint32_t shape=shape_it==m.srv_shapes.end()?0:shape_it->second;
        if(shape>=1&&shape<=9){
            // Material textures (Texture1D/2D/2DArray/3D/Cube/MS) sampled in VS (e.g. WPO/wind/LUT)
            // are preserved untouched in the copied VS descriptor table.
            continue;
        }
        if(shape==11||shape==12){
            // RawBuffer / StructuredBuffer: allow only if NOT indexed by SV_VertexID.
            for(const auto n:srv_uses){
                const auto& rhs=m.instructions[n].rhs;
                for(const auto& v:refs(rhs)){
                    if(vid_dep.count(v)){
                        out.reason="VS reads SV_VertexID-indexed Raw/StructuredBuffer t"+std::to_string(r.reg);
                        return false;
                    }
                }
            }
            continue;
        }
        if(shape!=10){
            out.reason="VS declares unsupported SRV shape on t"+std::to_string(r.reg);
            return false;
        }

        // TypedBuffer (shape == 10): inspect all bufferLoad instructions.
        bool any_vid_dep=false,all_f32=true;
        std::vector<std::pair<size_t,Text>> loads;
        for(const auto n:srv_uses){
            const auto& rhs=m.instructions[n].rhs;
            if(rhs.find("@dx.op.getDimensions")!=Text::npos)continue;
            const auto at=rhs.find("@dx.op.bufferLoad.");
            if(at==Text::npos){
                out.reason="VS performs unsupported op on TypedBuffer t"+std::to_string(r.reg);
                return false;
            }
            const auto open=rhs.find('(',at+7);
            const auto op=open==Text::npos?Text{}:rhs.substr(at+7,open-at-7);
            std::vector<Text> a;uint32_t opcode=0;
            if(!m.call(rhs,op,a)||a.size()<4||!integer(a[0],"i32",opcode)||opcode!=68){
                out.reason="VS bufferLoad operands not understood on t"+std::to_string(r.reg);
                return false;
            }
            if(op!="bufferLoad.f32")all_f32=false;
            const auto idx=final_value(a[2]);
            if(vid_dep.count(idx))any_vid_dep=true;
            loads.emplace_back(n,idx);
        }
        if(loads.empty()||!any_vid_dep)continue; // Non-per-vertex material buffer, untouched.
        if(!all_f32){
            out.reason="VS per-vertex TypedBuffer t"+std::to_string(r.reg)+" is not f32";
            return false;
        }

        const bool is_float2=r.type.find("vector<float, 2>")!=Text::npos;
        const bool is_float4=r.type.find("vector<float, 4>")!=Text::npos;
        const bool is_float1=r.type.find("class.Buffer<float>")!=Text::npos;

        auto all_match=[&](auto pred){
            return std::all_of(loads.begin(),loads.end(),[&](const auto& p){return pred(p.second);});
        };

        auto is_aux_or_zero=[&](const Text& s){return s=="0"||is_aux_index(s);};
        if(is_float2&&all_match(is_uv_index)){
            if(out.uv>=0){out.reason="VS has multiple LocalVF UV buffers";return false;}
            out.uv=int(r.reg);
        }else if(is_float4&&all_match(is_tangent_index)){
            if(out.tangent>=0){out.reason="VS has multiple LocalVF Tangent buffers";return false;}
            out.tangent=int(r.reg);
        }else if(is_float4&&all_match(is_aux_or_zero)){
            if(out.auxiliary>=0){out.reason="VS has multiple LocalVF Auxiliary/Color buffers";return false;}
            out.auxiliary=int(r.reg);
            for(const auto& p:loads)aux_loads.push_back(p.first);
        }else if(is_float1&&all_match(is_prev_index)){
            if(out.previous>=0){out.reason="VS has multiple LocalVF PreviousPosition buffers";return false;}
            out.previous=int(r.reg);
            for(const auto& p:loads)prev_loads.push_back(p.first);
        }else{
            auto clean_diag=[](Text s,size_t cap){
                for(char& ch:s)if(ch=='"'||ch=='\r'||ch=='\n'||ch=='\t')ch='\'';
                if(s.size()>cap)s.resize(cap);
                return s;
            };
            const char* short_ty=is_float2?"f2":is_float4?"f4":is_float1?"f1":"?";
            // Pick the first load index that failed the expected matcher so even the 96-byte
            // admission summary immediately identifies the exact unrecognized SSA pattern.
            Text fail_idx=loads.front().second;
            for(const auto& p:loads){
                if(is_float4&&!is_tangent_index(p.second)&&!is_aux_or_zero(p.second)){fail_idx=p.second;break;}
                if(is_float2&&!is_uv_index(p.second)){fail_idx=p.second;break;}
                if(is_float1&&!is_prev_index(p.second)){fail_idx=p.second;break;}
            }
            Text diag="VS unrec t"+std::to_string(r.reg)+" "+short_ty+" n="+std::to_string(loads.size())+
                " idx="+fail_idx;
            if(const auto* d0=m.def(fail_idx)){
                const auto r0=strip_inst_metadata(d0->rhs);
                diag+="=["+clean_diag(r0,96)+"]";
                unsigned shown=0;
                for(const auto& rv:refs(r0)){
                    if(const auto* d1=m.def(rv)){
                        diag+=" "+rv+"=["+clean_diag(strip_inst_metadata(d1->rhs),80)+"]";
                        if(++shown>=2)break;
                    }
                }
            }
            diag+=" roles="+std::to_string(out.uv)+","+std::to_string(out.tangent)+","+
                std::to_string(out.auxiliary)+","+std::to_string(out.previous)+
                " lvf="+std::to_string(lvf_lane[0].size())+","+std::to_string(lvf_lane[1].size())+","+
                std::to_string(lvf_lane[2].size())+","+std::to_string(lvf_lane[3].size())+
                " ty="+clean_diag(r.type,48);
            out.reason=std::move(diag);
            return false;
        }
    }

    if(out.uv<0&&out.tangent<0&&out.auxiliary<0&&out.previous<0){
        out.reason="VS reads no LocalVF per-vertex SRV stream";return false;
    }

    // 6. Determine auxiliary_requires_native_color structurally:
    // If the aux buffer directly populates all 4 lanes of a COLOR0 output semantic,
    // the shader itself proves the aux view is vertex color (direct=true -> requires_native_color=false).
    // Otherwise (e.g. aux consumed only internally for WPO/outline), conservatively require native color proof.
    if(out.auxiliary>=0){
        std::set<Text> aux_extracted;
        for(const auto n:aux_loads){
            const auto& var=m.instructions[n].lhs;if(var.empty())continue;
            for(const auto un:uses[var]){
                const auto& i=m.instructions[un];
                const auto prefix="extractvalue %dx.types.ResRet.f32 "+var+", ";
                uint32_t lane=0;
                if(starts(i.rhs,prefix)&&number(strip_inst_metadata(i.rhs.substr(prefix.size())),lane)&&lane<4&&!i.lhs.empty())
                    aux_extracted.insert(i.lhs);
            }
        }
        bool direct_color=false;
        for(const auto& s:m.outputs){
            if(s.name!="COLOR"||s.indices.size()!=1||s.indices[0]!=0||s.columns!=4||s.component!=9)continue;
            std::set<uint32_t> lanes;
            for(const auto& i:m.instructions){
                std::vector<Text> a;uint32_t id=0,row=0,lane=0;
                if(!m.call(i.rhs,"storeOutput.f32",a)||a.size()!=5)continue;
                if(!integer(a[1],"i32",id)||id!=s.id||!integer(a[2],"i32",row)||row||!integer(a[3],"i8",lane)||lane>=4)continue;
                if(aux_extracted.count(final_value(a[4])))lanes.insert(lane);
            }
            if(lanes.size()==4){direct_color=true;break;}
        }
        out.auxiliary_requires_native_color=!direct_color;
    }

    // 7. Prove previous_only_velocity, native_gate_zero_velocity, and reads_previous gate soundness.
    if(out.previous<0){
        out.previous_only_velocity=true;
        out.native_gate_zero_velocity=false;
    }else{
        uint32_t vel_id=UINT32_MAX;
        for(const auto& s:m.outputs)
            if(s.name=="VELOCITY_PREV_POS"&&s.indices.size()==1&&s.indices[0]==0&&s.columns==4)vel_id=s.id;
        bool prev_only_vel=vel_id!=UINT32_MAX;
        if(prev_only_vel){
            const auto numbered=[](const Text& value){
                uint32_t n=0;return !value.empty()&&value[0]=='%'&&number(value.substr(1),n);
            };
            std::set<Text> visited;std::vector<Text> todo;
            for(const auto n:prev_loads){
                if(!numbered(m.instructions[n].lhs)){prev_only_vel=false;break;}
                todo.push_back(m.instructions[n].lhs);
            }
            while(prev_only_vel&&!todo.empty()){
                const auto var=todo.back();todo.pop_back();
                if(!visited.insert(var).second)continue;
                for(const auto n:uses[var]){
                    const auto& i=m.instructions[n];
                    if(!i.lhs.empty()){
                        if(numbered(i.lhs)&&(pure_region(i.rhs)||starts(i.rhs,"phi ")||starts(i.rhs,"select ")||starts(i.rhs,"extractvalue "))){
                            todo.push_back(i.lhs);continue;
                        }
                        prev_only_vel=false;break;
                    }
                    std::vector<Text> a;uint32_t id=0;
                    if(!m.call(i.rhs,"storeOutput.f32",a)||a.size()!=5||!integer(a[1],"i32",id)||id!=vel_id){
                        prev_only_vel=false;break;
                    }
                }
            }
        }
        out.previous_only_velocity=prev_only_vel;

        NativeGateRegion gate;
        const bool have_gate=find_native_gate_region(m,m.primitive_reg,uses,gate);
        const bool all_prev_in_gate=have_gate&&std::all_of(prev_loads.begin(),prev_loads.end(),[&](size_t n){
            return gate.region.count(m.instructions[n].block)!=0;
        });
        if(all_prev_in_gate){
            out.native_gate_zero_velocity=gate_proof(m,m.primitive_reg,"VELOCITY_PREV_POS",0,
                {"0.000000e+00","0.000000e+00","0.000000e+00","1.000000e+00"});
            if(out.native_gate_zero_velocity)out.native_gate_closed_output={0,0,0,1};
        }else{
            // Previous buffer is read outside the native ((Primitive[428]&1)|View[2560]) gate.
            // Signal rt_localvf_inputs to bind real previous unconditionally and disable no-history.
            out.native_gate_zero_velocity=false;
            out.layout.view_gate_offset=UINT32_MAX;
        }
    }

    out.all_resource_reads_modelled=true;
    out.recognition=Recognition::executable_model;
    out.adaptation=Adaptation::layout_instance;
    out.model="structural-localvf:uv="+std::to_string(out.uv)+",tan="+std::to_string(out.tangent)+
              ",aux="+std::to_string(out.auxiliary)+",prev="+std::to_string(out.previous);
    out.reason=out.native_gate_zero_velocity?
        "structural LocalVF contract; native velocity gate proved":
        "structural LocalVF contract; current+previous proved";
    return true;
}

struct PixelInputProof {
    bool complete=true;
    std::vector<size_t> previous;
};
bool known_non_input_operation(const Text& family,uint32_t op) {
    // Shader-model-6.0 operations supported by this proof, not a list of
    // every accepted material opcode. Unrecognized calls only lose the
    // optional history proof, not the ordinary current/previous contract.
    return (family=="unary"&&((op>=6&&op<=27)||(op>=83&&op<=86)))||
        (family=="binary"&&op>=35&&op<=45)||(family=="tertiary"&&op>=46&&op<=52)||
        (family=="dot2"&&op==54)||(family=="dot3"&&op==55)||(family=="dot4"&&op==56)||
        (family=="createHandle"&&op==57)||(family=="cbufferLoadLegacy"&&op==59)||
        (family=="sample"&&op==60)||(family=="sampleBias"&&op==61)||(family=="sampleLevel"&&op==62)||
        (family=="sampleGrad"&&op==63)||(family=="sampleCmp"&&op==64)||(family=="sampleCmpLevelZero"&&op==65)||
        (family=="textureLoad"&&op==66)||(family=="bufferLoad"&&op==68)||
        (family=="discard"&&op==82)||(family=="storeOutput"&&op==5)||(family=="sampleIndex"&&op==90);
}
bool pixel_interface(const Module& m,Facts& out,PixelInputProof& reads) {
    std::set<uint32_t> targets;
    bool has_depth_out=false;
    for(const auto& s:m.outputs){
        const bool is_depth_out=
            ((s.name=="SV_DepthLessEqual"&&s.kind==18)||(s.name=="SV_Depth"&&s.kind==17)||(s.name=="SV_DepthGreaterEqual"&&s.kind==19))&&
            s.component==9&&s.rows==1&&s.columns==1&&s.start_row==-1&&s.start_column==-1&&
            s.indices==std::vector<uint32_t>{0}&&s.interpolation==0;
        if(is_depth_out){
            if(has_depth_out){out.reason="PS declares duplicate depth output";return false;}
            has_depth_out=true;
            if(s.kind==18)out.native_depth_less_equal=true;
            continue;
        }
        const bool target_comp=(s.component==9||s.component==8||s.component==5||s.component==4);
        if(s.name!="SV_Target"||s.kind!=16||!target_comp||s.indices.size()!=1||s.indices[0]>=8){
            out.reason="PS unsupported output "+s.name+" kind="+std::to_string(s.kind)+" comp="+std::to_string(s.component);
            return false;
        }
        targets.insert(s.indices[0]);
    }
    if(targets.empty()){out.reason="PS declares no SV_Target outputs";return false;}
    out.color_target_count=uint32_t(*targets.rbegin()+1);
    for(size_t n=0;n<m.instructions.size();++n){
        const auto& rhs=m.instructions[n].rhs;
        if(rhs.find("call ")==Text::npos){
            if(rhs.find("@dx.op.")!=Text::npos||rhs.find("invoke ")!=Text::npos||rhs.find("callbr ")!=Text::npos)
                reads.complete=false;
            continue;
        }
        const auto at=rhs.find('@');
        if(at==Text::npos||rhs.compare(at,7,"@dx.op.")!=0){reads.complete=false;continue;}
        const auto open=rhs.find('(',at+7);
        if(open==Text::npos){reads.complete=false;continue;}
        const auto operation=rhs.substr(at+7,open-at-7);
        const auto dot=operation.find('.');
        const auto family=operation.substr(0,dot);
        const auto type=dot==Text::npos?Text{}:operation.substr(dot+1);
        std::vector<Text> a;
        uint32_t opcode=0,id=0,row=0,col=0;
        if(!m.call(rhs,operation,a)||a.empty()||!integer(a[0],"i32",opcode)){reads.complete=false;continue;}
        unsigned expected=0,arity=0;
        if(family=="loadInput"){expected=4;arity=5;}
        else if(family=="evalSnapped"){expected=87;arity=6;}
        else if(family=="evalSampleIndex"){expected=88;arity=5;}
        else if(family=="evalCentroid"){expected=89;arity=4;}
        if(!expected){
            reads.complete&=known_non_input_operation(family,opcode);
            continue;
        }
        const bool known_type=type=="f32"||type=="f16"||(expected==4&&(type=="i32"||type=="i16"));
        const auto s=opcode==expected&&a.size()==arity&&known_type&&integer(a[1],"i32",id)&&
            integer(a[2],"i32",row)&&integer(a[3],"i8",col)?
            std::find_if(m.inputs.begin(),m.inputs.end(),[&](const auto& s){return s.id==id;}):m.inputs.end();
        if(s==m.inputs.end()||row>=s->rows||col>=s->columns){
            if(operation=="loadInput.f32"||operation=="loadInput.i32"){
                out.reason="PS invalid "+operation+" id="+std::to_string(id);
                return false;
            }
            reads.complete=false;continue;
        }
        if(s->name=="VELOCITY_PREV_POS")reads.previous.push_back(n);
    }
    out.pixel_ignores_previous=reads.complete&&reads.previous.empty();
    for(const auto& r:m.resources)if(r.resource_class==0)out.pixel_srv_reads[r.reg/64]|=1ull<<(r.reg%64);
    out.all_resource_reads_modelled=true;

    // Extract static DXIL material texture hints (UV-sampled Texture2D SRVs and tangent-space
    // normalmap 2*v-1 unpacks) so high-slot / translucent RabbitFX passes can match D/N/L/M
    // unambiguously without relying on fixed t0..t2 registers or hardcoding any shader hash.
    if(out.native_identity){
        RtPsMaterialHint hint{};
        std::map<Text,uint32_t> h2t;
        for(const auto& [h,pair]:m.handles){
            if(pair.first!=0)continue;
            for(const auto& r:m.resources){
                if(r.resource_class==0&&r.range_id==pair.second&&r.reg<64){
                    const auto sh=m.srv_shapes.find(r.reg);
                    if(sh!=m.srv_shapes.end()&&sh->second==2)h2t[h]=r.reg; // Texture2D only
                    break;
                }
            }
        }
        if(!h2t.empty()){
            std::set<uint32_t> texcoord_ids;
            for(const auto& s:m.inputs)if(s.name=="TEXCOORD")texcoord_ids.insert(s.id);
            std::map<Text,std::vector<size_t>> uses;
            std::set<Text> uv_vars;
            for(size_t n=0;n<m.instructions.size();++n){
                const auto& inst=m.instructions[n];
                for(const auto& r:refs(inst.rhs))uses[r].push_back(n);
                if(!inst.lhs.empty()&&inst.rhs.find("@dx.op.loadInput.")!=Text::npos){
                    const auto at=inst.rhs.find("@dx.op.loadInput.");
                    const auto open=inst.rhs.find('(',at+7);
                    const auto op=open==Text::npos?Text{}:inst.rhs.substr(at+7,open-at-7);
                    std::vector<Text> a;uint32_t opcode=0,id=0;
                    if(m.call(inst.rhs,op,a)&&a.size()==5&&integer(a[0],"i32",opcode)&&opcode==4&&
                       integer(a[1],"i32",id)&&texcoord_ids.count(id)){
                        uv_vars.insert(inst.lhs);
                    }
                }
            }
            // Forward propagate TEXCOORD UVs without crossing texture/buffer loads.
            std::set<Text> uv_dep=uv_vars;
            std::vector<Text> q(uv_vars.begin(),uv_vars.end());
            while(!q.empty()){
                const auto v=q.back();q.pop_back();
                const auto u=uses.find(v);if(u==uses.end())continue;
                for(const auto n:u->second){
                    const auto& inst=m.instructions[n];
                    if(inst.lhs.empty())continue;
                    if(inst.rhs.find("@dx.op.sample")!=Text::npos||inst.rhs.find("@dx.op.textureLoad")!=Text::npos||
                       inst.rhs.find("@dx.op.bufferLoad")!=Text::npos||inst.rhs.find("@dx.op.rawBufferLoad")!=Text::npos)continue;
                    if(uv_dep.insert(inst.lhs).second)q.push_back(inst.lhs);
                }
            }
            auto is_unpack_2x_m1=[&](const Text& var)->bool {
                if(var.empty())return false;
                const auto u1=uses.find(var);if(u1==uses.end())return false;
                for(const auto n1:u1->second){
                    const auto& i1=m.instructions[n1];
                    const auto& r1=i1.rhs;
                    // FMA(v, 2.0, -1.0)
                    if(r1.find("@dx.op.tertiary.f32")!=Text::npos&&r1.find("2.000000e+00")!=Text::npos&&
                       r1.find("-1.000000e+00")!=Text::npos)return true;
                    // fmul v, 2.0 -> fadd m, -1.0 (or fsub m, 1.0)
                    if(r1.find("fmul ")!=Text::npos&&r1.find("2.000000e+00")!=Text::npos&&!i1.lhs.empty()){
                        const auto u2=uses.find(i1.lhs);if(u2==uses.end())continue;
                        for(const auto n2:u2->second){
                            const auto& r2=m.instructions[n2].rhs;
                            if((r2.find("fadd ")!=Text::npos&&r2.find("-1.000000e+00")!=Text::npos)||
                               (r2.find("fsub ")!=Text::npos&&r2.find(" 1.000000e+00")!=Text::npos))return true;
                        }
                    }
                }
                return false;
            };
            unsigned order_n=0;
            for(size_t n=0;n<m.instructions.size();++n){
                const auto& inst=m.instructions[n];
                const auto at=inst.rhs.find("@dx.op.sample");
                if(at==Text::npos)continue;
                const auto open=inst.rhs.find('(',at+7);
                if(open==Text::npos)continue;
                const auto op=inst.rhs.substr(at+7,open-at-7);
                const auto dot=op.find('.');
                const auto fam=op.substr(0,dot);
                if(fam!="sample"&&fam!="sampleBias"&&fam!="sampleLevel"&&fam!="sampleGrad")continue;
                std::vector<Text> a;uint32_t opcode=0;
                if(!m.call(inst.rhs,op,a)||a.size()<5||!integer(a[0],"i32",opcode))continue;
                if(opcode<60||opcode>63)continue;
                const auto hit=h2t.find(final_value(a[1]));
                if(hit==h2t.end())continue;
                const uint32_t reg=hit->second;
                hint.non_cmp_2d|=1ull<<reg;
                const bool is_uv=uv_dep.count(final_value(a[3]))||uv_dep.count(final_value(a[4]));
                if(is_uv){
                    if(!(hint.uv_srvs&(1ull<<reg))&&order_n<16)hint.first_order[order_n++]=uint8_t(reg+1);
                    hint.uv_srvs|=1ull<<reg;
                }
                if(!inst.lhs.empty()){
                    Text lane0,lane1;
                    const auto u=uses.find(inst.lhs);
                    if(u!=uses.end()){
                        const auto pfx="extractvalue %dx.types.ResRet.";
                        for(const auto un:u->second){
                            const auto& ui=m.instructions[un];
                            if(ui.lhs.empty()||!starts(ui.rhs,pfx))continue;
                            const auto comma=ui.rhs.find(", ");
                            if(comma==Text::npos)continue;
                            uint32_t lane=0;
                            if(!number(strip_inst_metadata(ui.rhs.substr(comma+2)),lane))continue;
                            if(lane<4&&reg<32)hint.lane_mask[reg]|=uint8_t(1u<<lane);
                            if(lane==0)lane0=ui.lhs;
                            else if(lane==1)lane1=ui.lhs;
                        }
                    }
                    if(is_unpack_2x_m1(lane0)&&is_unpack_2x_m1(lane1))hint.normal_srvs|=1ull<<reg;
                }
            }
            hint.valid=true;
        }
        if(hint.valid){
            std::lock_guard<std::mutex> lk(g_ps_material_hint_mtx);
            g_ps_material_hints[out.native_identity]=hint;
        }
    }
    return true;
}
}

Facts analyse_ir(const Text& text,uint64_t native_identity,Stage stage) {
    Facts out;out.native_identity=native_identity;out.stage=stage;
    Module m;if(!m.parse(text,stage)){out.reason=m.reason;return out;}
    out.inputs=m.inputs;out.outputs=m.outputs;out.resources=m.resources;bounds(m,out);
    auto& layout=out.layout;layout.view_reg=m.view_reg;layout.primitive_reg=m.primitive_reg;
    layout.view_declared_bytes=m.view_bytes;layout.primitive_declared_bytes=m.primitive_bytes;
    const auto required=[&](uint32_t reg){const auto i=out.cb_read_end.find(reg);return i==out.cb_read_end.end()?0u:i->second;};
    layout.view_read_end=required(m.view_reg);layout.primitive_read_end=required(m.primitive_reg);
    if(stage==Stage::vertex){
        extract_localvf_vertex(m,out);
    }else{
        PixelInputProof input;
        if(!pixel_interface(m,out,input)){
            if(out.reason.empty())out.reason="pixel interface is outside the native material capability";
            return out;
        }
        out.recognition=Recognition::native_pixel_interface;
        out.adaptation=Adaptation::native_material;
        out.native_gate_zero_velocity=input.complete&&m.view_reg==0&&m.primitive_reg!=UINT32_MAX&&
            gate_proof(m,m.primitive_reg,"SV_Target",4,{"0.000000e+00","0.000000e+00","0.000000e+00","0.000000e+00"},input.previous);
        out.reason=out.native_gate_zero_velocity?"native pixel interface; isolated native gates zero only Target4":
            out.pixel_ignores_previous?"native pixel interface; actual executable does not load previous varying":
            "native pixel interface; real previous required because no-history is unproved";
    }
    return out;
}
Pair pair(const Facts& vs,const Facts& ps,uint32_t targets) {
    Pair out;
    if(vs.stage!=Stage::vertex||ps.stage!=Stage::pixel||vs.recognition!=Recognition::executable_model||
       ps.recognition!=Recognition::native_pixel_interface){
        out.reason="stage, program model or complete VS/PS/RTV combination unsupported";return out;
    }
    // High-MRT GBuffer passes (targets > 4 or ps.color_target_count > 4, e.g. 7-RT GBuffer with SV_Target4 velocity)
    // require an exact target count match. Low-MRT color passes (1..4 RTVs, e.g. UE4 SeparateTranslucency rtv=2 with
    // a PS writing SV_Target0 or rtv=1 reusing a 2-output translucent PS) are valid DX12 OM bindings natively.
    const bool targets_ok=targets>=1&&targets<=8&&
        (targets==ps.color_target_count||(targets<=4&&ps.color_target_count>=1&&ps.color_target_count<=4));
    if(!targets_ok){
        out.reason="pair target mismatch rtv="+std::to_string(targets)+" ps_targets="+std::to_string(ps.color_target_count);
        return out;
    }
    for(const auto& input:ps.inputs){
        // Rasterizer-generated PS system values (SV_PrimitiveID=7, SV_SampleIndex=10, SV_IsFrontFace=13, SV_Coverage=14).
        if(input.kind==13||input.kind==10||input.kind==7||input.kind==14)continue;
        const auto source=std::find_if(vs.outputs.begin(),vs.outputs.end(),[&](const auto& s){return s.name==input.name&&s.indices==input.indices;});
        if(source==vs.outputs.end()||source->component!=input.component||source->kind!=input.kind||
           source->rows!=input.rows||source->columns<input.columns||source->start_row!=input.start_row||
           source->start_column!=input.start_column||
           (source->interpolation!=input.interpolation&&!(input.name=="SV_Position"&&input.kind==3&&
               source->interpolation==4&&input.interpolation==5))){
            out.reason="PS input "+input.name+" is not supplied by the native LocalVF executable interface";return out;
        }
    }
    out.current_previous=true;
    out.pixel_unused_previous=ps.pixel_ignores_previous;
    out.no_history=vs.previous_only_velocity&&vs.native_gate_zero_velocity&&vs.layout.snapshot_covers_reads()&&
        (ps.pixel_ignores_previous||(ps.native_gate_zero_velocity&&ps.layout.snapshot_covers_reads()));
    if(out.no_history&&!ps.pixel_ignores_previous)out.pixel_primitive=int(ps.layout.primitive_reg);
    out.reason=out.no_history?"normal and independent no-history capabilities proved":"normal capability proved; real previous remains required";
    return out;
}
std::shared_ptr<const Facts> ContentCache::resolve(const void* bytes,size_t size,uint64_t identity,Stage stage,const Disassemble& disassemble) {
    std::lock_guard lock(mutex_);
    auto& bucket=entries_[identity];
    for(const auto& e:bucket)if(e.parser==parser_version&&e.rules==rules_version&&e.stage==stage&&e.bytes.size()==size&&
       (!size||(bytes&&!std::memcmp(e.bytes.data(),bytes,size)))){++stats_.hits;return e.facts;}
    Facts facts;facts.native_identity=identity;facts.stage=stage;Text text,why;
    if(!bytes||size<32||size>2u*1024u*1024u)facts.reason="container size outside supported bound";
    else if(!disassemble(bytes,size,text,why))facts.reason="DXC Disassemble: "+why;
    else facts=analyse_ir(text,identity,stage);
    Entry e;e.stage=stage;if(bytes&&size<=2u*1024u*1024u)e.bytes.assign(static_cast<const unsigned char*>(bytes),static_cast<const unsigned char*>(bytes)+size);
    e.facts=std::make_shared<const Facts>(std::move(facts));bucket.push_back(std::move(e));++stats_.parses;++stats_.entries;
    return bucket.back().facts;
}
CacheStats ContentCache::stats() const {std::lock_guard lock(mutex_);return stats_;}
}
