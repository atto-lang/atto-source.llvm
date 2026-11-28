#ifndef ATTO_BOUNDED_V2_BRIDGE_H
#define ATTO_BOUNDED_V2_BRIDGE_H
/* Freestanding bridge prototype: typed slots are 16 LE bytes (value/offset,
 * length). Variable fields occupy contiguous trailing bytes in field order.
 * All pointer ranges must be valid and quiescent; input/output must not overlap.
 * Returns -1 with no output changes on malformed values or insufficient capacity.
 * No allocation, imports, IO, reflection or native-size wire fields. */
#include <stdint.h>
#include <stddef.h>
#ifndef ATTO_BOUNDED_SCHEMA
#error missing schema
#endif
static const uint8_t tags[]={ATTO_BOUNDED_TAGS 0};
#define COUNT ((uint32_t)(sizeof(tags)-1))
#define MAX_FIELD 16384u
#define MAX_WIRE 65536u
#define MAX_SLOTS (MAX_WIRE+256u*16u)
static uint64_t read_le(const uint8_t* p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static void write_le(uint8_t* p,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)p[i]=(uint8_t)(v>>(8*i));}
static void copy_bytes(uint8_t* out,const uint8_t* in,uint32_t n){for(uint32_t i=0;i<n;++i)out[i]=in[i];}
static uint64_t checksum(const uint8_t* b,uint32_t n){uint64_t h=UINT64_C(0xcbf29ce484222325);for(uint32_t i=0;i<n;++i){h^=b[i];h*=UINT64_C(0x100000001b3);}return h;}
static int utf8(const uint8_t* b,uint32_t n){
    uint32_t p=0;
    while(p<n){uint8_t a=b[p++];if(a<128)continue;uint32_t v,min,k;
        if(a>=0xc2&&a<=0xdf){v=a&31u;min=0x80;k=1;}
        else if(a>=0xe0&&a<=0xef){v=a&15u;min=0x800;k=2;}
        else if(a>=0xf0&&a<=0xf4){v=a&7u;min=0x10000;k=3;}
        else return 0;
        if(k>n-p)return 0;
        for(uint32_t j=0;j<k;++j){uint8_t c=b[p++];if((c&0xc0)!=0x80)return 0;v=(v<<6)|(c&63u);}
        if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return 0;
    }return 1;
}
static int disjoint(const uint8_t* in,uint32_t n,const uint8_t* out,uint32_t m){
    if(!in||!out)return 0;
    uintptr_t a=(uintptr_t)in,b=(uintptr_t)out;
    if(n>UINTPTR_MAX-a||m>UINTPTR_MAX-b)return 0;
    return n==0||m==0||a+n<=b||b+m<=a;
}
uint32_t atto_bounded_fields_v2(void){return COUNT;}
uint64_t atto_bounded_schema_v2(void){return ATTO_BOUNDED_SCHEMA;}
int32_t atto_bounded_encode_v2(const uint8_t* in,uint32_t len,uint8_t* out,uint32_t cap){
    if(!in||!out||len>MAX_SLOTS||len<COUNT*16u)return -1;
    uint32_t raw=COUNT*16u,size=27;
    for(uint32_t i=0;i<COUNT;++i){uint8_t t=tags[i];uint64_t v=read_le(in+i*16u,8),n=read_le(in+i*16u+8,8);uint32_t more;
        if(t==1||t==2){if(n!=0||(t==2&&v>1))return -1;more=t==1?9:2;}
        else{if((t!=3&&t!=4)||v!=raw||n>MAX_FIELD||n>len-raw)return -1;if(t==3&&!utf8(in+raw,(uint32_t)n))return -1;raw+=(uint32_t)n;more=5+(uint32_t)n;}
        if(more>MAX_WIRE-size)return -1;
        size+=more;
    }
    if(raw!=len||cap<size||!disjoint(in,len,out,size))return -1;
    static const uint8_t magic[]={65,84,84,79,83,84,48,50,2};
    copy_bytes(out,magic,9);write_le(out+9,ATTO_BOUNDED_SCHEMA,8);write_le(out+17,COUNT,2);
    uint32_t p=19;
    for(uint32_t i=0;i<COUNT;++i){uint8_t t=tags[i];uint64_t v=read_le(in+i*16u,8),n=read_le(in+i*16u+8,8);out[p++]=t;
        if(t==1){write_le(out+p,v,8);p+=8;}else if(t==2){out[p++]=(uint8_t)v;}
        else{write_le(out+p,n,4);p+=4;copy_bytes(out+p,in+(uint32_t)v,(uint32_t)n);p+=(uint32_t)n;}
    }
    write_le(out+p,checksum(out,p),8);return (int32_t)size;
}
int32_t atto_bounded_decode_v2(const uint8_t* in,uint32_t len,uint8_t* out,uint32_t cap){
    static const uint8_t magic[]={65,84,84,79,83,84,48,50,2};
    if(!in||!out||len<27||len>MAX_WIRE)return -1;
    for(unsigned i=0;i<9;++i)if(in[i]!=magic[i])return -1;
    uint32_t end=len-8,p=19,need=COUNT*16u;
    if(read_le(in+9,8)!=ATTO_BOUNDED_SCHEMA||read_le(in+17,2)!=COUNT||read_le(in+end,8)!=checksum(in,end))return -1;
    for(uint32_t i=0;i<COUNT;++i){uint8_t t=tags[i];if(p>=end||in[p++]!=t)return -1;
        if(t==1){if(end-p<8)return -1;p+=8;}
        else if(t==2){if(p>=end||in[p++]>1)return -1;}
        else{if((t!=3&&t!=4)||end-p<4)return -1;uint32_t n=(uint32_t)read_le(in+p,4);p+=4;if(n>MAX_FIELD||n>end-p||n>MAX_SLOTS-need)return -1;if(t==3&&!utf8(in+p,n))return -1;p+=n;need+=n;}
    }
    if(p!=end||cap<need||!disjoint(in,len,out,need))return -1;
    p=19;uint32_t raw=COUNT*16u;
    for(uint32_t i=0;i<COUNT;++i){uint8_t t=tags[i];++p;uint8_t* slot=out+i*16u;
        if(t==1){write_le(slot,read_le(in+p,8),8);write_le(slot+8,0,8);p+=8;}
        else if(t==2){write_le(slot,in[p++],8);write_le(slot+8,0,8);}
        else{uint32_t n=(uint32_t)read_le(in+p,4);p+=4;write_le(slot,raw,8);write_le(slot+8,n,8);copy_bytes(out+raw,in+p,n);raw+=n;p+=n;}
    }
    return (int32_t)need;
}
#endif
