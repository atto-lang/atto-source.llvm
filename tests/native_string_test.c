#include "oreslang/runtime/NativeString.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"native String failure line %d: %s\n",__LINE__,#x);exit(1);}} while(0)

static void check_json_unquote_fixture(const unsigned char* bytes, size_t n,
                                       const unsigned char* expected, size_t expected_n) {
    OreslangNativeString source={0}, decoded={0};
    CHECK(oreslang_string_new_utf8(bytes,(uint64_t)n,&source)==ORESLANG_STRING_OK);
    CHECK(oreslang_string_json_unquote(&source,&decoded)==ORESLANG_STRING_OK);
    CHECK(decoded.byte_length==expected_n);
    if (expected_n) CHECK(memcmp(decoded.data,expected,expected_n)==0);
    if (expected_n) CHECK(oreslang_string_json_unquote(&source,&decoded)==ORESLANG_STRING_INVALID);
    CHECK(oreslang_string_json_unquote(&source,&source)==ORESLANG_STRING_INVALID);
    oreslang_string_drop(&source);
    oreslang_string_drop(&decoded);
}
static void check_json_rejected(const unsigned char* bytes, size_t n) {
    OreslangNativeString source={0}, decoded={0};
    CHECK(oreslang_string_new_utf8(bytes,(uint64_t)n,&source)==ORESLANG_STRING_OK);
    CHECK(oreslang_string_json_unquote(&source,&decoded)==ORESLANG_STRING_INVALID_JSON);
    CHECK(!decoded.data && decoded.byte_length==0);
    oreslang_string_drop(&source);
}
static void test_strict_json_unquote(void) {
    const unsigned char empty_json[]={'"','"'};
    check_json_unquote_fixture(empty_json,sizeof(empty_json),(const unsigned char*)"",0);
    const char* plain="\"hello\"";
    check_json_unquote_fixture((const unsigned char*)plain,strlen(plain),
                               (const unsigned char*)"hello",5);
    const char* escapes="\"\\u0000\\n\\t\\\"\\\\/\"";
    const unsigned char escaped_expected[]={0,'\n','\t','"','\\','/'};
    check_json_unquote_fixture((const unsigned char*)escapes,strlen(escapes),
                               escaped_expected,sizeof(escaped_expected));
    const char* paired="\"\\ud83d\\ude00\"";
    const unsigned char emoji[]={0xf0,0x9f,0x98,0x80};
    check_json_unquote_fixture((const unsigned char*)paired,strlen(paired),emoji,sizeof(emoji));
    const char* accent="\"\\u00e9\"";
    const unsigned char utf8_accent[]={0xc3,0xa9};
    check_json_unquote_fixture((const unsigned char*)accent,strlen(accent),
                               utf8_accent,sizeof(utf8_accent));
    const char* failures[]={
        "unquoted", "\"unterminated", "\"bad\\z\"", "\"\\ud800\"",
        "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"\\u12zz\"",
        "\"x\"x\"", "\"a\\", "\"\\u123\"", "\"line\nbreak\"",
        "\"\\uDFFF\"", "\"\\uD800\\uD800\""
    };
    for (size_t i=0;i<sizeof(failures)/sizeof(failures[0]);++i)
        check_json_rejected((const unsigned char*)failures[i],strlen(failures[i]));
    OreslangNativeString oversized={(unsigned char*)"x",UINT64_MAX}, output={0};
    CHECK(oreslang_string_json_unquote(&oversized,&output)==ORESLANG_STRING_TOO_LARGE);
    OreslangNativeString null_with_length={NULL,UINT64_MAX};
    CHECK(oreslang_string_json_unquote(&null_with_length,&output)==ORESLANG_STRING_INVALID);
    CHECK(!output.data && !output.byte_length);
    const unsigned char bad_utf8[]={0xc0,0xaf};
    OreslangNativeString invalid_utf8={(unsigned char*)bad_utf8,sizeof(bad_utf8)};
    CHECK(oreslang_string_json_unquote(&invalid_utf8,&output)==ORESLANG_STRING_INVALID_UTF8);
}

