#include "oreslang/runtime/ActorElasticNativeArenaV1.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t offset, length;
    uint32_t generation;
    uint8_t live;
} ElasticSlotV1;
struct OresElasticNativeArenaV1 {
    uint64_t actor_id, incarnation, domain;
    size_t hard_limit, committed, cursor, live;
    uint32_t max_slots;
    ElasticSlotV1* slots;
    uint8_t* native_bytes;
    OresElasticNativeBudgetV1 *tenant, *process;
#ifdef ORES_ELASTIC_TESTING
    int fail_next_growth;
#endif
};
static _Atomic(uint64_t) next_domain = 1;
static void scrub(void* ptr, size_t size) {
    volatile uint8_t* bytes = (volatile uint8_t*)ptr;
    for (size_t i=0; i<size; ++i) bytes[i]=0;
}
static int reserve(OresElasticNativeBudgetV1* budget, size_t bytes) {
    if (!budget || !bytes) return 0;
    size_t used = atomic_load_explicit(&budget->charged_bytes, memory_order_relaxed);
    do {
        if (used > budget->limit_bytes || bytes > budget->limit_bytes-used) return 0;
    } while (!atomic_compare_exchange_weak_explicit(&budget->charged_bytes,
        &used, used+bytes, memory_order_acq_rel, memory_order_relaxed));
    return 1;
}
static void refund(OresElasticNativeBudgetV1* budget, size_t bytes) {
    if (!bytes) return;
    /* Trusted host invariant; reject inconsistent budgets, never underflow. */
    size_t used = atomic_load_explicit(&budget->charged_bytes, memory_order_relaxed);
    do {
        if (bytes > used) abort();
    } while (!atomic_compare_exchange_weak_explicit(&budget->charged_bytes,
        &used, used-bytes, memory_order_acq_rel, memory_order_relaxed));
}
int ores_elastic_budget_init_v1(OresElasticNativeBudgetV1* budget, size_t limit_bytes) {
    if (!budget || !limit_bytes) return 0;
    budget->limit_bytes=limit_bytes;
    atomic_init(&budget->charged_bytes,0);
    return 1;
}
size_t ores_elastic_budget_used_v1(const OresElasticNativeBudgetV1* budget) {
    return budget ? atomic_load_explicit(&budget->charged_bytes,memory_order_acquire) : 0;
}
OresElasticNativeArenaV1* ores_elastic_native_open_v1(
    uint64_t actor_id, uint64_t incarnation, size_t hard_limit_bytes,
    uint32_t max_slots, OresElasticNativeBudgetV1* tenant,
    OresElasticNativeBudgetV1* process) {
    if (!actor_id || !incarnation || !hard_limit_bytes ||
        hard_limit_bytes>ORES_ELASTIC_NATIVE_MAX_BYTES_V1 ||
        !max_slots || max_slots>ORES_ELASTIC_NATIVE_MAX_SLOTS_V1 ||
        !tenant || !process) return NULL;
    OresElasticNativeArenaV1* a=calloc(1,sizeof(*a));
    if(!a) return NULL;
    a->slots=calloc(max_slots,sizeof(*a->slots));
    if(!a->slots){free(a);return NULL;}
    const uint64_t id=atomic_fetch_add_explicit(&next_domain,1,memory_order_relaxed);
    if(!id || id==UINT64_MAX){free(a->slots);free(a);return NULL;}
    a->actor_id=actor_id;a->incarnation=incarnation;a->domain=id;
    a->max_slots=max_slots;a->hard_limit=hard_limit_bytes;
    a->tenant=tenant;a->process=process;
    for(uint32_t i=0;i<max_slots;++i) a->slots[i].generation=1;
    return a; /* Zero native payload backing until first allocation. */
}
static int grow(OresElasticNativeArenaV1* a,size_t required) {
    if(required<=a->committed)return 1;
    if(required>a->hard_limit)return 0;
    size_t next=a->committed?a->committed:
        (a->hard_limit<4096?a->hard_limit:4096);
    while(next<required){
        if(next>a->hard_limit/2){next=a->hard_limit;break;}
        next*=2;
    }
    /* Replacement must be fully charged concurrently with old backing.
     * The tenant and process charges are independent and rollback is atomic. */
    if(!reserve(a->tenant,next))return 0;
    const int separate=a->tenant!=a->process;
    if(separate && !reserve(a->process,next)){
        refund(a->tenant,next);return 0;
    }
    uint8_t* fresh=NULL;
#ifdef ORES_ELASTIC_TESTING
    if(a->fail_next_growth){a->fail_next_growth=0;}
    else
#endif
        fresh=calloc(1,next); /* Touch/copy backing before old block released. */
    if(!fresh){
        if(separate)refund(a->process,next);
        refund(a->tenant,next);
        return 0;
    }
    if(a->native_bytes && a->cursor)memcpy(fresh,a->native_bytes,a->cursor);
    uint8_t* old=a->native_bytes;
    const size_t old_committed=a->committed;
    a->native_bytes=fresh;a->committed=next;
    if(old){
        scrub(old,old_committed);free(old);
        if(separate)refund(a->process,old_committed);
        refund(a->tenant,old_committed);
    }
    return 1;
}
static ElasticSlotV1* checked(OresElasticNativeArenaV1* a,OresElasticNativeHandleV1 h) {
    if(!a || h.domain!=a->domain || h.incarnation!=a->incarnation ||
       h.word==0 || h.generation==0 || h.slot>=a->max_slots) return NULL;
    ElasticSlotV1* slot=&a->slots[h.slot];
    const size_t offset=((size_t)h.word-1)*8;
    if(!slot->live || slot->offset!=offset || slot->generation!=h.generation)
        return NULL;
    return slot;
}
int ores_elastic_native_allocate_v1(OresElasticNativeArenaV1* a,size_t bytes,
                                    uint32_t alignment,OresElasticNativeHandleV1* out) {
    if(!a || !out || !bytes || alignment<8 || alignment>64 ||
       (alignment&(alignment-1)))return 0;
    const size_t mask=(size_t)alignment-1;
    if(a->cursor>SIZE_MAX-mask)return 0;
    const size_t offset=(a->cursor+mask)&~mask;
    if(offset>a->hard_limit || bytes>a->hard_limit-offset)return 0;
    uint32_t free_slot=a->max_slots;
    for(uint32_t i=0;i<a->max_slots;++i)
        if(!a->slots[i].live && a->slots[i].generation!=UINT32_MAX){free_slot=i;break;}
    if(free_slot==a->max_slots)return 0;
    const size_t end=offset+bytes;
    if(!grow(a,end))return 0;
    ElasticSlotV1* slot=&a->slots[free_slot];
    scrub(a->native_bytes+offset,bytes);
    slot->offset=offset;slot->length=bytes;slot->live=1;
    a->cursor=end;++a->live;
    *out=(OresElasticNativeHandleV1){a->domain,a->incarnation,
                                    (uint32_t)(offset/8+1),free_slot,slot->generation};
    return 1;
}
int ores_elastic_native_write_i64_v1(OresElasticNativeArenaV1* a,
    OresElasticNativeHandleV1 h,size_t relative_offset,int64_t value) {
    ElasticSlotV1* s=checked(a,h);
    if(!s || relative_offset>s->length || sizeof(value)>s->length-relative_offset)return 0;
    memcpy(a->native_bytes+s->offset+relative_offset,&value,sizeof(value));return 1;
}
int ores_elastic_native_read_i64_v1(OresElasticNativeArenaV1* a,
    OresElasticNativeHandleV1 h,size_t relative_offset,int64_t* out) {
    ElasticSlotV1* s=checked(a,h);
    if(!s || !out || relative_offset>s->length || sizeof(*out)>s->length-relative_offset)return 0;
    memcpy(out,a->native_bytes+s->offset+relative_offset,sizeof(*out));return 1;
}
int ores_elastic_native_drop_v1(OresElasticNativeArenaV1* a,OresElasticNativeHandleV1 h) {
    ElasticSlotV1* s=checked(a,h);
    if(!s)return 0;
    scrub(a->native_bytes+s->offset,s->length);
    s->live=0;
    if(s->generation!=UINT32_MAX)++s->generation;
    --a->live;
    if(!a->live){scrub(a->native_bytes,a->cursor);a->cursor=0;}
    return 1;
}
size_t ores_elastic_native_committed_v1(const OresElasticNativeArenaV1* a){return a?a->committed:0;}
size_t ores_elastic_native_live_v1(const OresElasticNativeArenaV1* a){return a?a->live:0;}
void ores_elastic_native_close_v1(OresElasticNativeArenaV1* a) {
    if(!a)return; /* Supervisor must have fenced outstanding host access. */
    if(a->native_bytes){
        scrub(a->native_bytes,a->committed);free(a->native_bytes);
        if(a->tenant!=a->process)refund(a->process,a->committed);
        refund(a->tenant,a->committed);
    }
    scrub(a->slots,(size_t)a->max_slots*sizeof(*a->slots));free(a->slots);
    scrub(a,sizeof(*a));free(a);
}
#ifdef ORES_ELASTIC_TESTING
void ores_elastic_native_fail_next_growth_for_test_v1(OresElasticNativeArenaV1* a){if(a)a->fail_next_growth=1;}
#endif
