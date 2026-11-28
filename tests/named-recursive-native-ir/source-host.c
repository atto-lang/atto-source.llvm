#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
extern int32_t atto_named_main(int64_t*);
int main(int argc,char** argv){
    assert(argc==3);
    const int32_t status=(int32_t)strtol(argv[1],0,10);
    const int64_t want=strtoll(argv[2],0,10);
    for(int i=0;i<3;i++){
        int64_t out=123456789;
        assert(atto_named_main(&out)==status);
        assert(out==(status?123456789:want));
    }
}
