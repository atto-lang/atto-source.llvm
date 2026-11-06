#include "oreslang/runtime/CudaCubinAot.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

using namespace oreslang::cuda_aot;

struct Mock final : Driver {
    unsigned sm=80;
    int loaded=0, uninstalled=0, launched=0, created=0, recorded=0, destroyed=0;
    int query=600;
    bool fail_symbol=false, fail_unload=false, fail_load=false;
    unsigned current_sm() override {return sm;}
    int load_cubin(Handle* out,const unsigned char*) override {
        ++loaded;*out=reinterpret_cast<Handle>(1);return fail_load?1:0;
    }
    int get_function(Handle* out,Handle,const char* name) override {
        if(fail_symbol || std::string(name)!="saxpy")return 1;
        *out=reinterpret_cast<Handle>(2);return 0;
    }
    int unload(Handle) override {if(fail_unload)return 1;++uninstalled;return 0;}
    int event_create(Handle* out) override {++created;*out=reinterpret_cast<Handle>(3);return 0;}
    int launch(Handle,const Dimensions&,void**,Handle) override {++launched;return 0;}
    int event_record(Handle,Handle) override {++recorded;return 0;}
    int event_query(Handle) override {return query;}
    int event_destroy(Handle) override {++destroyed;return 0;}
};

template<class F> static void rejected(F&& f) {
    bool hit=false;
    try{f();}catch(const std::exception&){hit=true;}
    assert(hit);
}

static constexpr Limits limits{65536,1024,49152,65535,65535,65535,1024,1024,64};
static constexpr Dimensions dims{4,1,1,256,1,1,0};
static std::array<unsigned char,64> elf() {
    std::array<unsigned char,64> b{};
    b[0]=0x7f;b[1]='E';b[2]='L';b[3]='F';
    b[4]=2;b[5]=1;b[6]=1;b[16]=2;b[18]=190;
    b[20]=1;b[52]=64;
    // Finalized NVIDIA sm_80 e_flags = 0x06005004.
    b[48]=4;b[49]=80;b[51]=6;
    return b;
}
int main() {
    auto bytes=elf();
    Cubin good{bytes.data(),bytes.size(),80};
    assert(cubin_sm(good)==80);
    {
        Mock api;
        Kernel kernel(api,good,"saxpy",limits);
        void* args[1]={bytes.data()};
        rejected([&]{kernel.submit({0,1,1,256,1,1,0},args,reinterpret_cast<void*>(4));});
        rejected([&]{kernel.submit({1,1,1,1025,1,1,0},args,reinterpret_cast<void*>(4));});
        rejected([&]{kernel.submit({std::numeric_limits<unsigned>::max(),1,1,1,1,1,0},args,reinterpret_cast<void*>(4));});
        rejected([&]{kernel.submit({1,1,1,256,1,1,49153},args,reinterpret_cast<void*>(4));});
        assert(api.launched==0 && api.created==0);
        kernel.submit(dims,args,reinterpret_cast<void*>(4));
        assert(api.launched==1 && api.recorded==1 && api.created==1);
        rejected([&]{kernel.close();});
        rejected([&]{kernel.submit(dims,args,reinterpret_cast<void*>(4));});
        assert(kernel.poll()==Poll::Pending);
        kernel.request_cancel();
        assert(kernel.poll()==Poll::Pending);
        api.query=700; // driver error cannot be mistaken for quiescence
        assert(kernel.poll()==Poll::Unknown);
        rejected([&]{kernel.close();});
        api.query=0;
        assert(kernel.poll()==Poll::Cancelled);
        assert(api.destroyed==1);
        kernel.close();
        assert(api.uninstalled==1);
    }
    {
        Mock api;
        api.sm=86;
        rejected([&]{Kernel kernel(api,good,"saxpy",limits);});
        assert(api.loaded==0);
    }
    {
        Mock api;api.fail_symbol=true;
        rejected([&]{Kernel kernel(api,good,"saxpy",limits);});
        assert(api.uninstalled==1);
    }
    {
        Mock api;api.fail_load=true;
        rejected([&]{Kernel kernel(api,good,"saxpy",limits);});
        assert(api.loaded==1 && api.uninstalled==1);
    }
    for(unsigned i=0;i<64;++i) {
        auto broken=bytes;
        broken[i]^=0xff;
        // Only mandatory header bytes must be denied; most bytes are opaque
        // payload interpreted by the actual CUDA driver.
        if(i==0||i==1||i==2||i==3||i==4||i==5||i==6||i==16||i==18||i==20||i==49||i==52) {
            rejected([&]{(void)cubin_sm({broken.data(),broken.size(),80});});
        }
    }
    rejected([&]{(void)cubin_sm({reinterpret_cast<const unsigned char*>(".version 8.0"),12,80});});
    rejected([&]{(void)cubin_sm({bytes.data(),bytes.size(),86});});
    auto unsupported=bytes;unsupported[49]=100;
    rejected([&]{(void)cubin_sm({unsupported.data(),unsupported.size(),100});});
    rejected([&]{(void)cubin_sm({nullptr,64,80});});
    assert(checked_launch(dims,limits)==1024);
    rejected([&]{(void)checked_launch({65535,65535,65535,1024,1024,64,0},limits);});
    return 0;
}
