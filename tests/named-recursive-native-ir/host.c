#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
extern int32_t atto_named_scalar(int64_t, int64_t*);
int main(int argc,char** argv){
    assert(argc==4);
    int64_t input=strtoll(argv[1],0,10),want=strtoll(argv[3],0,10);
    int32_t status=(int32_t)strtol(argv[2],0,10);
    for(int i=0;i<3;i++){
        int64_t out=123456789;
        assert(atto_named_scalar(input,&out)==status);
        assert(out==(status?123456789:want));
    }
}
