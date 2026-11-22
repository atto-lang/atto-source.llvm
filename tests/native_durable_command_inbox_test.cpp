// True native C++ SQLite host integration. Not compiled AttoLang guest.
#include "oreslang/runtime/SqliteBatchPersistence.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <unistd.h> // mkdtemp on macOS
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::runtime::persistence;
static std::vector<std::uint8_t> b(const std::string& s) {return {s.begin(),s.end()};}
static SqliteStore::CommandEvent ev(const std::string& p) {return {1,b(p)};}
template<typename F>static bool rejected(F f) {
    try {f();return false;} catch(const std::logic_error&) {return true;}
}
int main() {
    char folder[]="/tmp/atto-native-inbox-XXXXXX";
    char* dir=mkdtemp(folder);
    if(!dir)throw std::runtime_error("cannot create SQLite directory");
    const std::string db=std::string(dir)+"/inbox.sqlite";
    const Identity account{"tenant-a","bank-42","main"};
    try{
        {
            SqliteStore store(db);
            store.grant_epoch(account.tenant,account.actor,1);
            auto saved=store.commit_command(account,1,"op-42",0,b("Deposit(100)"),
                {ev("MoneyDeposited(60)"),ev("MoneyDeposited(40)")},b("Ok(balance=100)"));
            assert(!saved.duplicate&&saved.first_version==1&&saved.last_version==2
                   &&saved.reply==b("Ok(balance=100)"));
            // Compare persisted SQLite bytes, not an in-memory re-encoding:
            // the exact wire is shared by Java, LLVM, Wasm, Forge and .NET.
            sqlite3* audit=nullptr;
            if(sqlite3_open(db.c_str(),&audit)!=SQLITE_OK)
                throw std::runtime_error("cannot inspect committed command batch");
            std::string persisted;
            const int read_status=sqlite3_exec(audit,
                "SELECT hex(event_batch) FROM command_inbox "
                "WHERE tenant='tenant-a' AND actor='bank-42' "
                "AND slot='main' AND op='op-42'",
                [](void* output,int columns,char** fields,char**)->int {
                    if(columns!=1 || !fields || !fields[0]) return 1;
                    *static_cast<std::string*>(output)=fields[0];
                    return 0;
                },&persisted,nullptr);
            sqlite3_close(audit);
            if(read_status!=SQLITE_OK)
                throw std::runtime_error("cannot read committed command batch");
            assert(persisted==
                "0000000200000001000000124D6F6E65794465706F736974656428363029"
                "00000001000000124D6F6E65794465706F736974656428343029");
            auto recovered=store.load_fenced(account,1);
            assert(recovered.version==2 && recovered.events.size()==2);
            auto same=store.commit_command(account,1,"op-42",0,b("Deposit(100)"),
                {ev("new decide() must not run")},b("incorrect"));
            assert(same.duplicate && same.last_version==2 &&
                   same.reply==b("Ok(balance=100)"));
            assert(rejected([&]{(void)store.resolve_command_fenced(account,1,"op-42",b("Deposit(200)"));}));
            store.grant_epoch(account.tenant,account.actor,2);
            assert(rejected([&]{(void)store.resolve_command_fenced(account,1,"op-42",b("Deposit(100)"));}));
            auto previous=store.resolve_command_fenced(account,2,"op-42",b("Deposit(100)"));
            assert(previous && previous->duplicate&&previous->last_version==2
                &&previous->reply==b("Ok(balance=100)"));
            assert(!store.resolve_command_fenced(account,2,"missing",b("Deposit(100)")));
            assert(rejected([&]{
                (void)store.commit_command(account,2,"op-42",2,b("Deposit(200)"),
                    {ev("money")},b("wrong"));}));
            assert(store.load_fenced(account,2).events.size()==2);
            auto next=store.commit_command(account,2,"op-43",2,b("Deposit(5)"),
                {ev("MoneyDeposited(5)")},b("Ok(balance=105)"));
            assert(next.last_version==3);
        }
        {
            SqliteStore reopened(db);
            auto old=reopened.resolve_command_fenced(account,2,"op-42",b("Deposit(100)"));
            assert(old&&old->reply==b("Ok(balance=100)"));
            old->reply[0]=0;
            assert(reopened.resolve_command_fenced(account,2,"op-42",
                b("Deposit(100)"))->reply==b("Ok(balance=100)"));
        }
        {
            SqliteStore store(db);
            const Identity fail{"tenant-a","bank-fault","main"};
            store.grant_epoch(fail.tenant,fail.actor,1);
            sqlite3* injector=nullptr;
            if(sqlite3_open(db.c_str(),&injector)!=SQLITE_OK)
                throw std::runtime_error("injector open failed");
            char* error=nullptr;
            if(sqlite3_exec(injector,"CREATE TRIGGER fail_second BEFORE INSERT ON events "
                "WHEN NEW.actor='bank-fault' AND NEW.version=2 "
                "BEGIN SELECT RAISE(ABORT,'injected'); END",nullptr,nullptr,&error)!=SQLITE_OK){
                sqlite3_free(error);sqlite3_close(injector);
                throw std::runtime_error("trigger creation failed");
            }
            bool failed=false;
            try { (void)store.commit_command(fail,1,"op-fail",0,b("Deposit(10)"),
                {ev("6"),ev("4")},b("Ok(10)")); }
            catch(const std::exception&) {failed=true;}
            assert(failed);
            assert(store.load_fenced(fail,1).version==0);
            assert(!store.resolve_command_fenced(fail,1,"op-fail",b("Deposit(10)")));
            if(sqlite3_exec(injector,"DROP TRIGGER fail_second",nullptr,nullptr,&error)!=SQLITE_OK) {
                sqlite3_free(error);sqlite3_close(injector);
                throw std::runtime_error("trigger removal failed");
            }
            sqlite3_close(injector);
            assert(store.commit_command(fail,1,"op-fail",0,b("Deposit(10)"),
                {ev("6"),ev("4")},b("Ok(10)")).last_version==2);
        }
    }catch(...){std::filesystem::remove_all(dir);throw;}
    std::filesystem::remove_all(dir);
    std::cout<<"PASS native real SQLite durable inbox + atomic events + recovery\n";
}