int main(void) {
    CHECK(sizeof(void*)==8 && sizeof(OreslangNativeString)==16);
    CHECK(offsetof(OreslangNativeString,byte_length)==8);
    OreslangNativeString a={0},b={0},c={0},raw={0},quoted={0},emoji={0},quoted_emoji={0};
    CHECK(oreslang_string_new_utf8((const unsigned char*)"hi",2,&a)==ORESLANG_STRING_OK);
    CHECK(oreslang_string_clone(&a,&b)==ORESLANG_STRING_OK);
    CHECK(a.data!=b.data && memcmp(a.data,b.data,2)==0);
    CHECK(oreslang_string_concat(&a,&b,&c)==ORESLANG_STRING_OK);
    CHECK(c.byte_length==4 && memcmp(c.data,"hihi",4)==0);
    CHECK(oreslang_string_clone(&a,&a)==ORESLANG_STRING_INVALID);
    CHECK(oreslang_string_concat(&a,&b,&b)==ORESLANG_STRING_INVALID);
    const unsigned char chars[]={'a',0,'\n','"','\\'};
    CHECK(oreslang_string_new_utf8(chars,sizeof(chars),&raw)==ORESLANG_STRING_OK);
    CHECK(oreslang_string_json_quote(&raw,&quoted)==ORESLANG_STRING_OK);
    const char want[]={'"','a','\\','u','0','0','0','0','\\','n','\\','"','\\','\\','"'};
    CHECK(quoted.byte_length==sizeof(want) && memcmp(quoted.data,want,sizeof(want))==0);
    const unsigned char smile[]={0xf0,0x9f,0x98,0x80};
    CHECK(oreslang_string_new_utf8(smile,sizeof(smile),&emoji)==ORESLANG_STRING_OK);
    CHECK(oreslang_string_json_quote(&emoji,&quoted_emoji)==ORESLANG_STRING_OK);
    CHECK(quoted_emoji.byte_length==6 && quoted_emoji.data[0]=='"'
          && quoted_emoji.data[5]=='"' && memcmp(quoted_emoji.data+1,smile,4)==0);
    const unsigned char overlong[]={0xc0,0xaf},surrogate[]={0xed,0xa0,0x80};
    const unsigned char truncated[]={0xf0,0x9f},too_high[]={0xf4,0x90,0x80,0x80};
    CHECK(oreslang_string_new_utf8(overlong,sizeof(overlong),&raw)==ORESLANG_STRING_INVALID);
    OreslangNativeString empty={0};
    CHECK(oreslang_string_new_utf8(overlong,sizeof(overlong),&empty)==ORESLANG_STRING_INVALID_UTF8);
    CHECK(oreslang_string_new_utf8(surrogate,sizeof(surrogate),&empty)==ORESLANG_STRING_INVALID_UTF8);
    CHECK(oreslang_string_new_utf8(truncated,sizeof(truncated),&empty)==ORESLANG_STRING_INVALID_UTF8);
    CHECK(oreslang_string_new_utf8(too_high,sizeof(too_high),&empty)==ORESLANG_STRING_INVALID_UTF8);
    CHECK(oreslang_string_new_utf8((const unsigned char*)"x",UINT64_MAX,&empty)==ORESLANG_STRING_TOO_LARGE);
    CHECK(oreslang_string_new_utf8(NULL,0,&empty)==ORESLANG_STRING_OK);
    oreslang_string_drop(&a);oreslang_string_drop(&a);oreslang_string_drop(&b);
    oreslang_string_drop(&c);oreslang_string_drop(&raw);oreslang_string_drop(&quoted);
    oreslang_string_drop(&emoji);oreslang_string_drop(&quoted_emoji);
    CHECK(!a.data && !a.byte_length);
    test_strict_json_unquote();
    puts("native owned UTF-8 String ABI and strict bounded JSON quote/unquote codec: PASS");
    return 0;
}
