#include "oreslang/runtime/NativeCString.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do {     if (!(x)) {         fprintf(stderr, "native CString failure line %d: %s\n", __LINE__, #x);         exit(1);     } } while (0)

int main(void) {
    OreslangNativeString empty={0}, plain={0}, embedded={0}, unicode={0}, roundtrip={0};
    OreslangNativeCString cempty={0}, cplain={0}, cunicode={0};

    CHECK(oreslang_string_new_utf8(NULL,0,&empty)==ORESLANG_STRING_OK);
    CHECK(oreslang_cstring_from_string(&empty,&cempty)==ORESLANG_STRING_OK);
    CHECK(cempty.data && cempty.byte_length==0 && cempty.data[0]==0);

    const unsigned char no_terminator[]={'h','e','l','l','o'};
    CHECK(oreslang_string_new_utf8(no_terminator,sizeof(no_terminator),&plain)==ORESLANG_STRING_OK);
    CHECK(oreslang_cstring_from_string(&plain,&cplain)==ORESLANG_STRING_OK);
    CHECK(cplain.byte_length==sizeof(no_terminator));
    CHECK(memcmp(cplain.data,no_terminator,sizeof(no_terminator))==0);
    CHECK(cplain.data[cplain.byte_length]==0);
    CHECK(oreslang_cstring_to_string(&cplain,&roundtrip)==ORESLANG_STRING_OK);
    CHECK(roundtrip.byte_length==sizeof(no_terminator));
    CHECK(memcmp(roundtrip.data,no_terminator,sizeof(no_terminator))==0);

    const unsigned char nul_bytes[]={'A',0,'B'};
    CHECK(oreslang_string_new_utf8(nul_bytes,sizeof(nul_bytes),&embedded)==ORESLANG_STRING_OK);
    OreslangNativeCString rejected={0};
    CHECK(oreslang_cstring_from_string(&embedded,&rejected)==ORESLANG_STRING_EMBEDDED_NUL);
    CHECK(!rejected.data && rejected.byte_length==0);

    const unsigned char utf8[]={0xc3,0xa9,0xf0,0x9f,0x98,0x80};
    CHECK(oreslang_string_new_utf8(utf8,sizeof(utf8),&unicode)==ORESLANG_STRING_OK);
    CHECK(oreslang_cstring_from_string(&unicode,&cunicode)==ORESLANG_STRING_OK);
    CHECK(cunicode.byte_length==sizeof(utf8));
    CHECK(cunicode.data[sizeof(utf8)]==0);

    unsigned char forged_nul[]={'A',0,'B',0};
    OreslangNativeCString bad_nul={forged_nul,3};
    OreslangNativeString bad_out={0};
    CHECK(oreslang_cstring_to_string(&bad_nul,&bad_out)==ORESLANG_STRING_EMBEDDED_NUL);
    CHECK(!bad_out.data && bad_out.byte_length==0);

    unsigned char forged_utf8[]={0xc0,0xaf,0};
    OreslangNativeCString bad_utf8={forged_utf8,2};
    CHECK(oreslang_cstring_to_string(&bad_utf8,&bad_out)==ORESLANG_STRING_INVALID_UTF8);

    unsigned char missing_terminator[]={'x','y','z'};
    OreslangNativeCString bad_terminator={missing_terminator,2};
    CHECK(oreslang_cstring_to_string(&bad_terminator,&bad_out)==ORESLANG_STRING_INVALID);

    // Reject implausible extents before dereferencing caller-supplied storage.
    unsigned char tiny[] = {0};
    OreslangNativeCString absurd_extent = {tiny, UINT64_MAX};
    CHECK(oreslang_cstring_to_string(&absurd_extent, &bad_out)==ORESLANG_STRING_TOO_LARGE);
    CHECK(!bad_out.data && bad_out.byte_length==0);

    // Never overwrite an existing owning destination on a second conversion.
    CHECK(oreslang_cstring_from_string(&plain, &cplain)==ORESLANG_STRING_INVALID);
    CHECK(cplain.byte_length==sizeof(no_terminator));
    CHECK(memcmp(cplain.data, no_terminator, sizeof(no_terminator))==0);

    // UTF-8 surrogate encoding is invalid even when the span is fully bounded.
    const unsigned char surrogate_utf8[]={0xed,0xa0,0x80};
    OreslangNativeString invalid_source={0};
    CHECK(oreslang_string_new_utf8(surrogate_utf8,sizeof(surrogate_utf8),&invalid_source)
          ==ORESLANG_STRING_INVALID_UTF8);
    CHECK(!invalid_source.data && invalid_source.byte_length==0);

    oreslang_string_drop(&empty);
    oreslang_string_drop(&plain);
    oreslang_string_drop(&embedded);
    oreslang_string_drop(&unicode);
    oreslang_string_drop(&roundtrip);
    oreslang_cstring_drop(&cempty);
    oreslang_cstring_drop(&cplain);
    oreslang_cstring_drop(&cunicode);

    puts("native explicit-length String/CString ABI: PASS");
    return 0;
}
