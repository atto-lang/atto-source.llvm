/* Test-only hand-authored stand-ins for the compiler's emitted C ABI. */
#include "oreslang/runtime/FreestandingSourceActor.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>

static OresActorI64Heap *captured_sink_heap;
static uint64_t captured_sink_domain, captured_sink_epoch;
static OresActorI64Heap *captured_heap;
static uint64_t captured_domain, captured_epoch;
static void source_init(OresActorI64Heap *h,uint64_t d,uint64_t e){
    ores_actor_i64_heap_field_init(h,d,e,0,40);
    ores_actor_i64_heap_field_init_mutable(h,d,e,1,0);
}
static int64_t source_handler(OresActorI64Heap *h,uint64_t d,uint64_t e,int64_t n){
    captured_heap=h;captured_domain=d;captured_epoch=e;
    if(n==996) (void)ores_actor_i64_heap_field_read(h,d,e-1,1);
    if(n==997) (void)ores_actor_i64_heap_field_read(h,d,e,999);
    if(n==998) ores_actor_i64_heap_field_write(h,d,e,0,222);
    if(n==999) (void)ores_actor_i64_heap_field_read(captured_sink_heap,captured_sink_domain,captured_sink_epoch,0);
    int64_t base=ores_actor_i64_heap_field_read(h,d,e,0);
    int64_t sum=ores_actor_i64_heap_field_read(h,d,e,1)+base+n;
    ores_actor_i64_heap_field_write(h,d,e,1,sum);
    return sum;
}
static void sink_init(OresActorI64Heap *h,uint64_t d,uint64_t e){
    ores_actor_i64_heap_field_init_mutable(h,d,e,0,5);
}
static int64_t sink_handler(OresActorI64Heap *h,uint64_t d,uint64_t e,int64_t n){
    captured_sink_heap=h; captured_sink_domain=d; captured_sink_epoch=e;
    int64_t sum=ores_actor_i64_heap_field_read(h,d,e,0)+n;
    ores_actor_i64_heap_field_write(h,d,e,0,sum);
    return sum;
}
static void bad_stale_epoch(void){
    (void)ores_actor_i64_heap_field_read(captured_heap,captured_domain,captured_epoch,1);
}
static void bad_immutable_write(void){
    ores_actor_i64_heap_field_write(captured_heap,captured_domain,captured_epoch,0,222);
}
static void *source_ref, *sink_ref;
static void bad_double_stop(void){oreslang_source_actor_stop(source_ref);}
static void bad_send_after_stop(void){oreslang_source_actor_send(source_ref,8);}
static void bad_forged_handle(void){oreslang_source_actor_send((void*)0x1234,5);}
static void bad_in_turn_immutable(void){
    void *a=oreslang_source_actor_spawn(source_init,source_handler,2);
    oreslang_source_actor_send(a,998);
    oreslang_source_actor_stop(a);
}
static void bad_in_turn_epoch(void){
    void *a=oreslang_source_actor_spawn(source_init,source_handler,2);
    oreslang_source_actor_send(a,996);
    oreslang_source_actor_stop(a);
}
static void bad_in_turn_index(void){
    void *a=oreslang_source_actor_spawn(source_init,source_handler,2);
    oreslang_source_actor_send(a,997);
    oreslang_source_actor_stop(a);
}
static void bad_in_turn_cross_actor(void){
    void *a=oreslang_source_actor_spawn(source_init,source_handler,2);
    oreslang_source_actor_send(a,999);
    oreslang_source_actor_stop(a);
}
static void bad_full_mailbox(void){
    void *a=oreslang_source_actor_spawn(source_init,source_handler,2);
    for(unsigned i=0;i<=ORES_KERNEL_MAILBOX;i++) oreslang_source_actor_send(a,1);
}
static void zero_init(OresActorI64Heap *heap, uint64_t domain, uint64_t epoch) {
    (void)heap; (void)domain; (void)epoch;
}
static int64_t zero_message(OresActorI64Heap *heap, uint64_t domain,
                            uint64_t epoch, int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    return payload + 1;
}
static void incomplete_init(OresActorI64Heap *h, uint64_t d, uint64_t e) {
    ores_actor_i64_heap_field_init(h,d,e,0,33);
}
static void duplicate_init(OresActorI64Heap *h, uint64_t d, uint64_t e) {
    ores_actor_i64_heap_field_init_mutable(h,d,e,0,1);
    ores_actor_i64_heap_field_init_mutable(h,d,e,0,2);
}
static void read_uninitialized(OresActorI64Heap *h, uint64_t d, uint64_t e) {
    (void)ores_actor_i64_heap_field_read(h,d,e,0);
}
static void write_during_init(OresActorI64Heap *h, uint64_t d, uint64_t e) {
    ores_actor_i64_heap_field_init_mutable(h,d,e,0,1);
    ores_actor_i64_heap_field_write(h,d,e,0,2);
}
static void init_outside_field_range(OresActorI64Heap *h, uint64_t d, uint64_t e) {
    ores_actor_i64_heap_field_init(h,d,e,1,5);
}
static void illegal_actor_send(OresActorI64Heap *h,uint64_t d,uint64_t e,int64_t n) {
    (void)h;(void)d;(void)e;(void)n;
    oreslang_source_actor_send(source_ref,4);
}
static int64_t nested_sender(OresActorI64Heap *h,uint64_t d,uint64_t e,int64_t n) {
    illegal_actor_send(h,d,e,n);
    return 0;
}
static void bad_incomplete_initializer(void) {
    (void)oreslang_source_actor_spawn(incomplete_init,zero_message,2);
}
static void bad_duplicate_initializer(void) {
    (void)oreslang_source_actor_spawn(duplicate_init,zero_message,1);
}
static void bad_read_uninitialized(void) {
    (void)oreslang_source_actor_spawn(read_uninitialized,zero_message,1);
}
static void bad_write_during_init(void) {
    (void)oreslang_source_actor_spawn(write_during_init,zero_message,1);
}
static void bad_init_outside_field_range(void) {
    (void)oreslang_source_actor_spawn(init_outside_field_range,zero_message,1);
}
static void bad_send_inside_handler(void) {
    void *a=oreslang_source_actor_spawn(zero_init,nested_sender,0);
    oreslang_source_actor_send(a,10);
    oreslang_source_actor_stop(a);
}
static void bad_zero_field_index(void) {
    (void)oreslang_source_actor_spawn(init_outside_field_range,zero_message,0);
}
static void bad_field_capacity(void) {
    (void)oreslang_source_actor_spawn(zero_init,zero_message,ORES_KERNEL_FIELDS+1);
}
static void bad_null_callback(void) {
    (void)oreslang_source_actor_spawn(zero_init,0,0);
}
static void bad_stats_null(void) {
    ores_freestanding_source_stats(0);
}
static void expect_trap(void(*fn)(void)){
    pid_t child=fork();assert(child>=0);
    if(child==0){fn();_exit(99);}
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status)==SIGILL||WTERMSIG(status)==SIGTRAP);
}
int main(void){
    source_ref=oreslang_source_actor_spawn(source_init,source_handler,2);
    sink_ref=oreslang_source_actor_spawn(sink_init,sink_handler,1);
    oreslang_source_actor_send(source_ref,1);
    oreslang_source_actor_send(sink_ref,11);
    oreslang_source_actor_send(source_ref,2);
    oreslang_source_actor_send(sink_ref,3);
    oreslang_source_actor_stop(source_ref);
    oreslang_source_actor_stop(sink_ref);
    OresFreestandingActorStats stats={0};
    ores_freestanding_source_stats(&stats);
    assert(stats.spawned==2&&stats.retired==2&&stats.delivered==4);
    assert(stats.last_result[0]==83 && stats.last_result[1]==19);
    void *empty=oreslang_source_actor_spawn(zero_init,zero_message,0);
    oreslang_source_actor_send(empty,12);
    oreslang_source_actor_stop(empty);
    ores_freestanding_source_stats(&stats);
    assert(stats.spawned==3 && stats.retired==3 && stats.delivered==5);
    assert(stats.last_result[2]==13);
    void *negative_actor=oreslang_source_actor_spawn(zero_init,zero_message,0);
    oreslang_source_actor_send(negative_actor,-9);
    oreslang_source_actor_stop(negative_actor);
    ores_freestanding_source_stats(&stats);
    assert(stats.spawned==4 && stats.retired==4 && stats.delivered==6);
    assert(stats.last_result[3]==-8);
    expect_trap(bad_stale_epoch);
    expect_trap(bad_immutable_write);
    expect_trap(bad_double_stop);
    expect_trap(bad_send_after_stop);
    expect_trap(bad_forged_handle);
    expect_trap(bad_in_turn_immutable);
    expect_trap(bad_in_turn_epoch);
    expect_trap(bad_in_turn_index);
    expect_trap(bad_in_turn_cross_actor);
    expect_trap(bad_full_mailbox);
    expect_trap(bad_incomplete_initializer);
    expect_trap(bad_duplicate_initializer);
    expect_trap(bad_read_uninitialized);
    expect_trap(bad_write_during_init);
    expect_trap(bad_init_outside_field_range);
    expect_trap(bad_send_inside_handler);
    expect_trap(bad_zero_field_index);
    expect_trap(bad_field_capacity);
    expect_trap(bad_null_callback);
    expect_trap(bad_stats_null);
    puts("FREESTANDING_COMPILER_ACTOR_ABI_HOST_PASS 4 actors 20 negative cases");
    return 0;
}
