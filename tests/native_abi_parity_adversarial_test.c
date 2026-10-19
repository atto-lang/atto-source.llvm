#define _GNU_SOURCE
#include "oreslang/runtime/NativeEpollReactorABI.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static void require(int ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s errno=%d\n", why, errno); abort(); }
}
static int wait_pop(OresNativeNio* manager, OresNativeNioCompletion* out) {
    struct timespec delay={0,1000000};
    for (int n=0;n<3000;n++) {
        if (ores_native_nio_try_pop(manager,out)) return 1;
        nanosleep(&delay,NULL);
    }
    return 0;
}
static void cleanpipe(int p[2]) { close(p[0]); close(p[1]); }
int main(void) {
    require(!ores_native_nio_open(0,0,1),"reject invalid count");
    require(!ores_native_nio_open(5,0,1),"reject excessive count");
    require(!ores_native_nio_open(1,1,2),"reject invalid RT bound");
    OresNativeNio* a=ores_native_nio_open(1,0,1);
    OresNativeNio* b=ores_native_nio_open(1,0,1);
    require(a&&b,"create two managers");
    OresNativeNioCompletion stale={0};
    stale.watch.ticket=UINT64_MAX;
    stale.watch.reactor=UINT8_MAX;
    stale.fd=12345;
    stale.events=UINT32_MAX;
    stale.outcome=ORES_NATIVE_NIO_READY;
    require(!ores_native_nio_try_pop(a,&stale),"empty completion fails");
    require(stale.watch.ticket==0 && stale.watch.reactor==0 && stale.fd==0 &&
            stale.events==0 && stale.outcome==0,"failed completion clears stale output");
    require(!ores_native_nio_started_reactors(a),"cold manager");
    int p[2]; require(pipe2(p,O_NONBLOCK|O_CLOEXEC)==0,"create pipe");
    OresNativeNioWatch token={UINT64_MAX,UINT8_MAX};
    require(!ores_native_nio_try_watch(a,-1,EPOLLIN,&token) && token.ticket==0 && token.reactor==0,"bad fd clears stale output");
    token=(OresNativeNioWatch){UINT64_MAX,UINT8_MAX};
    require(!ores_native_nio_try_watch(a,p[0],0,&token) && !token.ticket,"bad events clear stale output");
    int anon=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    require(anon>=0,"create raw anon eventfd");
    token=(OresNativeNioWatch){UINT64_MAX,UINT8_MAX};
    require(!ores_native_nio_try_watch(a,anon,EPOLLIN,&token) && !token.ticket,"anonymous descriptor denied");
    close(anon);
    require(!ores_native_nio_started_reactors(a),"invalid requests keep cold");
    require(ores_native_nio_try_watch(a,p[0],EPOLLIN,&token),"valid raw pipe");
    require(token.ticket && ores_native_nio_started_reactors(a)==1,"admitted and started");
    int alias=dup(p[0]);require(alias>=0,"pipe dup");
    OresNativeNioWatch blocked={UINT64_MAX,UINT8_MAX};
    require(!ores_native_nio_try_watch(b,alias,EPOLLIN,&blocked),"global claim denies alias on separate manager");
    require(!ores_native_nio_try_cancel(b,token),"foreign manager cannot cancel public ticket");
    require(write(p[1],"x",1)==1,"send readiness");
    OresNativeNioCompletion event={0};
    require(wait_pop(a,&event),"actual readiness terminal");
    require(event.watch.ticket==token.ticket && event.fd==p[0] && event.outcome==ORES_NATIVE_NIO_READY,"terminal content provenance");
    require(!ores_native_nio_try_pop(a,&event),"no duplicate terminal");
    // Retired global raw-stream claim can now be admitted by another manager.
    OresNativeNioWatch second={0};
    require(ores_native_nio_try_watch(b,alias,EPOLLIN,&second),"released global claim");
    require(second.ticket && second.ticket!=token.ticket,"process unique public tickets");
    require(!ores_native_nio_try_cancel(a,second),"stale/foreign cancel fails closed");
    require(ores_native_nio_try_cancel(b,second),"owner cancellation succeeds");
    OresNativeNioCompletion cancelled={0};
    require(wait_pop(b,&cancelled),"cancel terminal emitted");
    require(cancelled.outcome==ORES_NATIVE_NIO_CANCELLED && cancelled.watch.ticket==second.ticket,"cancel exactly once");
    require(!ores_native_nio_try_cancel(b,second),"stale cancel rejected");
    // Completion fd is an eventfd notification, not a continuation.
    require(ores_native_nio_completion_fd(a,1)==-1,"invalid reactor index");
    ores_native_nio_drain_completion_signal(a,0);
    ores_native_nio_drain_completion_signal(b,0);
    close(alias);cleanpipe(p);
    ores_native_nio_close(a);ores_native_nio_close(b);
    OresNativeNio* realtime=ores_native_nio_open(1,1,1);
    require(realtime && ores_native_nio_started_reactors(realtime)==1,"RT eager setup");
    ores_native_nio_close(realtime);
    printf("NATIVE_C_ABI_ADVERSARIAL_PASS cases=20\n");
    return 0;
}
