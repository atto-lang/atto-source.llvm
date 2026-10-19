/* White-box, single-TU property test of actual ready-index source. */
#include "oreslang/runtime/NativeActorScheduler.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t rng = UINT32_C(0x7f19ab83);
static uint32_t sample(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static size_t reference(const OresNativeActorScheduler* s, size_t start, size_t worker, int ctrl, int prefer) {
    for (size_t off=0; off<s->actor_count; ++off) {
        size_t actor=(start+off)%s->actor_count;
        const ActorLane* lane=&s->actors[actor];
        if (lane->active) continue;
        if (ctrl ? !lane->has_control : !lane->queued) continue;
        if (prefer && lane->last_carrier_plus_one != worker+1) continue;
        return actor;
    }
    return SIZE_MAX;
}
static size_t exercise(size_t count) {
    OresNativeActorScheduler s={0};
    s.actor_count=count; s.ready_word_count=(count+63)/64;
    s.actors=calloc(count,sizeof(*s.actors));
    s.ready_control_bits=calloc(s.ready_word_count,sizeof(uint64_t));
    s.ready_data_bits=calloc(s.ready_word_count,sizeof(uint64_t));
    assert(s.actors && s.ready_control_bits && s.ready_data_bits);
    size_t checks=0;
    for (size_t step=0; step<250; ++step) {
        for (int j=0;j<24;++j) {
            size_t i=sample()%count;
            ActorLane* a=&s.actors[i];
            a->active=sample()&1; a->queued=sample()&1;
            a->has_control=sample()&1;
            a->last_carrier_plus_one=sample()%5;
            refresh_ready_locked(&s,i);
        }
        for (size_t w=0;w<s.ready_word_count;++w) {
            uint64_t bit=UINT64_C(1)<<(w%64);
            size_t sw=w/64;
            assert(!!s.ready_control_bits[w] == !!(s.ready_control_summary[sw]&bit));
            assert(!!s.ready_data_bits[w] == !!(s.ready_data_summary[sw]&bit));
        }
        size_t start=sample()%count,worker=sample()%4;
        for(int ctrl=0;ctrl<2;++ctrl)for(int prefer=0;prefer<=(ctrl?0:1);++prefer) {
            const uint64_t* bits=ctrl?s.ready_control_bits:s.ready_data_bits;
            const uint64_t* sum=ctrl?s.ready_control_summary:s.ready_data_summary;
            size_t got=select_ready_locked(&s,bits,sum,start,worker,prefer);
            size_t want=reference(&s,start,worker,ctrl,prefer);
            if(got!=want) {
                fprintf(stderr,"index mismatch actors=%zu start=%zu ctrl=%d preferred=%d got=%zu wanted=%zu\n",
                    count,start,ctrl,prefer,got,want);
                abort();
            }
            ++checks;
        }
    }
    free(s.ready_data_bits); free(s.ready_control_bits);free(s.actors);
    return checks;
}
int main(void) {
    const size_t sizes[]={1,2,63,64,65,127,128,129,1023,1024,1025,
        4095,4096,4097,16383,16384,16385,65535,65536};
    size_t checks=0;
    for(size_t i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i)checks+=exercise(sizes[i]);
    printf("NATIVE_READY_SUMMARY_ORACLE_PASS comparisons=%zu\n",checks);
    return 0;
}
