#include "oreslang/runtime/NativeString.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
static int empty(const OreslangNativeString* p) {
    return p && !p->data && !p->byte_length;
}
static int bounded(uint64_t n) {
    return n <= ORESLANG_STRING_MAX_BYTES && n <= (uint64_t)SIZE_MAX;
}
static int utf8(const unsigned char* s, uint64_t n) {
    uint64_t i=0;
    if (!s && n) return 0;
    while (i<n) {
        unsigned char lead=s[i++];
        if (lead<0x80) continue;
        unsigned int rest;
        if (lead>=0xc2 && lead<=0xdf) rest=1;
        else if (lead>=0xe0 && lead<=0xef) rest=2;
        else if (lead>=0xf0 && lead<=0xf4) rest=3;
        else return 0;
        if (rest > n-i || (s[i]&0xc0)!=0x80) return 0;
        if ((lead==0xe0 && s[i]<0xa0) || (lead==0xed && s[i]>0x9f)
         || (lead==0xf0 && s[i]<0x90) || (lead==0xf4 && s[i]>0x8f)) return 0;
        for (unsigned int k=0;k<rest;k++) if ((s[i++]&0xc0)!=0x80) return 0;
    }
    return 1;
}
static OreslangStringStatus check(const OreslangNativeString* s) {
    if (!s || (!s->data && s->byte_length)) return ORESLANG_STRING_INVALID;
    if (!bounded(s->byte_length)) return ORESLANG_STRING_TOO_LARGE;
    return utf8(s->data,s->byte_length)?ORESLANG_STRING_OK:ORESLANG_STRING_INVALID_UTF8;
}
static OreslangStringStatus alloc_copy(const unsigned char* s, uint64_t n, OreslangNativeString* out) {
    if (!empty(out) || (!s && n)) return ORESLANG_STRING_INVALID;
    if (!bounded(n)) return ORESLANG_STRING_TOO_LARGE;
    if (!n) return ORESLANG_STRING_OK;
    unsigned char* result=(unsigned char*)malloc((size_t)n);
    if (!result) return ORESLANG_STRING_NO_MEMORY;
    memcpy(result,s,(size_t)n);
    out->data=result;out->byte_length=n;
    return ORESLANG_STRING_OK;
}
OreslangStringStatus oreslang_string_new_utf8(const unsigned char* s, uint64_t n, OreslangNativeString* out) {
    if (!empty(out)) return ORESLANG_STRING_INVALID;
    if (!bounded(n)) return ORESLANG_STRING_TOO_LARGE;
    if (!utf8(s,n)) return ORESLANG_STRING_INVALID_UTF8;
    return alloc_copy(s,n,out);
}
OreslangStringStatus oreslang_string_clone(const OreslangNativeString* s, OreslangNativeString* out) {
    if (s==out || !empty(out)) return ORESLANG_STRING_INVALID;
    OreslangStringStatus x=check(s);
    return x==ORESLANG_STRING_OK?alloc_copy(s->data,s->byte_length,out):x;
}
OreslangStringStatus oreslang_string_concat(const OreslangNativeString* a,
        const OreslangNativeString* b,OreslangNativeString* out) {
    if (a==out || b==out || !empty(out)) return ORESLANG_STRING_INVALID;
    OreslangStringStatus x=check(a);
    if (x!=ORESLANG_STRING_OK) return x;
    x=check(b);
    if (x!=ORESLANG_STRING_OK) return x;
    if (a->byte_length > ORESLANG_STRING_MAX_BYTES-b->byte_length) return ORESLANG_STRING_TOO_LARGE;
    uint64_t n=a->byte_length+b->byte_length;
    if (!n) return ORESLANG_STRING_OK;
    unsigned char* p=(unsigned char*)malloc((size_t)n);
    if (!p) return ORESLANG_STRING_NO_MEMORY;
    if (a->byte_length) memcpy(p,a->data,(size_t)a->byte_length);
    if (b->byte_length) memcpy(p+a->byte_length,b->data,(size_t)b->byte_length);
    out->data=p;out->byte_length=n;
    return ORESLANG_STRING_OK;
}
OreslangStringStatus oreslang_string_json_quote(const OreslangNativeString* s,OreslangNativeString* out) {
    if (s==out || !empty(out)) return ORESLANG_STRING_INVALID;
    OreslangStringStatus x=check(s);
    if (x!=ORESLANG_STRING_OK) return x;
    uint64_t n=2;
    for (uint64_t i=0;i<s->byte_length;i++) {
        unsigned char c=s->data[i];
        uint64_t width=c<0x20?6:(c=='"'||c=='\\'?2:1);
        if (c=='\b'||c=='\f'||c=='\n'||c=='\r'||c=='\t') width=2;
        if (n>ORESLANG_STRING_MAX_BYTES-width) return ORESLANG_STRING_TOO_LARGE;
        n+=width;
    }
    unsigned char* p=(unsigned char*)malloc((size_t)n);
    if (!p) return ORESLANG_STRING_NO_MEMORY;
    const char hex[]="0123456789abcdef";
    uint64_t j=0;p[j++]='"';
    for (uint64_t i=0;i<s->byte_length;i++) {
        unsigned char c=s->data[i];
        if (c=='"'||c=='\\') {p[j++]='\\';p[j++]=c;}
        else if (c=='\b'||c=='\f'||c=='\n'||c=='\r'||c=='\t') {
            p[j++]='\\';p[j++]=c=='\b'?'b':c=='\f'?'f':c=='\n'?'n':c=='\r'?'r':'t';
        } else if (c<0x20) {
            p[j++]='\\';p[j++]='u';p[j++]='0';p[j++]='0';
            p[j++]=hex[c>>4];p[j++]=hex[c&15];
        } else p[j++]=c;
    }
    p[j++]='"';out->data=p;out->byte_length=j;
    return ORESLANG_STRING_OK;
}
void oreslang_string_drop(OreslangNativeString* p) {
    if (!p) return;
    free(p->data);p->data=NULL;p->byte_length=0;
}
