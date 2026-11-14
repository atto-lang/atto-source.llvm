#ifndef ORES_NATIVE_FUNCTOR_FUSED_V0_H
#define ORES_NATIVE_FUNCTOR_FUSED_V0_H
/* Experimental C11 *fused scalar* tagged-sum ABI. NOT linked from .atto codegen.
   Bounded actor-owned slots, no malloc/GC; caller's compiler must enforce
   linear handles and full static type shapes. No nested affine layouts yet. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define FF_CAP 64u
#define FF_NIL UINT16_MAX

typedef enum { FF_OK=0, FF_STALE=1, FF_BORROWED=2, FF_TYPE=3, FF_FULL=4 } ff_status;
typedef enum { FF_OR=1, FF_RO=2, FF_OO=3, FF_O=4, FF_RR=5, FF_R=6 } ff_form;
typedef enum {
    FF_OR_NONE=1, FF_OR_SOME_OK=2, FF_OR_SOME_ERR=3,
    FF_RO_OK_NONE=4, FF_RO_OK_SOME=5, FF_RO_ERR=6,
    FF_OO_NONE=7, FF_OO_SOME_NONE=8, FF_OO_SOME_SOME=9,
    FF_O_NONE=10, FF_O_SOME=11,
    FF_RR_OUTER_ERR=12, FF_RR_INNER_ERR=13, FF_RR_OK=14,
    FF_R_ERR=15, FF_R_OK=16
} ff_tag;
typedef struct { uint16_t index; uint64_t generation; } ff_handle;
typedef struct {
    uint64_t generation;
    uint16_t next;
    uint8_t live, borrowed, form, tag, owns_resource;
    uint32_t error_domain;
    int64_t value;
} ff_cell;
typedef void (*ff_destroy_fn)(int64_t value, void* context);
typedef int64_t (*ff_map_fn)(int64_t value, void* context);
typedef struct {
    ff_cell cells[FF_CAP];
    uint16_t free_head, live_count, peak_count;
    uint32_t allocations, releases, destructors;
    ff_destroy_fn destroy;
    void* destroy_context;
} ff_arena;
static inline bool ff_tag_valid(ff_form form, ff_tag tag) {
    switch(form) {
        case FF_OR: return tag>=FF_OR_NONE && tag<=FF_OR_SOME_ERR;
        case FF_RO: return tag>=FF_RO_OK_NONE && tag<=FF_RO_ERR;
        case FF_OO: return tag>=FF_OO_NONE && tag<=FF_OO_SOME_SOME;
        case FF_O: return tag==FF_O_NONE || tag==FF_O_SOME;
        case FF_RR: return tag>=FF_RR_OUTER_ERR && tag<=FF_RR_OK;
        case FF_R: return tag==FF_R_ERR || tag==FF_R_OK;
        default: return false;
    }
}
static inline bool ff_has_value(ff_tag tag) {
    return tag!=FF_OR_NONE && tag!=FF_RO_OK_NONE && tag!=FF_OO_NONE &&
           tag!=FF_OO_SOME_NONE && tag!=FF_O_NONE;
}
static inline void ff_init(ff_arena* a, ff_destroy_fn drop, void* context) {
    if (!a) return;
    *a=(ff_arena){0};a->destroy=drop;a->destroy_context=context;
    for(uint16_t i=0;i<FF_CAP;i++) {
        a->cells[i].generation=1;
        a->cells[i].next=((unsigned)i+1u<FF_CAP)?(uint16_t)(i+1u):FF_NIL;
    }
    a->free_head=0;
}
static inline ff_cell* ff_get(ff_arena* a, ff_handle h) {
    if(!a||h.index>=FF_CAP) return NULL;
    ff_cell* s=&a->cells[h.index];
    return s->live && s->generation==h.generation?s:NULL;
}
static inline ff_status ff_make(ff_arena* a,ff_form form,ff_tag tag,uint32_t domain,
                                int64_t value,bool owns,ff_handle* out) {
    if(!a||!out||!ff_tag_valid(form,tag) ||
       ((form==FF_OR||form==FF_RO||form==FF_RR||form==FF_R)!=(domain!=0)) ||
       (owns && (!ff_has_value(tag)||!a->destroy)))return FF_TYPE;
    if(a->free_head==FF_NIL)return FF_FULL;
    uint16_t i=a->free_head;ff_cell* s=&a->cells[i];a->free_head=s->next;
    s->live=1;s->borrowed=0;s->form=(uint8_t)form;s->tag=(uint8_t)tag;
    s->error_domain=domain;s->value=value;s->owns_resource=owns?1:0;s->next=FF_NIL;
    out->index=i;out->generation=s->generation;
    ++a->live_count;++a->allocations;
    if(a->live_count>a->peak_count)a->peak_count=a->live_count;
    return FF_OK;
}
static inline ff_status ff_borrow(ff_arena* a,ff_handle h) {
    ff_cell* s=ff_get(a,h);if(!s)return FF_STALE;
    if(s->borrowed)return FF_BORROWED;
    s->borrowed=1;return FF_OK;
}
static inline ff_status ff_end_borrow(ff_arena* a,ff_handle h) {
    ff_cell* s=ff_get(a,h);if(!s)return FF_STALE;
    if(!s->borrowed)return FF_TYPE;
    s->borrowed=0;return FF_OK;
}
static inline ff_status ff_transpose(ff_arena* a,ff_handle h) {
    ff_cell* s=ff_get(a,h);if(!s)return FF_STALE;
    if(s->borrowed)return FF_BORROWED;
    if(!ff_tag_valid((ff_form)s->form,(ff_tag)s->tag))return FF_TYPE;
    if(s->form==FF_OR) {
        s->form=FF_RO;
        s->tag=s->tag==FF_OR_NONE?FF_RO_OK_NONE:
               s->tag==FF_OR_SOME_OK?FF_RO_OK_SOME:FF_RO_ERR;
    }else if(s->form==FF_RO) {
        s->form=FF_OR;
        s->tag=s->tag==FF_RO_OK_NONE?FF_OR_NONE:
               s->tag==FF_RO_OK_SOME?FF_OR_SOME_OK:FF_OR_SOME_ERR;
    }else return FF_TYPE;
    return FF_OK;
}
/* Always removes exactly one wrapper, preserving nominal E and payload owner. */
static inline ff_status ff_flatten(ff_arena* a,ff_handle h) {
    ff_cell* s=ff_get(a,h);if(!s)return FF_STALE;
    if(s->borrowed)return FF_BORROWED;
    if(!ff_tag_valid((ff_form)s->form,(ff_tag)s->tag))return FF_TYPE;
    if(s->form==FF_OO) {
        s->form=FF_O;
        s->tag=s->tag==FF_OO_SOME_SOME?FF_O_SOME:FF_O_NONE;
    }else if(s->form==FF_RR) {
        s->form=FF_R;
        s->tag=s->tag==FF_RR_OK?FF_R_OK:FF_R_ERR;
    }else return FF_TYPE;
    return FF_OK;
}
/* The callback is deliberately scalar-only: no implicit F<G<T>> recursion. */
static inline ff_status ff_map_i64(ff_arena* a,ff_handle h,ff_map_fn fn,void* context) {
    ff_cell* s=ff_get(a,h);if(!s)return FF_STALE;
    if(s->borrowed)return FF_BORROWED;
    if(!ff_tag_valid((ff_form)s->form,(ff_tag)s->tag))return FF_TYPE;
    if(!fn||s->owns_resource)return FF_TYPE;
    if(s->form!=FF_O && s->form!=FF_R)return FF_TYPE;
    if(s->tag==FF_O_SOME || s->tag==FF_R_OK)s->value=fn(s->value,context);
    return FF_OK;
}
static inline ff_status ff_map_err_i64(ff_arena* a,ff_handle h,uint32_t new_domain,
                                       ff_map_fn fn,void* context) {
    ff_cell* s=ff_get(a,h);if(!s)return FF_STALE;
    if(s->borrowed)return FF_BORROWED;
    if(!ff_tag_valid((ff_form)s->form,(ff_tag)s->tag))return FF_TYPE;
    if(s->form!=FF_R||!new_domain||!fn||s->owns_resource)return FF_TYPE;
    if(s->tag==FF_R_ERR)s->value=fn(s->value,context);
    s->error_domain=new_domain;return FF_OK;
}
static inline ff_status ff_drop(ff_arena* a,ff_handle* h) {
    if(!h)return FF_TYPE;
    ff_cell* s=ff_get(a,*h);if(!s)return FF_STALE;
    if(s->borrowed)return FF_BORROWED;
    bool owned=s->owns_resource;int64_t value=s->value;
    uint16_t i=h->index;
    s->live=0;
    if(s->generation==UINT64_MAX){
        /* Permanently retire on exhausted generation identity rather than
         * permitting a stale historical handle to become valid again. */
        s->next=FF_NIL;
    }else{
        ++s->generation;
        s->next=a->free_head;a->free_head=i;
    }
    --a->live_count;++a->releases;
    h->index=FF_NIL;h->generation=0;
    if(owned){++a->destructors;a->destroy(value,a->destroy_context);}
    return FF_OK;
}
#endif
