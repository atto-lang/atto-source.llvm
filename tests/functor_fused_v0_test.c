#include "NativeFunctorFusedV0.h"
#include <assert.h>
#include <stdio.h>
static unsigned drops,callbacks;static int64_t drop_sum;
static void destroy(int64_t v,void*ctx){(void)ctx;++drops;drop_sum+=v;}
static int64_t add_two(int64_t v,void*ctx){(void)ctx;++callbacks;return v+2;}
int main(void){
  ff_arena a;ff_init(&a,destroy,NULL);ff_handle h,stale;
  for(unsigned n=FF_OR_NONE;n<=FF_OR_SOME_ERR;n++){
    assert(ff_make(&a,FF_OR,(ff_tag)n,99,42,n==FF_OR_SOME_OK,&h)==FF_OK);
    stale=h;assert(ff_borrow(&a,h)==FF_OK);
    assert(ff_transpose(&a,h)==FF_BORROWED && ff_drop(&a,&h)==FF_BORROWED);
    assert(ff_end_borrow(&a,h)==FF_OK);
    assert(ff_transpose(&a,h)==FF_OK && ff_get(&a,h)->form==FF_RO);
    assert(ff_transpose(&a,h)==FF_OK && ff_get(&a,h)->tag==n);
    assert(ff_map_i64(&a,h,add_two,NULL)==FF_TYPE);
    assert(ff_drop(&a,&h)==FF_OK && ff_drop(&a,&stale)==FF_STALE);
  }
  assert(callbacks==0 && drops==1 && drop_sum==42);
  for(unsigned n=FF_RO_OK_NONE;n<=FF_RO_ERR;n++){
    assert(ff_make(&a,FF_RO,(ff_tag)n,99,20,false,&h)==FF_OK);
    assert(ff_transpose(&a,h)==FF_OK && ff_transpose(&a,h)==FF_OK);
    assert(ff_get(&a,h)->tag==n);
    assert(ff_drop(&a,&h)==FF_OK);
  }
  for(unsigned n=FF_OO_NONE;n<=FF_OO_SOME_SOME;n++){
    assert(ff_make(&a,FF_OO,(ff_tag)n,0,9,n==FF_OO_SOME_SOME,&h)==FF_OK);
    assert(ff_flatten(&a,h)==FF_OK && ff_get(&a,h)->form==FF_O);
    assert(ff_flatten(&a,h)==FF_TYPE);
    assert(ff_drop(&a,&h)==FF_OK);
  }
  assert(drops==2 && drop_sum==51);
  for(unsigned n=FF_RR_OUTER_ERR;n<=FF_RR_OK;n++){
    assert(ff_make(&a,FF_RR,(ff_tag)n,44,7,n==FF_RR_OK,&h)==FF_OK);
    assert(ff_flatten(&a,h)==FF_OK && ff_get(&a,h)->form==FF_R);
    assert(ff_drop(&a,&h)==FF_OK);
  }
  assert(drops==3 && drop_sum==58);
  assert(ff_make(&a,FF_R,FF_R_OK,45,88,true,&h)==FF_OK);
  assert(ff_map_i64(&a,h,add_two,NULL)==FF_TYPE);
  assert(ff_map_err_i64(&a,h,46,add_two,NULL)==FF_TYPE);
  assert(ff_get(&a,h)->value==88 && ff_get(&a,h)->error_domain==45);
  assert(ff_drop(&a,&h)==FF_OK);
  assert(drops==4 && drop_sum==146);
  assert(ff_make(&a,FF_R,FF_R_OK,44,10,false,&h)==FF_OK);
  assert(ff_map_i64(&a,h,add_two,NULL)==FF_OK);
  assert(ff_get(&a,h)->value==12 && callbacks==1);
  assert(ff_map_err_i64(&a,h,45,add_two,NULL)==FF_OK && callbacks==1);
  assert(ff_get(&a,h)->error_domain==45);
  assert(ff_drop(&a,&h)==FF_OK);
  assert(ff_make(&a,FF_R,FF_R_ERR,44,8,false,&h)==FF_OK);
  assert(ff_map_i64(&a,h,add_two,NULL)==FF_OK && callbacks==1);
  assert(ff_map_err_i64(&a,h,45,add_two,NULL)==FF_OK && callbacks==2);
  assert(ff_get(&a,h)->value==10);
  assert(ff_drop(&a,&h)==FF_OK);
  assert(ff_make(&a,FF_OR,FF_OR_SOME_OK,77,42,false,&h)==FF_OK);
  ff_cell* corrupted=ff_get(&a,h);
  assert(corrupted!=NULL);
  corrupted->tag=FF_O_SOME; // invalid combination; cannot silently become Err
  assert(ff_transpose(&a,h)==FF_TYPE);
  assert(ff_map_i64(&a,h,add_two,NULL)==FF_TYPE);
  corrupted->tag=FF_OR_SOME_OK;
  assert(ff_transpose(&a,h)==FF_OK);
  assert(ff_drop(&a,&h)==FF_OK);
  assert(ff_make(&a,FF_R,FF_R_OK,77,2,false,&h)==FF_OK);
  assert(ff_borrow(&a,h)==FF_OK);
  assert(ff_map_i64(&a,h,add_two,NULL)==FF_BORROWED);
  assert(ff_map_err_i64(&a,h,78,add_two,NULL)==FF_BORROWED);
  assert(ff_end_borrow(&a,h)==FF_OK);
  assert(ff_drop(&a,&h)==FF_OK);
  ff_handle all[FF_CAP];
  for(unsigned i=0;i<FF_CAP;i++)assert(ff_make(&a,FF_O,FF_O_NONE,0,0,false,&all[i])==FF_OK);
  assert(ff_make(&a,FF_O,FF_O_NONE,0,0,false,&h)==FF_FULL);
  for(unsigned i=0;i<FF_CAP;i++)assert(ff_drop(&a,&all[i])==FF_OK);
  assert(a.live_count==0 && a.peak_count==FF_CAP);
  assert(a.allocations==a.releases);
  /* Simulate exhaustion of one handle generation: release must RETIRE its
     slot, preventing historical handles from reviving on wrap-around. */
  assert(ff_make(&a,FF_O,FF_O_NONE,0,0,false,&h)==FF_OK);
  ff_cell* last=ff_get(&a,h);
  last->generation=UINT64_MAX;
  h.generation=UINT64_MAX;
  stale=h;
  assert(ff_drop(&a,&h)==FF_OK);
  assert(ff_get(&a,stale)==NULL);
  for(unsigned i=0;i<FF_CAP-1u;i++)
    assert(ff_make(&a,FF_O,FF_O_NONE,0,0,false,&all[i])==FF_OK);
  assert(ff_make(&a,FF_O,FF_O_NONE,0,0,false,&h)==FF_FULL);
  for(unsigned i=0;i<FF_CAP-1u;i++)assert(ff_drop(&a,&all[i])==FF_OK);
  assert(a.live_count==0 && a.allocations==a.releases);
  printf("PASS native bounded fused Option/Result ownership ABI; GCC+Clang; guest=NOT_EXECUTED\n");
}
