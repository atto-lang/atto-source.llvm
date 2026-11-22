#include "atto/host_udp_loopback.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
static int granted = 1;
static unsigned denied_port = 0;
static unsigned denied_source_port = 0;
static atomic_int hold_send_authorize = 0;
static atomic_int send_entered = 0;
static int authorize(void *context, unsigned port, int operation) {
    if (operation == ATTO_UDP_SEND && atomic_load(&hold_send_authorize)) {
        atomic_store(&send_entered, 1);
        while (atomic_load(&hold_send_authorize)) sched_yield();
    }
    (void)context;
    if (!granted || (operation == ATTO_UDP_SEND && port == denied_port) ||
        (operation == ATTO_UDP_PEER && port == denied_source_port))
        return -EACCES;
    return 0;
}
static int read_next(atto_udp_loopback *s, uint8_t *buf, size_t cap, size_t *n, unsigned *p) {
    for (int i = 0; i < 1000; i++) {
        int rc=atto_udp_loopback_receive(s,buf,cap,n,p);
        if (rc != -EAGAIN) return rc;
        struct timespec tiny={.tv_sec=0,.tv_nsec=1000000};
        nanosleep(&tiny,NULL);
    }
    return -ETIMEDOUT;
}
struct blocked_send { atto_udp_loopback *socket; unsigned port; };
static void *blocked_sender(void *context) {
    struct blocked_send *task=(struct blocked_send *)context;
    const uint8_t bytes[]={8};
    size_t n=0;
    int rc=atto_udp_loopback_send(task->socket,task->port,bytes,sizeof bytes,&n);
    assert(rc==0 && n==sizeof bytes);
    return NULL;
}
int main(void) {
    atto_udp_loopback *a=NULL,*b=NULL;
    granted=0;
    assert(atto_udp_loopback_open(&a,0,authorize,NULL)==-EACCES && !a);
    granted=1;
    assert(atto_udp_loopback_open(&a,0,authorize,NULL)==0 && a);
    assert(atto_udp_loopback_open(&b,0,authorize,NULL)==0 && b);
    int ap=atto_udp_loopback_port(a),bp=atto_udp_loopback_port(b);
    assert(ap>0 && bp>0 && ap!=bp);
    uint8_t out[100]={0xAE};size_t sent=99,n=99;unsigned source=99;
    assert(atto_udp_loopback_send(a,0,out,1,&sent)==-EINVAL && sent==0);
    assert(atto_udp_loopback_send(a,(unsigned)bp,NULL,1,&sent)==-EINVAL);
    denied_port=(unsigned)bp;
    assert(atto_udp_loopback_send(a,(unsigned)bp,out,1,&sent)==-EACCES);
    denied_port=0;
    const uint8_t pkt[]={1,0,2,3};
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==0 && sent==4);
    assert(read_next(b,out,sizeof(out),&n,&source)==0 && n==4 && source==(unsigned)ap);
    assert(memcmp(out,pkt,4)==0);
    assert(atto_udp_loopback_send(a,(unsigned)bp,NULL,0,&sent)==0 && sent==0);
    assert(read_next(b,out,sizeof(out),&n,&source)==0 && n==0 && source==(unsigned)ap);
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==0);
    out[0]=0xAA;
    assert(read_next(b,out,2,&n,&source)==-EMSGSIZE && out[0]==0xAA && n==0 && source==0);
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==0);
    assert(read_next(b,out,sizeof(out),&n,&source)==0 && n==4);
    /* A revoked *sender* must never publish its already-received bytes. */
    denied_source_port=(unsigned)ap;
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==0);
    out[0]=0xD4;
    assert(read_next(b,out,sizeof(out),&n,&source)==-EACCES &&
           n==0 && source==0 && out[0]==0xD4);
    denied_source_port=0;
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==0);
    assert(read_next(b,out,sizeof(out),&n,&source)==0 &&
           n==4 && source==(unsigned)ap);

    granted=0;
    assert(atto_udp_loopback_receive(b,out,sizeof(out),&n,&source)==-EACCES);
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==-EACCES);
    granted=1;
    assert(atto_udp_loopback_close(a)==0);
    assert(atto_udp_loopback_close(a)==0);
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==-EBADF);
    assert(atto_udp_loopback_receive(a,out,sizeof(out),&n,&source)==-EBADF);
    assert(atto_udp_loopback_destroy(a)==0);
    assert(atto_udp_loopback_destroy(b)==0);

    /* Concurrency gate: destruction must NOT free a socket whose operation
     * remains inside a trusted host permission callback. Retry only after
     * that physical operation exits and host ingress is fenced. */
    atto_udp_loopback *busy=NULL;
    assert(atto_udp_loopback_open(&busy,0,authorize,NULL)==0);
    const int busy_port = atto_udp_loopback_port(busy);
    assert(busy_port>0);
    struct blocked_send task={busy,(unsigned)busy_port};
    atomic_store(&send_entered,0);
    atomic_store(&hold_send_authorize,1);
    pthread_t worker;
    assert(pthread_create(&worker,NULL,blocked_sender,&task)==0);
    for(int i=0;i<1000000 && !atomic_load(&send_entered);++i) sched_yield();
    assert(atomic_load(&send_entered));
    assert(atto_udp_loopback_destroy(busy)==-EAGAIN);
    atomic_store(&hold_send_authorize,0);
    assert(pthread_join(worker,NULL)==0);
    assert(atto_udp_loopback_destroy(busy)==0);

    puts("Native LLVM UDP loopback host ABI: passed (not guest import or actor lifecycle)");
    return 0;
}
