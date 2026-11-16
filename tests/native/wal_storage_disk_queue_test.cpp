#define ORES_WAL_HOST_FAULT_TEST 1
#include "oreslang/runtime/WalStorageDiskQueue.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using Owner=oreslang::runtime::WalStorageFdOwner;
using Queue=oreslang::runtime::WalStorageDiskQueue;
using Scope=Owner::Scope;
int checks=0;
void ok(bool result, const char* reason) {
    ++checks;
    if (!result) throw std::runtime_error(reason);
}
template<class Error,class Fn>
void rejects(Fn&& fn, const char* reason) {
    ++checks;
    try { fn(); }
    catch (const Error&) { return; }
    throw std::runtime_error(reason);
}
void write_u32(std::vector<std::uint8_t>& b,std::size_t off,std::uint32_t n) {
    for(std::size_t i=0;i<4;++i) b[off+i]=static_cast<std::uint8_t>((n>>(i*8))&255);
}
std::vector<std::uint8_t> frame(std::uint32_t tenant,std::uint32_t generation,
                                std::uint32_t sequence,std::uint32_t operation) {
    std::vector<std::uint8_t> b(32,0);
    b[0]=79;b[1]=68;b[2]=66;b[3]=49;b[4]=1;b[5]=1;
    write_u32(b,6,tenant);write_u32(b,10,generation);
    write_u32(b,14,sequence);write_u32(b,18,operation);
    b[22]=1;b[24]=1;b[26]='k';b[27]='v';
    std::uint32_t a=1,c=0;
    for(std::size_t i=0;i<28;++i){a=(a+b[i])%65521;c=(c+a)%65521;}
    b[28]=static_cast<std::uint8_t>(a);b[29]=static_cast<std::uint8_t>(a>>8);
    b[30]=static_cast<std::uint8_t>(c);b[31]=static_cast<std::uint8_t>(c>>8);
    return b;
}
class TempFile {
public:
    TempFile() {
        char tmp[]="/tmp/ores-wal-disk-XXXXXX";
        fd_=::mkstemp(tmp);
        if(fd_<0)throw std::runtime_error("mkstemp failed");
        path_=tmp;
    }
    TempFile(const TempFile&)=delete;
    TempFile& operator=(const TempFile&)=delete;
    ~TempFile(){ if(fd_>=0)(void)::close(fd_);(void)::unlink(path_.c_str()); }
    int transfer() {int fd=fd_;fd_=-1;return fd;}
    const std::string& path()const{return path_;}
private:
    int fd_=-1;
    std::string path_;
};
std::unique_ptr<Owner> adopt(TempFile& file,int* activation,Scope scope){
    return std::unique_ptr<Owner>(Owner::adopt_verified_fd(file.transfer(),activation,scope));
}
Owner::SyncedReceipt get(std::future<Owner::SyncedReceipt>& f) {
    if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("unresolved off-carrier WAL future");
    return f.get();
}
void basic() {
    TempFile temp;
    int authority=1,foreign=2;
    Scope scope{7,9,3,11,5};
    {
        Queue queue(adopt(temp,&authority,scope),&authority,scope);
        rejects<std::invalid_argument>([&]{
            queue.submit(&foreign,scope,frame(7,5,1,41));
        },"foreign activation accepted");
        rejects<std::invalid_argument>([&]{
            queue.submit(&authority,Scope{7,9,3,11,6},frame(7,5,1,41));
        },"stale generation accepted");
        rejects<std::length_error>([&]{
            queue.submit(&authority,scope,std::vector<std::uint8_t>(1119,0));
        },"oversized WAL buffered before byte budget check");
        ok(queue.outstanding()==0,"rejected calls cannot reserve admission");
        auto bytes=frame(7,5,1,41);
        auto f=queue.submit(&authority,scope,bytes);
        bytes.assign(32,0);
        auto receipt=get(f);
        ok(receipt.operation()==41,"physical receipt operation");
        ok(receipt.sequence()==1,"physical receipt sequence");
        ok(receipt.end_offset()==32,"real positional WAL sync");
        ok(receipt.frame()==frame(7,5,1,41),"receipt defensive source copy");
        auto second=queue.submit(&authority,scope,frame(7,5,2,42));
        ok(get(second).end_offset()==64,"second physical sync off carrier");
        // More than twice the fixed ring size: both request and completion
        // cursors wrap while physical end offsets remain sequential.
        for (std::uint32_t seq=3;seq<=70;++seq) {
            auto next=queue.submit(&authority,scope,frame(7,5,seq,seq+40));
            if (get(next).end_offset()!=32u*seq)
                throw std::runtime_error("ring cursor wrap corrupted WAL offset");
        }
        ok(queue.outstanding()==0,"fixed rings drain after wraparound");
        ok(!queue.quarantined(),"healthy owner not quarantined");
        queue.revoke();
        rejects<std::logic_error>([&]{
            queue.submit(&authority,scope,frame(7,5,3,43));
        },"revocation refused new admission");
    }
    const int fd=::open(temp.path().c_str(),O_RDONLY);
    if(fd<0)throw std::runtime_error("reopen failed");
    struct stat st {};
    if(::fstat(fd,&st)!=0){::close(fd);throw std::runtime_error("fstat failed");}
    ok(st.st_size==2240,"reopen preserved synced bytes");
    std::array<std::uint8_t,32> actual{};
    ssize_t n=::pread(fd,actual.data(),actual.size(),0);
    (void)::close(fd);
    auto expected=frame(7,5,1,41);
    ok(n==32 && std::equal(actual.begin(),actual.end(),expected.begin()),
       "byte exact physical read after owner destruction");
}
void bounded_and_healthy_tenant() {
    TempFile one;
    TempFile other;
    int owner1=3,owner2=4;
    Scope scope1{7,9,3,11,5},scope2{8,9,3,12,5};
    std::mutex lock;
    std::condition_variable cv;
    bool entered=false,released=false;
    auto hook=[&]{
        std::unique_lock<std::mutex> guard(lock);
        entered=true;
        cv.notify_all();
        cv.wait(guard,[&]{return released;});
    };
    auto queue=Queue::for_host_fault_test(adopt(one,&owner1,scope1),&owner1,scope1,hook);
    std::vector<std::future<Owner::SyncedReceipt>> pending;
    pending.push_back(queue->submit(&owner1,scope1,frame(7,5,1,100)));
    {
        std::unique_lock<std::mutex> guard(lock);
        ok(cv.wait_for(guard,std::chrono::seconds(5),[&]{return entered;}),
           "worker reached physical boundary");
    }
    for(std::uint32_t i=2;i<=32;++i)
        pending.push_back(queue->submit(&owner1,scope1,frame(7,5,i,99+i)));
    ok(queue->outstanding()==32,"32 total admitted, including executing work");
    rejects<std::length_error>([&]{
        queue->submit(&owner1,scope1,frame(7,5,33,133));
    },"33rd operation admitted");
    {
        Queue healthy(adopt(other,&owner2,scope2),&owner2,scope2);
        auto f=healthy.submit(&owner2,scope2,frame(8,5,1,201));
        ok(get(f).operation()==201,"healthy tenant progresses while first tenant stalls");
    }
    queue->revoke();
    {
        std::lock_guard<std::mutex> guard(lock);
        released=true;
    }
    cv.notify_all();
    int unknown=0;
    for(auto& future:pending){
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            throw std::runtime_error("quarantined future not retired");
        try { (void)future.get(); }
        catch(const Owner::UnknownOutcome&){++unknown;}
    }
    ok(unknown==32,"all queued requests resolve as unknown after revocation");
    queue.reset(); // off-carrier worker joins; native FD finally released
    ok(::access(one.path().c_str(),F_OK)==0,"backing WAL remains for replay");
}
void global_queue_budget() {
    int activation=31;
    Scope scope{7,9,3,11,5};
    std::vector<std::unique_ptr<TempFile>> files;
    std::vector<std::unique_ptr<Queue>> queues;
    for(std::size_t i=0;i<Queue::kMaxDiskQueues;++i) {
        files.push_back(std::make_unique<TempFile>());
        queues.push_back(std::make_unique<Queue>(adopt(*files.back(),&activation,scope),
                                                  &activation,scope));
    }
    TempFile overflow;
    rejects<std::length_error>([&]{
        Queue exceeded(adopt(overflow,&activation,scope),&activation,scope);
    },"global worker budget admitted unbounded queues");
    queues.pop_back(); // both threads join and release one global budget slot
    TempFile reused;
    queues.push_back(std::make_unique<Queue>(adopt(reused,&activation,scope),
                                              &activation,scope));
    ok(queues.back()->outstanding()==0,"retired queue releases global budget");
    queues.clear();
}
void physical_error() {
    TempFile temp;
    int authority=5;
    Scope scope{7,9,3,11,5};
    const int fd=::open(temp.path().c_str(),O_RDONLY);
    if(fd<0)throw std::runtime_error("read-only open failed");
    // adopt only if caller retains exclusive and verified custody. The host test
    // deliberately injects an unusable read-only fd to exercise quarantine.
    auto owner=std::unique_ptr<Owner>(Owner::adopt_verified_fd(fd,&authority,scope));
    Queue queue(std::move(owner),&authority,scope);
    auto first=queue.submit(&authority,scope,frame(7,5,1,11));
    try { (void)get(first); throw std::runtime_error("read-only WAL accepted"); }
    catch (const Owner::UnknownOutcome&) { ok(true,"physical write error becomes Unknown"); }
    ok(queue.quarantined(),"physical failure poisons owner");
    rejects<std::logic_error>([&]{
        queue.submit(&authority,scope,frame(7,5,2,12));
    },"quarantined owner admitted more writes");
}
}
int main() {
    basic();
    bounded_and_healthy_tenant();
    global_queue_budget();
    physical_error();
    std::cout<<"WAL_DISK_QUEUE_HOST_PASS checks="<<checks
             <<"; NO ACTOR PIN OR GUEST PROOF\n";
}
