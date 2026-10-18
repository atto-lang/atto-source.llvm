#include <stdio.h>
int firmware_main(void);
int main(void) {
    int code=firmware_main();
#ifdef NEGATIVE_GUEST
    if(code!=99){fprintf(stderr,"NEGATIVE_FW_UNEXPECTED_CODE %d\n",code);return 1;}
    puts("ABI_FIRMWARE_NEGATIVE_EXPECTED_FAILURE_PASS");
#else
    if(code!=0){fprintf(stderr,"POSITIVE_FW_FAILED %d\n",code);return 1;}
    puts("ABI_FIRMWARE_POSITIVE_PASS");
#endif
    return 0;
}
