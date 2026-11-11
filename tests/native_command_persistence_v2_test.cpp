// Executable native SQLite v2 host command parity. NOT .ores compiler output.
#include "SqliteCommandPersistenceV2.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
using namespace oreslang::runtime::persistence::v2;
static Bytes b(const std::string& x){return {x.begin(),x.end()};}
static Command command(std::string op,std::uint64_t epoch,std::uint64_t version,
                       std::string id="deposit:100"){
    return {{"tenant-a","bank","main"},std::move(op),epoch,version,1,b(id),{b("+40"),b("+60")},b("100")};
}
static void fail(const std::string& code,const std::function<void()>& f){
    bool rejected=false;try{f();}catch(const std::exception& e){rejected=std::string(e.what()).find(code)!=std::string::npos;}
    assert(rejected);
}
int main(){
    char path_template[]="/tmp/ores-native-command-v2-XXXXXX";
    char* dir=mkdtemp(path_template);assert(dir);
    std::string db=std::string(dir)+"/store.sqlite";
    const Identity id{"tenant-a","bank","main"};
    // An actor with no activation fence reports epoch zero in the database.
    // Zero is NOT an authorized incarnation and must not permit a read or proof.
    {
        SqliteCommandStore store(db,"tenant-a");
        const Identity never_activated{"tenant-a","never-activated","main"};
        fail("INVALID_EPOCH",[&]{(void)store.load_fenced(never_activated,0);});
        fail("INVALID_EPOCH",[&]{(void)store.verify(never_activated,"missing",0,b("x"));});
        fail("INVALID_EPOCH",[&]{(void)store.load_fenced(never_activated,UINT64_MAX);});
        fail("INVALID_EPOCH",[&]{(void)store.verify(never_activated,"missing",UINT64_MAX,b("x"));});
        fail("STALE_EPOCH",[&]{(void)store.load_fenced(never_activated,1);});
        fail("STALE_EPOCH",[&]{(void)store.verify(never_activated,"missing",1,b("x"));});
    }
    {
        SqliteCommandStore store(db,"tenant-a");store.grant_epoch("bank",1);
        auto first=store.append(command("deposit-1",1,0));
        assert(!first.duplicate&&first.last_version==2&&first.event_count==2);
        auto r=store.load_fenced(id,1);assert(r.version==2&&r.events.size()==2);
        assert(r.events[0].payload==b("+40")&&r.events[1].payload==b("+60"));
        assert(store.verify(id,"deposit-1",1,b("deposit:100"))->last_version==2);
        fail("VERSION_CONFLICT",[&]{store.append(command("deposit-2",1,0));});
        fail("OP_ID_CONFLICT",[&]{store.append(command("deposit-1",1,0,"deposit:101"));});
        store.grant_epoch("bank",2);
        fail("STALE_EPOCH",[&]{store.verify(id,"deposit-1",1,b("deposit:100"));});
        auto duplicate=store.append(command("deposit-1",2,2));
        assert(duplicate.duplicate&&duplicate.last_version==2);
        assert(store.load_fenced(id,2).events.size()==2);
    }
    {
        SqliteCommandStore reopened(db,"tenant-a");reopened.grant_epoch("bank",3);
        assert(reopened.append(command("deposit-1",3,2)).duplicate);
        assert(reopened.load_fenced(id,3).version==2);
    }
    // Real second-insert failure must roll back head, both events and receipt.
    std::string rollback=db+"-rollback";
    {
        SqliteCommandStore store(rollback,"tenant-a");store.grant_epoch("bank",1);
        sqlite3* raw=nullptr;assert(sqlite3_open(rollback.c_str(),&raw)==SQLITE_OK);
        assert(sqlite3_exec(raw,"CREATE TRIGGER reject_second BEFORE INSERT ON native_command_v2_events WHEN NEW.version=2 BEGIN SELECT RAISE(ABORT,'second event'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(raw);
        fail("SQLITE_FAILURE",[&]{store.append(command("uncommitted",1,0));});
        assert(store.load_fenced(id,1).version==0);
        assert(!store.verify(id,"uncommitted",1,b("deposit:100")));
        assert(sqlite3_open(rollback.c_str(),&raw)==SQLITE_OK);
        assert(sqlite3_exec(raw,"DROP TRIGGER reject_second",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(raw);
        assert(store.append(command("uncommitted",1,0)).last_version==2);
    }
    // Actual child death after SQLite COMMIT but before owner ACK.
    std::string crashdb=db+"-crash";
    pid_t child=fork();assert(child>=0);
    if(child==0){
        SqliteCommandStore store(crashdb,"tenant-a");store.grant_epoch("bank",1);
        (void)store.append(command("crash-deposit",1,0));
        kill(getpid(),SIGKILL); // actual OS kill; no destructor or actor ACK
        _exit(200);
    }
    int status=0;assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    {
        SqliteCommandStore store(crashdb,"tenant-a");store.grant_epoch("bank",2);
        assert(store.load_fenced(id,2).version==2);
        assert(store.append(command("crash-deposit",2,2)).duplicate);
        assert(store.load_fenced(id,2).events.size()==2);
    }
    // Count limits alone do not bound total replay memory. Keep storage
    // receipts after replay-byte refusal so the caller can fail closed.
    std::string bounded=db+"-bounded";
    {
        SqliteCommandStore store(bounded,"tenant-a",3);
        store.grant_epoch("bank",1);
        auto first=command("tiny-first",1,0);first.events={b("+1")};
        assert(store.append(first).last_version==1);
        assert(store.load_fenced(id,1).events.size()==1);
        auto second=command("tiny-second",1,1);second.events={b("+2")};
        assert(store.append(second).last_version==2);
        fail("REPLAY_BYTES_LIMIT_EXCEEDED",[&]{(void)store.load_fenced(id,1);});
        assert(store.verify(id,"tiny-second",1,b("deposit:100"))->last_version==2);
    }
    fail("INVALID_CONFIG",[&]{SqliteCommandStore invalid(bounded,"tenant-a",0);});
    std::filesystem::remove_all(dir);
    std::filesystem::remove(rollback);std::filesystem::remove(crashdb);
    std::filesystem::remove(bounded);
    std::cout<<"PASS native SQLite v2 multi-event, epoch retry, rollback and process crash\n";
}
