#include "oreslang/gpu/G1CudaProvider.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <atomic>
#include <memory>
#include <random>
#include <thread>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>
#include <string>
using namespace oreslang::gpu;
using namespace oreslang::gpu::g2;
static int count=0;
static void check(bool ok){if(!ok)throw std::runtime_error("GPU-G2 mock lifecycle assertion failed");++count;}
template<typename E=std::exception,typename F>static void rejects(F f){bool threw=false;try{f();}catch(const E&){threw=true;}check(threw);}
struct Fake final:Driver {
    std::string device="driver-verified-mock-gpu";
    Limits limits{{65535,65535,65535},{1024,1024,64},1024,49152};
    std::map<Handle,std::vector<float>> buffers;
    Handle serial=1;
    Handle live_module=0,live_event=0;
    bool ready=false,fail_upload=false,fail_load=false,fail_record=false,fail_free=false,fail_event_query=false,stream_idle=false;
    bool fail_kernel=false,fail_download=false,fail_destroy_event=false,fail_unload=false;
    int fail_allocate_at=0, allocation_count=0;
    std::size_t launches=0,frees=0,uploads=0,downloads=0,unloads=0;
    std::vector<std::string> order;
    const std::string& device_identity()const override{return device;}
    const Limits& device_limits()const override{return limits;}
    Handle load_ptx(std::string_view s)override{if(fail_load)throw std::runtime_error("load failed");check(s.find(".entry saxpy")!=s.npos);return live_module=++serial;}
    Handle kernel(Handle h,const char* entry)override{check(h==live_module&&std::string(entry)=="saxpy");return ++serial;}
    Handle allocate(std::size_t n)override{check(n>0);if(++allocation_count==fail_allocate_at)throw std::runtime_error("injected allocation failure");const auto h=++serial;buffers[h].resize(n/sizeof(float));return h;}
    void upload(Handle d,const float* src,std::size_t n)override{order.push_back("upload");if(fail_upload)throw std::runtime_error("enqueue failure");check(buffers.count(d)==1);std::memcpy(buffers.at(d).data(),src,n);++uploads;}
    void launch(Handle fn,std::uint32_t grid,std::uint32_t block,float a,Handle x,Handle y,Handle out,std::uint64_t n)override{
        if(fail_kernel)throw std::runtime_error("injected launch failure");
        check(fn!=0&&grid!=0&&block!=0&&static_cast<std::uint64_t>(grid)*block>=n);++launches;order.push_back("kernel");
        for(std::size_t i=0;i<n;++i) buffers.at(out)[i]=a*buffers.at(x)[i]+buffers.at(y)[i];
    }
    void download(float* dst,Handle src,std::size_t n)override{if(fail_download)throw std::runtime_error("injected download failure");
        check(buffers.count(src)==1);order.push_back("download");std::memcpy(dst,buffers.at(src).data(),n);++downloads;}
    Handle record_event()override{if(fail_record)throw std::runtime_error("no event");order.push_back("event");return live_event=++serial;}
    EventResult event_result(Handle e)override{check(e==live_event);if(fail_event_query)throw std::runtime_error("query unavailable");return ready?EventResult::Complete:EventResult::Pending;}
    bool stream_quiescent()override{return stream_idle;}
    void destroy_event(Handle e)override{if(fail_destroy_event)throw std::runtime_error("injected event-release failure");check(e==live_event);live_event=0;}
    void free_buffer(Handle p)override{if(fail_free)throw std::runtime_error("release failure");check(buffers.erase(p)==1);++frees;}
    void unload(Handle m)override{if(fail_unload)throw std::runtime_error("injected module-release failure");check(m==live_module);live_module=0;++unloads;}
};
static std::string ptx=".version 7.0\n.visible .entry saxpy() { ret; }";
int main(){
    {Fake d;SaxpyOperation op(d,"tenant-a",100,4096);check(op.state()==State::Ready);
     rejects<std::logic_error>([&]{(void)op.output();});
     rejects<std::invalid_argument>([&]{op.submit("",2.0f,{1},{2});});
     rejects<std::invalid_argument>([&]{op.submit(ptx,2.0f,{1},{2,3});});
     rejects<std::invalid_argument>([&]{op.submit(ptx,2.0f,{1},{2},0);});
     check(d.buffers.empty());
     op.submit(ptx,2.0f,{1,2,3},{4,5,6});
     check(op.state()==State::Submitted&&d.buffers.size()==3&&d.launches==1);
     check(d.order==std::vector<std::string>({"upload","upload","kernel","download","event"}));
     rejects<std::logic_error>([&]{(void)op.output();});
     check(!op.poll()&&op.has_live_device_handles());
     rejects<std::logic_error>([&]{op.close();});
     d.ready=true;check(op.poll());check(op.state()==State::Completed);
     check(op.output()==std::vector<float>({6,9,12}));check(d.buffers.empty()&&d.frees==3&&d.unloads==1);
     rejects<std::logic_error>([&]{op.submit(ptx,1,{1},{1});});op.close();check(op.state()==State::Closed);
    }
    {Fake d;SaxpyOperation op(d,"tenant",400,4096);op.submit(ptx,1,{1,2},{3,4});op.request_cancel();
     check(op.state()==State::CancelRequested);rejects<std::logic_error>([&]{(void)op.output();});
     check(!op.poll()&&d.frees==0);d.ready=true;check(!op.poll()&&d.frees==0);
     d.stream_idle=true;check(op.poll()&&op.state()==State::Cancelled);
     check(d.frees==3);op.close();}
    {Fake d;SaxpyOperation op(d,"tenant",500,4096);op.submit(ptx,1,{},{});check(op.state()==State::Completed&&d.launches==0&&d.frees==0);op.close();}
    {Fake d;d.fail_load=true;SaxpyOperation op(d,"tenant",600,4096);rejects<std::runtime_error>([&]{op.submit(ptx,1,{1},{2});});check(op.state()==State::Failed&&!op.has_live_device_handles());op.close();}
    {Fake d;d.fail_upload=true;SaxpyOperation op(d,"tenant",700,4096);rejects<std::runtime_error>([&]{op.submit(ptx,1,{1},{2});});
     check(op.state()==State::Quarantined&&d.frees==0);check(!op.poll());d.stream_idle=true;check(op.poll());check(op.state()==State::Failed&&d.frees==3);op.close();}
    {Fake d;d.fail_record=true;SaxpyOperation op(d,"tenant",800,4096);rejects<std::runtime_error>([&]{op.submit(ptx,1,{1},{2});});
     check(op.state()==State::Quarantined&&d.launches==1&&d.frees==0);check(!op.poll());d.stream_idle=true;check(op.poll());check(d.frees==3);op.close();}
    {Fake d;SaxpyOperation op(d,"tenant",900,4096);op.submit(ptx,1,{1},{2});d.ready=true;d.fail_free=true;
     rejects<std::runtime_error>([&]{(void)op.poll();});check(op.state()==State::ReleaseFailed&&op.has_live_device_handles());
     d.fail_free=false;check(op.poll());check(op.output()==std::vector<float>({3}));op.close();}
    {Fake d;SaxpyOperation op(d,"tenant",1000,4096);op.submit(ptx,1,{1},{2});
     d.fail_event_query=true;check(!op.poll()&&op.state()==State::Quarantined&&d.frees==0);
     d.fail_event_query=false;d.ready=true;check(!op.poll()&&d.frees==0);
     d.stream_idle=true;check(op.poll()&&op.state()==State::Failed&&d.frees==3);op.close();}
    {Fake d;d.limits.max_block=Dim3(32,1,1);SaxpyOperation op(d,"tenant",1100,4096);
     rejects<std::invalid_argument>([&]{op.submit(ptx,1,{1},{2},256);});
     check(d.buffers.empty()&&d.launches==0);op.submit(ptx,1,{1},{2},32);
     d.ready=true;check(op.poll()&&op.output()==std::vector<float>({3}));op.close();}
    {Fake d;SaxpyOperation op(d,"tenant",1200,4);
     rejects<std::invalid_argument>([&]{op.submit(ptx,1,{1,2},{2,3});});
     check(d.buffers.empty());rejects<std::logic_error>([&]{(void)op.poll();});
     rejects<std::logic_error>([&]{op.request_cancel();});}
    {Fake d;SaxpyOperation first(d,"first-tenant",1300,4096);
     SaxpyOperation second(d,"second-tenant",1400,4096);
     first.submit(ptx,1,{1},{2});
     rejects<std::logic_error>([&]{second.submit(ptx,2,{3},{4});});
     check(second.state()==State::Ready && d.launches==1 && d.buffers.size()==3);
     d.ready=true;check(first.poll() && first.state()==State::Completed);first.close();
     second.submit(ptx,2,{3},{4});check(d.launches==2);
     check(second.poll() && second.output()==std::vector<float>({10}));second.close();}
    {Fake d;d.fail_upload=true;
     SaxpyOperation unsafe(d,"tenant-a",1500,4096);
     SaxpyOperation pending(d,"tenant-b",1600,4096);
     rejects<std::runtime_error>([&]{unsafe.submit(ptx,1,{1},{2});});
     check(unsafe.state()==State::Quarantined);
     d.fail_upload=false;
     rejects<std::logic_error>([&]{pending.submit(ptx,1,{5},{6});});
     check(!unsafe.poll() && d.frees==0);
     d.stream_idle=true;check(unsafe.poll() && unsafe.state()==State::Failed);unsafe.close();
     pending.submit(ptx,1,{5},{6});d.ready=true;check(pending.poll());
     check(pending.output()==std::vector<float>({11}));pending.close();}
    {Fake d;SaxpyOperation first(d,"tenant-a",1700,4096);
     SaxpyOperation second(d,"tenant-b",1800,4096);
     first.submit(ptx,1,{1},{2});d.ready=true;d.fail_free=true;
     rejects<std::runtime_error>([&]{(void)first.poll();});
     check(first.state()==State::ReleaseFailed);
     rejects<std::logic_error>([&]{second.submit(ptx,1,{1},{2});});
     d.fail_free=false;check(first.poll());first.close();
     second.submit(ptx,1,{1},{2});check(second.poll());second.close();}
    {Fake d;SaxpyOperation first(d,"tenant-a",1900,4096);
     SaxpyOperation second(d,"tenant-b",2000,4096);
     first.submit(ptx,1,{1},{2});first.request_cancel();
     rejects<std::logic_error>([&]{second.submit(ptx,1,{1},{2});});
     d.ready=true;d.stream_idle=true;check(first.poll());first.close();
     second.submit(ptx,1,{1},{2});d.ready=true;check(second.poll());second.close();}
    rejects<std::invalid_argument>([]{Fake d;SaxpyOperation op(d,"tenant",std::numeric_limits<std::uint64_t>::max(),4096);});
    rejects<std::invalid_argument>([]{Fake d;SaxpyOperation op(d,"",100,4096);});
    rejects<std::invalid_argument>([]{Fake d;SaxpyOperation op(d,"tenant",0,4096);});
    // Fault injection before any physical enqueue must reclaim reservations.
    for (int nth=1;nth<=3;++nth) {
        Fake d;d.fail_allocate_at=nth;SaxpyOperation op(d,"allocation-fail",2500+nth*3,4096);
        rejects<std::runtime_error>([&]{op.submit(ptx,1,{1},{2});});
        check(op.state()==State::Failed&&d.buffers.empty()&&d.live_module==0);
        check(!op.has_live_device_handles());op.close();
    }
    // Faults after a possible enqueue may never release until stream proof.
    for (int mode=0;mode<2;++mode) {
        Fake d;d.fail_kernel=mode==0;d.fail_download=mode==1;
        SaxpyOperation op(d,"submit-fault",2600+mode*3,4096);
        rejects<std::runtime_error>([&]{op.submit(ptx,1,{1},{2});});
        check(op.state()==State::Quarantined&&d.buffers.size()==3&&d.frees==0);
        check(!op.poll()&&d.frees==0);d.stream_idle=true;
        check(op.poll()&&op.state()==State::Failed&&d.buffers.empty());op.close();
    }
    // Cleanup failures must preserve the stream lease and allow safe retry.
    for (int mode=0;mode<2;++mode) {
        Fake d;SaxpyOperation first(d,"release-fault",2700+mode*3,4096);
        SaxpyOperation second(d,"contender",2800+mode*3,4096);
        first.submit(ptx,1,{1},{2});d.ready=true;
        d.fail_destroy_event=mode==0;d.fail_unload=mode==1;
        rejects<std::runtime_error>([&]{(void)first.poll();});
        check(first.state()==State::ReleaseFailed);
        rejects<std::logic_error>([&]{second.submit(ptx,1,{1},{2});});
        d.fail_destroy_event=false;d.fail_unload=false;
        check(first.poll()&&first.output()==std::vector<float>({3}));first.close();
        second.submit(ptx,2,{3},{4});check(second.poll());second.close();
        check(d.buffers.empty()&&d.live_module==0);
    }
    // Four separate operations may race to acquire one driver-owned stream.
    // Simultaneous gate release exercises the real mutex-protected claim.
    for (int trial=0;trial<128;++trial) {
        Fake d;std::array<std::unique_ptr<SaxpyOperation>,4> ops;
        for (int j=0;j<4;++j)
            ops[j]=std::make_unique<SaxpyOperation>(d,"tenant"+std::to_string(j),3000+trial*12+j*3,4096);
        std::array<bool,4> accepted{};
        std::array<std::exception_ptr,4> errors{};
        std::atomic<int> ready{0};std::atomic<bool> go{false};
        std::array<std::thread,4> workers;
        for (int j=0;j<4;++j)workers[j]=std::thread([&,j]{
            ready.fetch_add(1,std::memory_order_release);
            while(!go.load(std::memory_order_acquire))std::this_thread::yield();
            try{ops[j]->submit(ptx,2.0f,{float(j+1)},{10.0f});accepted[j]=true;}
            catch(const std::logic_error&){}catch(...){errors[j]=std::current_exception();}
        });
        while(ready.load(std::memory_order_acquire)!=4)std::this_thread::yield();
        go.store(true,std::memory_order_release);
        for(auto& worker:workers)worker.join();
        for(const auto& err:errors)if(err)std::rethrow_exception(err);
        int winners=0;for(bool b:accepted)winners+=b?1:0;
        check(winners==1&&d.launches==1&&d.buffers.size()==3);
        d.ready=true;
        for(int j=0;j<4;++j){
            if(accepted[j]){
                check(ops[j]->poll()&&ops[j]->output()==std::vector<float>({float(2*(j+1)+10)}));
                ops[j]->close();
            }else check(ops[j]->state()==State::Ready&&!ops[j]->has_live_device_handles());
        }
        check(d.buffers.empty()&&d.live_module==0);
    }
    // Deterministic property tests: mixed workgroup sizes, padded lanes,
    // n=0, full float vectors and CPU numerical oracle across 300 shapes.
    std::mt19937_64 rng(0xa7700026ULL);
    for(int trial=0;trial<300;++trial){
        Fake d;const auto n=static_cast<std::size_t>(rng()%2050);
        const auto group=static_cast<std::uint32_t>(1u<<(rng()%9));
        const auto scale=static_cast<float>(int(rng()%9)-4);
        std::vector<float> x(n),y(n);
        for(std::size_t i=0;i<n;++i){x[i]=float(int(rng()%511)-255);y[i]=float(int(rng()%511)-255);}
        SaxpyOperation op(d,"fuzz",50000+trial*3,1u<<20);
        op.submit(ptx,scale,x,y,group);
        if(n==0)check(d.launches==0&&op.state()==State::Completed);
        else{check(!op.poll());d.ready=true;check(op.poll());}
        const auto& out=op.output();check(out.size()==n);
        for(std::size_t i=0;i<n;++i)check(out[i]==scale*x[i]+y[i]);
        check(d.buffers.empty()&&d.live_module==0);op.close();
    }
    std::cout<<"PASS "<<count<<" GPU-G2 host-only driver lifecycle assertions\n";
}
