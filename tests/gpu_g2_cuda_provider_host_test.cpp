#include "oreslang/gpu/G1CudaProvider.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
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
    std::size_t launches=0,frees=0,uploads=0,downloads=0,unloads=0;
    std::vector<std::string> order;
    const std::string& device_identity()const override{return device;}
    const Limits& device_limits()const override{return limits;}
    Handle load_ptx(std::string_view s)override{if(fail_load)throw std::runtime_error("load failed");check(s.find(".entry saxpy")!=s.npos);return live_module=++serial;}
    Handle kernel(Handle h,const char* entry)override{check(h==live_module&&std::string(entry)=="saxpy");return ++serial;}
    Handle allocate(std::size_t n)override{check(n>0);const auto h=++serial;buffers[h].resize(n/sizeof(float));return h;}
    void upload(Handle d,const float* src,std::size_t n)override{order.push_back("upload");if(fail_upload)throw std::runtime_error("enqueue failure");check(buffers.count(d)==1);std::memcpy(buffers.at(d).data(),src,n);++uploads;}
    void launch(Handle fn,std::uint32_t grid,std::uint32_t block,float a,Handle x,Handle y,Handle out,std::uint64_t n)override{
        check(fn!=0&&grid!=0&&block!=0&&static_cast<std::uint64_t>(grid)*block>=n);++launches;order.push_back("kernel");
        for(std::size_t i=0;i<n;++i) buffers.at(out)[i]=a*buffers.at(x)[i]+buffers.at(y)[i];
    }
    void download(float* dst,Handle src,std::size_t n)override{check(buffers.count(src)==1);order.push_back("download");std::memcpy(dst,buffers.at(src).data(),n);++downloads;}
    Handle record_event()override{if(fail_record)throw std::runtime_error("no event");order.push_back("event");return live_event=++serial;}
    EventResult event_result(Handle e)override{check(e==live_event);if(fail_event_query)throw std::runtime_error("query unavailable");return ready?EventResult::Complete:EventResult::Pending;}
    bool stream_quiescent()override{return stream_idle;}
    void destroy_event(Handle e)override{check(e==live_event);live_event=0;}
    void free_buffer(Handle p)override{if(fail_free)throw std::runtime_error("release failure");check(buffers.erase(p)==1);++frees;}
    void unload(Handle m)override{check(m==live_module);live_module=0;++unloads;}
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
    rejects<std::invalid_argument>([]{Fake d;SaxpyOperation op(d,"tenant",std::numeric_limits<std::uint64_t>::max(),4096);});
    rejects<std::invalid_argument>([]{Fake d;SaxpyOperation op(d,"",100,4096);});
    rejects<std::invalid_argument>([]{Fake d;SaxpyOperation op(d,"tenant",0,4096);});
    std::cout<<"PASS "<<count<<" GPU-G2 host-only driver lifecycle assertions\n";
}
