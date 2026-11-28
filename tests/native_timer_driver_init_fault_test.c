#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeTimerDriver.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
static int fail, attrs, mutexes, conds;
static atomic_uint fired;
int __real_pthread_condattr_init(pthread_condattr_t*);
int __real_pthread_condattr_setclock(pthread_condattr_t*, clockid_t);
int __real_pthread_condattr_destroy(pthread_condattr_t*);
int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int __real_pthread_mutex_destroy(pthread_mutex_t*);
int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int __real_pthread_cond_destroy(pthread_cond_t*);
int __real_pthread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
int __wrap_pthread_condattr_init(pthread_condattr_t* a) { if (fail==1) return ENOMEM; int r=__real_pthread_condattr_init(a); if(!r) attrs++; return r; }
int __wrap_pthread_condattr_setclock(pthread_condattr_t* a, clockid_t c) { assert(attrs==1); if(fail==2) return EINVAL; return __real_pthread_condattr_setclock(a,c); }
int __wrap_pthread_condattr_destroy(pthread_condattr_t* a) { assert(attrs==1); attrs--; return __real_pthread_condattr_destroy(a); }
int __wrap_pthread_mutex_init(pthread_mutex_t* a,const pthread_mutexattr_t* b) { if(fail==3) return EAGAIN; int r=__real_pthread_mutex_init(a,b); if(!r) mutexes++; return r; }
int __wrap_pthread_mutex_destroy(pthread_mutex_t* a) { assert(mutexes==1); mutexes--; return __real_pthread_mutex_destroy(a); }
int __wrap_pthread_cond_init(pthread_cond_t* a,const pthread_condattr_t* b) { if(fail==4) return EAGAIN; int r=__real_pthread_cond_init(a,b); if(!r) conds++; return r; }
int __wrap_pthread_cond_destroy(pthread_cond_t* a) { assert(conds==1); conds--; return __real_pthread_cond_destroy(a); }
int __wrap_pthread_create(pthread_t* a,const pthread_attr_t* b,void*(*c)(void*),void* d) { if(fail==5) return EAGAIN; return __real_pthread_create(a,b,c,d); }
static void on_fire(void* ctx, uint64_t payload) { (void)ctx; assert(payload==42); atomic_fetch_add(&fired,1); }
int main(void) {
  OresTimerDriver d;
  for(fail=1;fail<=5;fail++) {
    assert(!ores_timer_driver_start(&d,8,1000000,on_fire,NULL));
    assert(!d.started && !d.wheel.deadline && !d.wheel.payload && !d.wheel.next && !d.wheel.prev && !d.wheel.where && !d.wheel.generation);
    assert(attrs==0 && mutexes==0 && conds==0);
    ores_timer_driver_stop(&d);
  }
  fail=0;
  assert(ores_timer_driver_start(&d,8,1000000,on_fire,NULL));
  uint64_t far=ores_timer_driver_arm_after(&d,1000000000,0);
  assert(far && ores_timer_driver_arm_after(&d,1000000,42));
  struct timespec t={0,1000000};
  for(int i=0;i<500 && !atomic_load(&fired);i++) nanosleep(&t,NULL);
  assert(atomic_load(&fired)==1 && ores_timer_driver_cancel(&d,far));
  ores_timer_driver_stop(&d);
  assert(!d.started && !attrs && !mutexes && !conds);
  puts("five initialization failures and one real driver lifecycle passed");
}
