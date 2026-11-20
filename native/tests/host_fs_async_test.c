#include "atto/host_fs_async.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* In isolated local builds we use an in-memory broker mock. The source CI
 * links to the REAL scoped broker; mocks must never count as FS isolation. */
static pthread_mutex_t broker_lock=PTHREAD_MUTEX_INITIALIZER;
static char stored[128]="seed";
static int revoked=0;
static int defer_publication_once=0;
static int observed_deferred_publication=0;
static pthread_mutex_t io_gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_signal=PTHREAD_COND_INITIALIZER;
static int hold_write=0;
static int write_started=0;
int atto_fs_broker_read(atto_fs_broker *b,uint64_t t,uint64_t g,const char *p,unsigned char *out,size_t cap,size_t *size){
    pthread_mutex_lock(&broker_lock);
    int rc=(revoked||t!=b->tenant_id||g!=b->generation||strcmp(p,"data/input"))?-EACCES:0;
    if(!rc){*size=strlen(stored);if(*size>cap)rc=-EFBIG;else memcpy(out,stored,*size);}
    pthread_mutex_unlock(&broker_lock);return rc;
}
int atto_fs_broker_write(atto_fs_broker *b,uint64_t t,uint64_t g,const char *p,const unsigned char *in,size_t len){
    pthread_mutex_lock(&io_gate);
    if (hold_write) {
        write_started=1;
        pthread_cond_broadcast(&io_signal);
        while (hold_write) pthread_cond_wait(&io_signal,&io_gate);
    }
    pthread_mutex_unlock(&io_gate);
    pthread_mutex_lock(&broker_lock);
    int rc=(revoked||t!=b->tenant_id||g!=b->generation||strcmp(p,"out/copy")||len>=sizeof(stored))?-EACCES:0;
    if(!rc){memcpy(stored,in,len);stored[len]=0;}
    pthread_mutex_unlock(&broker_lock);return rc;
}
int atto_fs_broker_publish_read(atto_fs_broker *b,uint64_t t,uint64_t g,const char *p,const unsigned char *in,size_t len,unsigned char *out,size_t cap){
    if (defer_publication_once) {
        defer_publication_once=0;
        observed_deferred_publication=1;
        return -EAGAIN; /* Simulate a contended real broker revoke gate. */
    }
    pthread_mutex_lock(&broker_lock);
    int rc=(revoked||t!=b->tenant_id||g!=b->generation||strcmp(p,"data/input")||len>cap)?-EACCES:0;
    if(!rc)memcpy(out,in,len);
    pthread_mutex_unlock(&broker_lock);return rc;
}
static int finished(atto_fs_async_service *s,atto_fs_host_actor *a,uint64_t ticket,unsigned char *out,size_t cap,size_t *n){
  for(int i=0;i<1000000;++i){int rc=atto_fs_async_poll(s,a,ticket,out,cap,n);if(rc!=-EAGAIN)return rc;if(i%200==0)sched_yield();}return -ETIMEDOUT;
}
int main(void){
  atto_fs_broker broker={0};
  broker.tenant_id=71; broker.generation=4; broker.policy_layer_count=6;
  atto_fs_async_service *s=NULL;
  assert(atto_fs_async_create(&s,&broker,4)==0);
  atto_fs_host_actor actor={71,4,10,2},other={71,4,11,2};
  unsigned char readbuf[80];memset(readbuf,0xBC,sizeof readbuf);size_t n=99;
  uint64_t ticket=0;
  assert(atto_fs_async_submit_read(s,&actor,"data/input",20,&ticket)==0 && ticket!=0);
  assert(atto_fs_async_poll(s,&other,ticket,readbuf,sizeof readbuf,&n)==-EACCES && n==0 && readbuf[0]==0xBC);
  assert(finished(s,&actor,ticket,readbuf,sizeof readbuf,&n)==0 && n==4 && memcmp(readbuf,"seed",4)==0);
  assert(atto_fs_async_poll(s,&actor,ticket,readbuf,sizeof readbuf,&n)==-ESTALE);
  /* A transient broker mutex conflict MUST NOT consume a ready ticket. */
  defer_publication_once=1;
  assert(atto_fs_async_submit_read(s,&actor,"data/input",20,&ticket)==0);
  memset(readbuf,0xA4,sizeof readbuf);
  assert(finished(s,&actor,ticket,readbuf,sizeof readbuf,&n)==0);
  assert(observed_deferred_publication==1 && n==4 && memcmp(readbuf,"seed",4)==0);
  assert(atto_fs_async_poll(s,&actor,ticket,readbuf,sizeof readbuf,&n)==-ESTALE);
  unsigned char payload[]="updated";
  assert(atto_fs_async_submit_write(s,&actor,"out/copy",payload,7,&ticket)==0);
  payload[0]='X'; /* accepted job has copied the guest input */
  assert(finished(s,&actor,ticket,NULL,0,&n)==0);
  assert(strcmp(stored,"updated")==0);
  assert(atto_fs_async_cancel(s,&actor,ticket)==-ESTALE);
  /* Hold the real worker in a write to prove that a second queued write
   * can be cancelled before execution, whereas the running write has an
   * indeterminate side-effect outcome even after a cancel request. */
  uint64_t running=0,queued=0;
  pthread_mutex_lock(&io_gate); hold_write=1; write_started=0; pthread_mutex_unlock(&io_gate);
  assert(atto_fs_async_submit_write(s,&actor,"out/copy",(const unsigned char*)"started",7,&running)==0);
  pthread_mutex_lock(&io_gate);
  while(!write_started) pthread_cond_wait(&io_signal,&io_gate);
  pthread_mutex_unlock(&io_gate);
  assert(atto_fs_async_submit_write(s,&actor,"out/copy",(const unsigned char*)"queued",6,&queued)==0);
  int cr;
  do { cr=atto_fs_async_cancel(s,&actor,queued); } while(cr==-EAGAIN);
  assert(cr==0);
  assert(finished(s,&actor,queued,NULL,0,&n)==-ECANCELED);
  do { cr=atto_fs_async_cancel(s,&actor,running); } while(cr==-EAGAIN);
  assert(cr==0);
  pthread_mutex_lock(&io_gate); hold_write=0; pthread_cond_broadcast(&io_signal); pthread_mutex_unlock(&io_gate);
  assert(finished(s,&actor,running,NULL,0,&n)==-EINPROGRESS);
  assert(strcmp(stored,"started")==0);
  assert(atto_fs_async_submit_read(s,&actor,"data/input",30,&ticket)==0);
  pthread_mutex_lock(&broker_lock);revoked=1;pthread_mutex_unlock(&broker_lock);
  memset(readbuf,0xAE,sizeof readbuf);
  assert(finished(s,&actor,ticket,readbuf,sizeof readbuf,&n)==-EACCES);
  assert(n==0 && readbuf[0]==0xAE);
  assert(atto_fs_async_submit_read(s,&actor,"data/input",30,&ticket)==0);
  assert(atto_fs_async_forget_actor(s,&actor)==0);
  assert(finished(s,&actor,ticket,readbuf,sizeof readbuf,&n)==-ESTALE);
  atto_fs_async_destroy(s);
  broker.policy_layer_count=0;
  assert(atto_fs_async_create(&s,&broker,4)==-EACCES);
  puts("ATTO_FS_ASYNC_HOST_ONLY_TEST_PASS");return 0;
}
