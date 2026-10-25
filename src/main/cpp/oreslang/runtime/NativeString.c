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


/* Strict, bounded JSON string decoding into an owned UTF-8 buffer.
 * The caller retains ownership of the input and must drop the output.
 */
static int json_hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}
static int parse_json_hex4(const unsigned char* bytes, uint64_t length,
                           uint64_t* offset, uint32_t* cp) {
    if (length - *offset < 4) return 0;
    uint32_t codepoint = 0;
    for (unsigned k = 0; k < 4; ++k) {
        int nibble = json_hex_nibble(bytes[(*offset)++]);
        if (nibble < 0) return 0;
        codepoint = (codepoint << 4) | (uint32_t)nibble;
    }
    *cp = codepoint;
    return 1;
}
static void json_emit_utf8(uint32_t cp, unsigned char* output, uint64_t* n) {
    if (cp < 0x80) output[(*n)++] = (unsigned char)cp;
    else if (cp < 0x800) {
        output[(*n)++] = (unsigned char)(0xc0 | (cp >> 6));
        output[(*n)++] = (unsigned char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        output[(*n)++] = (unsigned char)(0xe0 | (cp >> 12));
        output[(*n)++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        output[(*n)++] = (unsigned char)(0x80 | (cp & 0x3f));
    } else {
        output[(*n)++] = (unsigned char)(0xf0 | (cp >> 18));
        output[(*n)++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        output[(*n)++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        output[(*n)++] = (unsigned char)(0x80 | (cp & 0x3f));
    }
}
OreslangStringStatus oreslang_string_json_unquote(const OreslangNativeString* json,
                                                  OreslangNativeString* out) {
    if (json == out || !empty(out)) return ORESLANG_STRING_INVALID;
    OreslangStringStatus status = check(json);
    if (status != ORESLANG_STRING_OK) return status;
    const uint64_t length = json->byte_length;
    const unsigned char* bytes = json->data;
    if (length < 2 || bytes[0] != '"' || bytes[length - 1] != '"')
        return ORESLANG_STRING_INVALID_JSON;
    if (length == 2) return ORESLANG_STRING_OK;
    /* Decoding never expands the JSON source length. An escaped surrogate
       pair consumes 12 source bytes and emits four UTF-8 bytes. */
    unsigned char* result = (unsigned char*)malloc((size_t)(length - 2));
    if (!result) return ORESLANG_STRING_NO_MEMORY;
    uint64_t written = 0;
    for (uint64_t i = 1; i < length - 1; ++i) {
        unsigned char c = bytes[i];
        if (c < 0x20 || c == '"') goto invalid_json;
        if (c != '\\') {result[written++] = c; continue;}
        if (++i >= length - 1) goto invalid_json;
        c = bytes[i];
        switch (c) {
        case '"': case '\\': case '/': result[written++] = c; break;
        case 'b': result[written++] = '\b'; break;
        case 'f': result[written++] = '\f'; break;
        case 'n': result[written++] = '\n'; break;
        case 'r': result[written++] = '\r'; break;
        case 't': result[written++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            ++i;
            if (!parse_json_hex4(bytes, length - 1, &i, &cp)) goto invalid_json;
            --i;
            if (cp >= 0xdc00 && cp <= 0xdfff) goto invalid_json;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (length - 1 - (i + 1) < 6 || bytes[i + 1] != '\\'
                    || bytes[i + 2] != 'u') goto invalid_json;
                i += 3;
                uint32_t low;
                if (!parse_json_hex4(bytes, length - 1, &i, &low)) goto invalid_json;
                --i;
                if (low < 0xdc00 || low > 0xdfff) goto invalid_json;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            }
            json_emit_utf8(cp, result, &written);
            break;
        }
        default: goto invalid_json;
        }
    }
    out->data = result;
    out->byte_length = written;
    return ORESLANG_STRING_OK;
invalid_json:
    free(result);
    return ORESLANG_STRING_INVALID_JSON;
}
