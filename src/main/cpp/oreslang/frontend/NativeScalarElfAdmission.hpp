#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <set>
#include <utility>

namespace oreslang::package_interface::elf_scalar_detail {

// Deliberately closed ELF64 little-endian x86-64 relocatable scalar-provider
// slice. This is a structural gate, not a proof of machine-code safety.
// No external ELF library or unaligned native struct reads.
inline void verify(const std::vector<std::uint8_t>& b,std::string_view symbol) {
    const auto fail=[](){throw std::runtime_error("restricted ELF scalar object rejected");};
    const auto range=[&](std::uint64_t off,std::uint64_t size){
        return off<=b.size() && size<=b.size()-off;
    };
    const auto read=[&](std::uint64_t off,unsigned n){
        if(!range(off,n))fail();
        std::uint64_t v=0;
        for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[off+i])<<(8*i);
        return v;
    };
    if(b.size()<64 || b.size()>16u*1024u*1024u ||
        read(0,4)!=0x464c457f || read(4,4)!=0x00010102 ||
        read(8,8)!=0 || read(16,2)!=1 || read(18,2)!=62 ||
        read(20,4)!=1 || read(24,8)!=0 || read(32,8)!=0 ||
        read(48,4)!=0 || read(52,2)!=64 || read(54,2)!=0 ||
        read(56,2)!=0 || read(58,2)!=64)fail();
    const auto shoff=read(40,8), count=read(60,2), names=read(62,2);
    if(count<5 || count>32 || names==0 || names>=count ||
       shoff<64 || shoff%8 || !range(shoff,count*64))fail();
    struct Section {std::uint64_t name,type,flags,off,size,link,info,align,ent;};
    std::vector<Section> sections;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> occupied{{0,64},{shoff,shoff+count*64}};
    for(std::uint64_t i=0;i<count;++i){
        const auto h=shoff+i*64;
        Section s{read(h,4),read(h+4,4),read(h+8,8),read(h+24,8),
          read(h+32,8),read(h+40,4),read(h+44,4),read(h+48,8),read(h+56,8)};
        if(i==0){for(unsigned j=0;j<64;++j)if(b[h+j])fail();}
        else {
            if(read(h+16,8)!=0 || !range(s.off,s.size) || s.off<64 ||
                !s.align || s.align>4096 || (s.align&(s.align-1)) ||
                s.off%s.align || s.link>=count)fail();
            if(s.size){
                for(const auto& [lo,hi]:occupied)
                    if(s.off<hi && lo<s.off+s.size)fail();
                occupied.emplace_back(s.off,s.off+s.size);
            }
        }
        sections.push_back(s);
    }
    const auto string_at=[&](const Section& s,std::uint64_t pos){
        if(s.type!=3 || pos>=s.size)fail();
        const auto begin=s.off+pos;
        auto end=begin;
        while(end<s.off+s.size && b[end] && end-begin<=512)++end;
        if(end>=s.off+s.size || end-begin>512)fail();
        return std::string(reinterpret_cast<const char*>(b.data()+begin),end-begin);
    };
    const auto& name_table=sections[names];
    std::set<std::string> seen;
    std::uint64_t text=0,sym=0,eh=0,rela=0,addr=0,stack=0;
    for(std::uint64_t i=1;i<count;++i){
        const auto& s=sections[i];const auto name=string_at(name_table,s.name);
        if(!seen.insert(name).second)fail();
        if(name==".text" && s.type==1 && s.flags==6 && s.size && !s.link && !s.info && !s.ent)text=i;
        else if((name==".strtab"||name==".shstrtab") && s.type==3 && !s.flags && !s.link && !s.info && !s.ent && s.size && !b[s.off] && !b[s.off+s.size-1]){}
        else if(name==".symtab" && s.type==2 && !s.flags && s.ent==24 && s.size%24==0 && s.size>=48 && s.size/24<=64)sym=i;
        else if(name==".note.GNU-stack" && s.type==1 && !s.flags && !s.size && !s.link && !s.info && !s.ent)stack=i;
        else if(name==".eh_frame" && (s.type==1||s.type==0x70000001) && s.flags==2 && s.size && !s.link && !s.info && !s.ent)eh=i;
        else if(name==".rela.eh_frame" && s.type==4 && s.flags==0x40 && s.ent==24 && s.size==24)rela=i;
        else if(name==".llvm_addrsig" && s.type==0x6fff4c03 && s.flags==0x80000000 && !s.size && !s.info && !s.ent)addr=i;
        else fail();
    }
    if(!text||!sym||!stack||bool(eh)!=bool(rela))fail();
    const auto& sy=sections[sym]; const auto& st=sections[sy.link];
    const auto n=sy.size/24;
    if(st.type!=3 || sy.info<1 || sy.info>=n ||
        (addr && sections[addr].link!=sym))fail();
    std::uint64_t text_section_symbol=0, exported=0;
    for(std::uint64_t i=0;i<n;++i){
        auto off=sy.off+i*24;
        if(i==0){for(unsigned j=0;j<24;++j)if(b[off+j])fail();continue;}
        const auto name=string_at(st,read(off,4));
        const auto info=read(off+4,1),other=read(off+5,1),ndx=read(off+6,2);
        const auto val=read(off+8,8),size=read(off+16,8);
        if(other || ((i<sy.info)!=(info>>4==0)))fail();
        if(info==4 && ndx==0xfff1 && !val && !size && !name.empty())continue;
        if(info==3 && ndx==text && !val && !size && name.empty() && !text_section_symbol){text_section_symbol=i;continue;}
        if(info==0x12 && ndx==text && name==symbol && !val &&
            size==sections[text].size && !exported){exported=i;continue;}
        fail();
    }
    if(!exported)fail();
    if(rela){
        const auto& r=sections[rela];
        if(r.link!=sym || r.info!=eh || !text_section_symbol ||
           read(r.off+8,8)!=((text_section_symbol<<32)|2u) ||
           read(r.off+16,8)!=0 || read(r.off,8)>sections[eh].size ||
           sections[eh].size-read(r.off,8)<4)fail();
    }
}
} // namespace oreslang::package_interface::elf_scalar_detail
