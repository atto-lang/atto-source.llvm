#include "oreslang/runtime/NativeString.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"native String failure line %d: %s\n",__LINE__,#x);exit(1);}} while(0)
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
    puts("native owned UTF-8 String ABI and bounded JSON codec: PASS");
    return 0;
}
