#pragma once
// Trusted host-only SQLite v2 command contract. NOT an .ores actor or async carrier.
// Canonical command bytes are stored intact: same-op retries compare the full bytes.
#include <sqlite3.h>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime::persistence::v2 {
using Bytes = std::vector<std::uint8_t>;
struct Identity { std::string tenant,actor,slot; };
struct Command {
    Identity id;
    std::string operation_id;
    std::uint64_t epoch,expected_version;
    std::uint32_t schema_version;
    Bytes command_bytes;
    std::vector<Bytes> events;
    Bytes result_bytes;
};
struct Receipt {
    std::string operation_id;
    std::uint64_t first_version,last_version;
    std::uint32_t event_count;
    Bytes result_bytes;
    bool duplicate;
};
struct Event { std::uint64_t version;std::uint32_t schema_version;std::string operation_id;Bytes payload; };
struct Recovery { std::uint64_t version;std::vector<Event> events; };
inline void valid(const std::string& s) {
    // Until Unicode-NFC/ICU canonicalization is specified on native v2, fail
    // closed for non-ASCII rather than silently admit non-canonical aliases.
    if(s.empty()||s.size()>128)throw std::invalid_argument("INVALID_IDENTITY");
    for(unsigned char c:s)if(c<33||c>126)throw std::invalid_argument("INVALID_IDENTITY");
}
class SqliteCommandStore {
    sqlite3* db_=nullptr;
    std::mutex lock_;
    static constexpr std::size_t MAX_REPLAY_BYTES = 64u * 1024u * 1024u;
    std::size_t replay_byte_limit_ = MAX_REPLAY_BYTES;
    static void chk(int rc) {
        if(rc!=SQLITE_OK && rc!=SQLITE_DONE && rc!=SQLITE_ROW)
            throw std::runtime_error("SQLITE_FAILURE");
    }
    void exec(const char* sql) { chk(sqlite3_exec(db_,sql,nullptr,nullptr,nullptr)); }
    void begin(){exec("BEGIN IMMEDIATE");}
    void commit(){exec("COMMIT");}
    void rollback() noexcept {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);}
    struct Stmt {
        sqlite3_stmt* s=nullptr;
        explicit Stmt(sqlite3* db,const char* query){chk(sqlite3_prepare_v2(db,query,-1,&s,nullptr));}
        ~Stmt(){if(s)sqlite3_finalize(s);}
        Stmt(const Stmt&)=delete;
        Stmt& operator=(const Stmt&)=delete;
        void text(int col,const std::string& x){chk(sqlite3_bind_text(s,col,x.data(),static_cast<int>(x.size()),SQLITE_TRANSIENT));}
        void integer(int col,std::uint64_t x){if(x>INT64_MAX)throw std::invalid_argument("INVALID_VERSION");chk(sqlite3_bind_int64(s,col,static_cast<sqlite3_int64>(x)));}
        void blob(int col,const Bytes& x){if(x.size()>INT_MAX)throw std::length_error("PAYLOAD_TOO_LARGE");chk(sqlite3_bind_blob(s,col,x.empty()?static_cast<const void*>(""):x.data(),static_cast<int>(x.size()),SQLITE_TRANSIENT));}
        int step(){int rc=sqlite3_step(s);chk(rc);return rc;}
    };
    static void bind(Stmt& q,const Identity& id){q.text(1,id.tenant);q.text(2,id.actor);q.text(3,id.slot);}
    static Bytes bytes(sqlite3_stmt* s,int col){const auto* p=static_cast<const std::uint8_t*>(sqlite3_column_blob(s,col));const int n=sqlite3_column_bytes(s,col);return n>0?Bytes(p,p+n):Bytes{};}
    std::uint64_t epoch(const Identity& id){
        Stmt q(db_,"SELECT epoch FROM fences WHERE tenant=? AND actor=?");q.text(1,id.tenant);q.text(2,id.actor);
        return q.step()==SQLITE_ROW?static_cast<std::uint64_t>(sqlite3_column_int64(q.s,0)):0;
    }
    std::uint64_t head(const Identity& id){
        Stmt q(db_,"SELECT version FROM native_command_v2_heads WHERE tenant=? AND actor=? AND slot=?");bind(q,id);
        return q.step()==SQLITE_ROW?static_cast<std::uint64_t>(sqlite3_column_int64(q.s,0)):0;
    }
    std::optional<Receipt> lookup(const Identity& id,const std::string& op,const Bytes& command){
        Stmt q(db_,"SELECT command_bytes,first_version,last_version,event_count,result_bytes FROM native_command_v2_receipts WHERE tenant=? AND actor=? AND slot=? AND op_id=?");
        bind(q,id);q.text(4,op);if(q.step()==SQLITE_DONE)return std::nullopt;
        if(bytes(q.s,0)!=command)throw std::logic_error("OP_ID_CONFLICT");
        auto first=static_cast<std::uint64_t>(sqlite3_column_int64(q.s,1));
        auto last=static_cast<std::uint64_t>(sqlite3_column_int64(q.s,2));
        auto count=static_cast<std::uint32_t>(sqlite3_column_int(q.s,3));
        if(first<1||last<first||last>head(id)||count==0||last-first+1!=count)
            throw std::logic_error("CORRUPT_COMMAND_RECORD");
        return Receipt{op,first,last,count,bytes(q.s,4),true};
    }
    bool covers(const Identity& id,const std::string& op,std::uint64_t version){
        Stmt q(db_,"SELECT first_version,last_version,event_count FROM native_command_v2_receipts WHERE tenant=? AND actor=? AND slot=? AND op_id=?");
        bind(q,id);q.text(4,op);if(q.step()==SQLITE_DONE)return false;
        auto f=static_cast<std::uint64_t>(sqlite3_column_int64(q.s,0));
        auto l=static_cast<std::uint64_t>(sqlite3_column_int64(q.s,1));
        auto count=static_cast<std::uint32_t>(sqlite3_column_int(q.s,2));
        return f>0&&l>=f&&l-f+1==count&&version>=f&&version<=l;
    }
    void tenant(const Identity& id)const {if(id.tenant!=tenant_)throw std::invalid_argument("WRONG_TENANT");}
    std::string tenant_;
