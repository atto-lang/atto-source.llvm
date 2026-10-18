static volatile unsigned firmware_bss_witness;
static volatile unsigned firmware_data_witness = 0x76543210u;
#include "oreslang/runtime/FreestandingSourceActor.h"
static void oreslang_actor_Source_init(OresActorI64Heap *h,uint64_t d,uint64_t e){
    ores_actor_i64_heap_field_init(h,d,e,0,40);
    ores_actor_i64_heap_field_init_mutable(h,d,e,1,0);
}
static int64_t oreslang_actor_Source_on_message(OresActorI64Heap *h,uint64_t d,uint64_t e,int64_t n){
    int64_t sum=ores_actor_i64_heap_field_read(h,d,e,1)+
                ores_actor_i64_heap_field_read(h,d,e,0)+n;
    ores_actor_i64_heap_field_write(h,d,e,1,sum);
    return sum;
}
static void oreslang_actor_Sink_init(OresActorI64Heap *h,uint64_t d,uint64_t e){
    ores_actor_i64_heap_field_init_mutable(h,d,e,0,5);
}
static int64_t oreslang_actor_Sink_on_message(OresActorI64Heap *h,uint64_t d,uint64_t e,int64_t n){
    int64_t sum=ores_actor_i64_heap_field_read(h,d,e,0)+n;
    ores_actor_i64_heap_field_write(h,d,e,0,sum);
    return sum;
}
int firmware_main(void){
    if (firmware_bss_witness != 0 || firmware_data_witness != 0x76543210u) return 70;
    void *a=oreslang_source_actor_spawn(oreslang_actor_Source_init,oreslang_actor_Source_on_message,2);
    void *b=oreslang_source_actor_spawn(oreslang_actor_Sink_init,oreslang_actor_Sink_on_message,1);
    oreslang_source_actor_send(a,1);
    oreslang_source_actor_send(b,11);
    oreslang_source_actor_send(a,2);
    oreslang_source_actor_send(b,3);
    oreslang_source_actor_stop(a);
    oreslang_source_actor_stop(b);
    OresFreestandingActorStats x;
    ores_freestanding_source_stats(&x);
    if (!(x.spawned==2 && x.retired==2 && x.delivered==4 &&
            x.last_result[0]==83 && x.last_result[1]==19)) return 44;
#ifdef NEGATIVE_GUEST
    return 99;
#else
    return 0;
#endif
}
