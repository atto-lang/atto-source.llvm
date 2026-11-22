#include "atto/host_udp_loopback.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
static int granted = 1;
static unsigned denied_port = 0;
static int authorize(void *context, unsigned port, int operation) {
    (void)context;
    if (!granted || (operation == ATTO_UDP_SEND && port == denied_port)) return -EACCES;
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
    granted=0;
    assert(atto_udp_loopback_receive(b,out,sizeof(out),&n,&source)==-EACCES);
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==-EACCES);
    granted=1;
    assert(atto_udp_loopback_close(a)==0);
    assert(atto_udp_loopback_close(a)==0);
    assert(atto_udp_loopback_send(a,(unsigned)bp,pkt,4,&sent)==-EBADF);
    assert(atto_udp_loopback_receive(a,out,sizeof(out),&n,&source)==-EBADF);
    atto_udp_loopback_destroy(a);
    atto_udp_loopback_destroy(b);
    puts("Native LLVM UDP loopback host ABI: passed (not guest import or actor lifecycle)");
    return 0;
}