public:
    SqliteCommandStore(const std::string& filename,std::string tenant,
                       std::size_t replay_byte_limit=MAX_REPLAY_BYTES)
        :replay_byte_limit_(replay_byte_limit),tenant_(std::move(tenant)){
        valid(tenant_);
        if(replay_byte_limit_==0||replay_byte_limit_>MAX_REPLAY_BYTES)
            throw std::invalid_argument("INVALID_CONFIG");
        if(filename.empty())throw std::invalid_argument("INVALID_PATH");
        if(sqlite3_open_v2(filename.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK){
            if(db_) sqlite3_close(db_);
            db_=nullptr;
            throw std::runtime_error("SQLITE_OPEN_FAILED");
        }
        try{
            sqlite3_busy_timeout(db_,5000);
            exec("PRAGMA journal_mode=WAL");exec("PRAGMA synchronous=FULL");
            exec("CREATE TABLE IF NOT EXISTS fences(tenant TEXT NOT NULL,actor TEXT NOT NULL,epoch INTEGER NOT NULL,PRIMARY KEY(tenant,actor))");
            exec("CREATE TABLE IF NOT EXISTS native_command_v2_heads(tenant TEXT NOT NULL,actor TEXT NOT NULL,slot TEXT NOT NULL,version INTEGER NOT NULL,PRIMARY KEY(tenant,actor,slot))");
            exec("CREATE TABLE IF NOT EXISTS native_command_v2_events(tenant TEXT NOT NULL,actor TEXT NOT NULL,slot TEXT NOT NULL,version INTEGER NOT NULL,op_id TEXT NOT NULL,schema INTEGER NOT NULL,payload BLOB NOT NULL,PRIMARY KEY(tenant,actor,slot,version))");
            exec("CREATE TABLE IF NOT EXISTS native_command_v2_receipts(tenant TEXT NOT NULL,actor TEXT NOT NULL,slot TEXT NOT NULL,op_id TEXT NOT NULL,command_bytes BLOB NOT NULL,first_version INTEGER NOT NULL,last_version INTEGER NOT NULL,event_count INTEGER NOT NULL,result_bytes BLOB NOT NULL,PRIMARY KEY(tenant,actor,slot,op_id))");
            exec("CREATE TRIGGER IF NOT EXISTS native_command_v2_immutable_delete BEFORE DELETE ON native_command_v2_receipts BEGIN SELECT RAISE(ABORT,'IMMUTABLE_COMMAND_RECEIPT'); END");
            exec("CREATE TRIGGER IF NOT EXISTS native_command_v2_immutable_update BEFORE UPDATE ON native_command_v2_receipts BEGIN SELECT RAISE(ABORT,'IMMUTABLE_COMMAND_RECEIPT'); END");
        }catch(...){sqlite3_close(db_);db_=nullptr;throw;}
    }
    ~SqliteCommandStore(){if(db_)sqlite3_close(db_);}
    SqliteCommandStore(const SqliteCommandStore&)=delete;
    SqliteCommandStore& operator=(const SqliteCommandStore&)=delete;
    void grant_epoch(const std::string& actor,std::uint64_t requested){
        std::lock_guard<std::mutex> guard(lock_);valid(actor);
        if(requested<1||requested>INT64_MAX)throw std::invalid_argument("INVALID_EPOCH");
        begin();try{
            Identity id{tenant_,actor,"main"};if(epoch(id)>requested)throw std::logic_error("STALE_EPOCH");
            Stmt q(db_,"INSERT INTO fences VALUES(?,?,?) ON CONFLICT(tenant,actor) DO UPDATE SET epoch=excluded.epoch");
            q.text(1,tenant_);q.text(2,actor);q.integer(3,requested);q.step();commit();
        }catch(...){rollback();throw;}
    }
    Receipt append(const Command& command){
        std::lock_guard<std::mutex> guard(lock_);const auto& id=command.id;
        tenant(id);valid(id.actor);valid(id.slot);valid(command.operation_id);
        if(command.epoch<1||command.epoch>INT64_MAX||command.expected_version>INT64_MAX||command.schema_version<1||
           command.events.empty()||command.events.size()>256)throw std::invalid_argument("INVALID_COMMAND");
        std::size_t charge=command.command_bytes.size()+command.result_bytes.size();
        for(const auto& ev:command.events){if(ev.size()>(1u<<20)||charge>(1u<<20)-ev.size())throw std::length_error("PAYLOAD_TOO_LARGE");charge+=ev.size();}
        if(charge>(1u<<20))throw std::length_error("PAYLOAD_TOO_LARGE");
        begin();try{
            if(epoch(id)!=command.epoch)throw std::logic_error("STALE_EPOCH");
            if(auto old=lookup(id,command.operation_id,command.command_bytes)){
                commit();return *old;
            }
            auto version=head(id);
            if(version!=command.expected_version)throw std::logic_error("VERSION_CONFLICT");
            if(command.events.size()>INT64_MAX-version)throw std::logic_error("VERSION_EXHAUSTED");
            std::uint32_t i=0;
            for(const auto& ev:command.events){
                Stmt q(db_,"INSERT INTO native_command_v2_events VALUES(?,?,?,?,?,?,?)");
                bind(q,id);q.integer(4,version+ ++i);q.text(5,command.operation_id);
                q.integer(6,command.schema_version);q.blob(7,ev);q.step();
            }
            const auto next=version+i;
            {Stmt q(db_,"INSERT INTO native_command_v2_heads VALUES(?,?,?,?) ON CONFLICT(tenant,actor,slot) DO UPDATE SET version=excluded.version");bind(q,id);q.integer(4,next);q.step();}
            {Stmt q(db_,"INSERT INTO native_command_v2_receipts VALUES(?,?,?,?,?,?,?,?,?)");bind(q,id);q.text(4,command.operation_id);q.blob(5,command.command_bytes);q.integer(6,version+1);q.integer(7,next);q.integer(8,i);q.blob(9,command.result_bytes);q.step();}
            commit();return Receipt{command.operation_id,version+1,next,i,command.result_bytes,false};
        }catch(...){rollback();throw;}
    }
    std::optional<Receipt> verify(const Identity& id,const std::string& op,std::uint64_t expected_epoch,const Bytes& command){
        std::lock_guard<std::mutex> guard(lock_);tenant(id);valid(id.actor);valid(id.slot);valid(op);
        if(expected_epoch==0 || expected_epoch>INT64_MAX)throw std::invalid_argument("INVALID_EPOCH");
        begin();try{
            if(epoch(id)!=expected_epoch)throw std::logic_error("STALE_EPOCH");
            auto proof=lookup(id,op,command);commit();return proof;
        }catch(...){rollback();throw;}
    }
    Recovery load_fenced(const Identity& id,std::uint64_t expected_epoch){
        std::lock_guard<std::mutex> guard(lock_);tenant(id);valid(id.actor);valid(id.slot);
        if(expected_epoch==0 || expected_epoch>INT64_MAX)throw std::invalid_argument("INVALID_EPOCH");
        begin();try{
            if(epoch(id)!=expected_epoch)throw std::logic_error("STALE_EPOCH");
            const auto version=head(id);std::uint64_t cursor=0;
            std::size_t replayed_bytes=0;
            Recovery result{version,{}};
            Stmt q(db_,"SELECT version,op_id,schema,payload,length(payload) FROM native_command_v2_events WHERE tenant=? AND actor=? AND slot=? ORDER BY version ASC LIMIT 65537");bind(q,id);
            while(q.step()==SQLITE_ROW){
                if(result.events.size()>=65536)throw std::logic_error("REPLAY_LIMIT_EXCEEDED");
                const auto next=static_cast<std::uint64_t>(sqlite3_column_int64(q.s,0));
                const std::string op(reinterpret_cast<const char*>(sqlite3_column_text(q.s,1)));
                const auto schema=static_cast<std::uint32_t>(sqlite3_column_int(q.s,2));
                if(next!=cursor+1||schema==0)throw std::logic_error("JOURNAL_GAP");
                if(!covers(id,op,next))throw std::logic_error("UNINDEXED_JOURNAL");
                // Check SQL BLOB length before copying into the recovery
                // vector. An event-count bound alone permits huge replays.
                if(sqlite3_column_type(q.s,4)==SQLITE_NULL)
                    throw std::logic_error("CORRUPT_JOURNAL_PAYLOAD");
                const auto length=sqlite3_column_int64(q.s,4);
                if(length<0||static_cast<std::uint64_t>(length)>replay_byte_limit_-replayed_bytes)
                    throw std::logic_error("REPLAY_BYTES_LIMIT_EXCEEDED");
                Bytes payload=bytes(q.s,3);
                if(payload.size()!=static_cast<std::size_t>(length))
                    throw std::logic_error("CORRUPT_JOURNAL_PAYLOAD");
                replayed_bytes+=static_cast<std::size_t>(length);
                result.events.push_back(Event{next,schema,op,std::move(payload)});cursor=next;
            }
            if(cursor!=version)throw std::logic_error("JOURNAL_GAP");
            commit();return result;
        }catch(...){rollback();throw;}
    }
};
} // namespace oreslang::runtime::persistence::v2
