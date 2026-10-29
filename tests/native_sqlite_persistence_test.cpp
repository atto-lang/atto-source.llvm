// Test the REAL SQLite WAL/FULL provider. Host only: not compiler-generated .ores actor execution.
#include "SqliteBatchPersistence.hpp"
#include <cassert>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <iostream>
#include <filesystem>
using namespace oreslang::runtime::persistence;
static Write write(std::string actor,std::string operation,std::uint64_t epoch=1,std::uint64_t expected=0,WriteKind kind=WriteKind::Snapshot,std::uint8_t data=42){
    return {{"tenant-a",std::move(actor),"main"},std::move(operation),epoch,expected,1,kind,{data}};
}
int main(){
    char folder[]="/tmp/ores-tenant-persistence-XXXXXX";
    char* dir=mkdtemp(folder); assert(dir);
    std::string path=std::string(dir)+"/tenant.db";
    { // 1,000 independent records; batches use the same 256-record admission limit.
        SqliteStore store(path);
        BatchSidecar sidecar("tenant-a",store,2000,1<<20,1024,256,1<<16);
        for(int i=0;i<1000;i++){
            std::string a="a"+std::to_string(i);store.grant_epoch("tenant-a",a,1);
            sidecar.enqueue(write(a,"op"+std::to_string(i)));
        }
        int committed=0, batches=0;
        while(sidecar.queued_count()){
            auto result=sidecar.flush();++batches;
            for(const auto& r:result){assert(r.durable()&&r.status==Status::Committed);++committed;}
        }
        assert(committed==1000&&batches==4);
    }
    { // Reopen after a real process has closed its storage handle.
        SqliteStore store(path);
        auto r=store.load({"tenant-a","a999","main"});
        assert(r.version==1&&r.snapshot_version==1&&r.snapshot[0]==42);
        auto retry=store.commit({write("a999","op999")});
        assert(retry.size()==1&&retry[0].status==Status::Duplicate&&retry[0].durable());
        store.grant_epoch("tenant-a","a999",2);
        auto old=store.commit({write("a999","op999")});assert(old[0].status==Status::StaleEpoch);
        auto conflict=store.commit({write("a999","op999",2,1)});
        assert(conflict[0].status==Status::OperationConflict);
        auto journal=store.commit({write("a999","journal",2,1,WriteKind::Event,65)});
        assert(journal[0].status==Status::Committed&&journal[0].durable());
        auto x=store.load({"tenant-a","a999","main"});
        assert(x.version==2&&x.snapshot_version==1&&x.events.size()==1&&x.events[0].payload[0]==65);
    }
    { // Child's completed SQLite commit survives abrupt _exit; unfinished queued write does not.
        pid_t pid=fork();assert(pid>=0);
        if(pid==0){
            SqliteStore s(path);s.grant_epoch("tenant-a","child",1);
            auto written=s.commit({write("child","committed")});
            if(written.size()!=1||!written[0].durable())_exit(7);
            BatchSidecar queue("tenant-a",s,10,4096,16,10,64);
            queue.enqueue(write("child","not-flushed",1,1));
            _exit(0); // simulate process loss before flush; destructors are intentionally bypassed
        }
        int status=0;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
        SqliteStore store(path);
        assert(store.load({"tenant-a","child","main"}).version==1);
        auto after=store.commit({write("child","not-flushed",1,1)});
        assert(after[0].status==Status::Committed&&after[0].version==2);
    }
    { // Version CAS within one batch: actor A conflict does not poison actor B.
        SqliteStore store(path);
        store.grant_epoch("tenant-a","mixed-a",1);store.grant_epoch("tenant-a","mixed-b",1);
        auto r=store.commit({write("mixed-a","1"),write("mixed-a","2"),write("mixed-b","1")});
        assert(r.size()==3&&r[0].durable()&&r[1].status==Status::VersionConflict&&r[2].durable());
        assert(store.load({"tenant-a","mixed-b","main"}).version==1);
    }
    std::filesystem::remove_all(dir);
    std::cout<<"PASS real SQLite WAL/FULL host provider (4 groups, 1000 actors, reopen, abrupt child exit)\n";
}
