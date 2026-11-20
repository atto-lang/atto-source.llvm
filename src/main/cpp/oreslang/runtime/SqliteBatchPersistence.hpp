#pragma once
// SQLite WAL provider reference for trusted host I/O workers, not a VM sandbox or .ores guest.
#include "NativeBatchPersistence.hpp"
#include <sqlite3.h>
#include <climits>
#include <cstring>
#include <memory>
#include <optional>

namespace oreslang::runtime::persistence {
class SqliteStore final : public Store {
    sqlite3* db_ = nullptr;
    mutable std::mutex mu_;
    static void check(int rc) {
        if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
            throw std::runtime_error("SQLite persistence operation failed");
    }
    struct Statement {
        sqlite3_stmt* p = nullptr;
        Statement(sqlite3* db, const char* sql) { check(sqlite3_prepare_v2(db,sql,-1,&p,nullptr)); }
        ~Statement() { if (p) sqlite3_finalize(p); }
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;
        void text(int n, const std::string& value) {
            check(sqlite3_bind_text(p,n,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT));
        }
        void integer(int n, std::uint64_t value) {
            if (value > static_cast<std::uint64_t>(INT64_MAX)) throw std::invalid_argument("i64 storage value exceeded");
            check(sqlite3_bind_int64(p,n,static_cast<sqlite3_int64>(value)));
        }
        void blob(int n, const std::vector<std::uint8_t>& bytes) {
            if (bytes.size() > INT_MAX) throw std::length_error("blob too large");
            check(sqlite3_bind_blob(p,n,bytes.empty()?static_cast<const void*>(""):static_cast<const void*>(bytes.data()),static_cast<int>(bytes.size()),SQLITE_TRANSIENT));
        }
        void null(int n) { check(sqlite3_bind_null(p,n)); }
        int step() { int rc=sqlite3_step(p); check(rc); return rc; }
    };
    void sql(const char* statement) { check(sqlite3_exec(db_,statement,nullptr,nullptr,nullptr)); }
    void begin() { sql("BEGIN IMMEDIATE"); }
    void rollback() noexcept { sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr); }
    void commit() { sql("COMMIT"); }
    static void bind_identity(Statement& q, const Identity& id) {
        q.text(1,id.tenant); q.text(2,id.actor); q.text(3,id.slot);
    }
    std::optional<std::uint64_t> epoch(const Identity& id) {
        Statement q(db_,"SELECT epoch FROM fences WHERE tenant=? AND actor=?");
        q.text(1,id.tenant);q.text(2,id.actor);
        if (q.step()==SQLITE_DONE) return std::nullopt;
        return static_cast<std::uint64_t>(sqlite3_column_int64(q.p,0));
    }
    std::uint64_t version(const Identity& id) {
        Statement q(db_,"SELECT version FROM state WHERE tenant=? AND actor=? AND slot=?");
        bind_identity(q,id);
        if(q.step()==SQLITE_DONE)return 0;
        return static_cast<std::uint64_t>(sqlite3_column_int64(q.p,0));
    }
public:
    struct Event { std::uint64_t version; std::uint32_t schema_version; std::vector<std::uint8_t> payload; };
    struct Recovery {
        std::uint64_t version=0, snapshot_version=0;
        std::uint32_t snapshot_schema=0;
        std::vector<std::uint8_t> snapshot;
        std::vector<Event> events;
    };
    explicit SqliteStore(const std::string& trusted_path) {
        if (trusted_path.empty() || trusted_path==":memory:" || trusted_path.rfind("file:",0)==0)
            throw std::invalid_argument("durable provider requires trusted file path");
        if (sqlite3_open_v2(trusted_path.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
            if(db_){sqlite3_close(db_);db_=nullptr;}
            throw std::runtime_error("SQLite persistence database open failed");
        }
        try {
            sql("PRAGMA busy_timeout=5000");
            Statement mode(db_,"PRAGMA journal_mode=WAL");
            if (mode.step()!=SQLITE_ROW ||
                std::string(reinterpret_cast<const char*>(sqlite3_column_text(mode.p,0)))!="wal")
                throw std::runtime_error("SQLite WAL mode unavailable");
            sql("PRAGMA synchronous=FULL");
            sql("CREATE TABLE IF NOT EXISTS fences(tenant TEXT NOT NULL, actor TEXT NOT NULL, epoch INTEGER NOT NULL, PRIMARY KEY(tenant,actor))");
            sql("CREATE TABLE IF NOT EXISTS state(tenant TEXT NOT NULL, actor TEXT NOT NULL, slot TEXT NOT NULL, version INTEGER NOT NULL, snapshot_version INTEGER NOT NULL DEFAULT 0, snapshot_schema INTEGER NOT NULL DEFAULT 0, snapshot BLOB, PRIMARY KEY(tenant,actor,slot))");
            sql("CREATE TABLE IF NOT EXISTS ops(tenant TEXT NOT NULL, actor TEXT NOT NULL, slot TEXT NOT NULL, op TEXT NOT NULL, epoch INTEGER NOT NULL, expected INTEGER NOT NULL, kind INTEGER NOT NULL, schema INTEGER NOT NULL, payload BLOB NOT NULL, version INTEGER NOT NULL, PRIMARY KEY(tenant,actor,slot,op))");
            sql("CREATE TABLE IF NOT EXISTS events(tenant TEXT NOT NULL, actor TEXT NOT NULL, slot TEXT NOT NULL, version INTEGER NOT NULL, schema INTEGER NOT NULL, payload BLOB NOT NULL, PRIMARY KEY(tenant,actor,slot,version))");
            sql("CREATE TABLE IF NOT EXISTS command_inbox(tenant TEXT NOT NULL, actor TEXT NOT NULL, slot TEXT NOT NULL, op TEXT NOT NULL, epoch INTEGER NOT NULL, expected INTEGER NOT NULL, request BLOB NOT NULL, first_version INTEGER NOT NULL, last_version INTEGER NOT NULL, event_batch BLOB NOT NULL, reply BLOB NOT NULL, PRIMARY KEY(tenant,actor,slot,op))");
        } catch(...) { sqlite3_close(db_);db_=nullptr; throw; }
    }
    ~SqliteStore() override { if(db_) sqlite3_close(db_); }
    SqliteStore(const SqliteStore&)=delete;
    SqliteStore& operator=(const SqliteStore&)=delete;
    void grant_epoch(const std::string& tenant,const std::string& actor,std::uint64_t requested) override {
        if (!valid_id(tenant)||!valid_id(actor)||requested==0||requested>INT64_MAX)
            throw std::invalid_argument("invalid trusted epoch grant");
        std::lock_guard<std::mutex> lock(mu_);
        begin();
        try {
            auto current=epoch({tenant,actor,"main"});
            if(current && *current>requested) throw std::logic_error("STALE_EPOCH");
            Statement q(db_,"INSERT INTO fences VALUES(?,?,?) ON CONFLICT(tenant,actor) DO UPDATE SET epoch=excluded.epoch");
            q.text(1,tenant);q.text(2,actor);q.integer(3,requested);q.step();
            commit();
        } catch(...) {rollback();throw;}
    }
    std::vector<Receipt> commit(const std::vector<Write>& writes) override {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Receipt> results;results.reserve(writes.size());
        begin();
        try {
            for(const auto& w:writes) {
                if(!valid_id(w.id.tenant)||!valid_id(w.id.actor)||!valid_id(w.id.slot)||!valid_id(w.operation_id)||
                   !w.epoch||!w.schema_version||w.epoch>INT64_MAX||w.expected_version>=INT64_MAX||
                   w.payload.size()>1048576||
                   (w.kind!=WriteKind::Snapshot && w.kind!=WriteKind::Event))
                    throw std::invalid_argument("invalid persistent write metadata");
                if(epoch(w.id)!=std::optional<std::uint64_t>(w.epoch)) {
                    results.push_back({w.operation_id,Status::StaleEpoch,0,false});continue;
                }
                Statement old(db_,"SELECT epoch,expected,kind,schema,payload,version FROM ops WHERE tenant=? AND actor=? AND slot=? AND op=?");
                bind_identity(old,w.id);old.text(4,w.operation_id);
                if(old.step()==SQLITE_ROW) {
                    const auto bytes=sqlite3_column_bytes(old.p,4);
                    const void* src=sqlite3_column_blob(old.p,4);
                    bool match=sqlite3_column_int64(old.p,0)==static_cast<sqlite3_int64>(w.epoch)&&
                        sqlite3_column_int64(old.p,1)==static_cast<sqlite3_int64>(w.expected_version)&&
                        sqlite3_column_int(old.p,2)==static_cast<int>(w.kind)&&
                        sqlite3_column_int(old.p,3)==static_cast<int>(w.schema_version)&&
                        bytes==static_cast<int>(w.payload.size())&&
                        (bytes==0||std::memcmp(src,w.payload.data(),static_cast<std::size_t>(bytes))==0);
                    results.push_back({w.operation_id,match?Status::Duplicate:Status::OperationConflict,
                        match?static_cast<std::uint64_t>(sqlite3_column_int64(old.p,5)):0,false});continue;
                }
                // Command inbox and legacy single-event write IDs are disjoint.
                Statement inbox(db_,"SELECT 1 FROM command_inbox WHERE tenant=? AND actor=? AND slot=? AND op=?");
                bind_identity(inbox,w.id);inbox.text(4,w.operation_id);
                if(inbox.step()==SQLITE_ROW) {
                    results.push_back({w.operation_id,Status::OperationConflict,0,false});continue;
                }
                std::uint64_t current=version(w.id);
                if(current!=w.expected_version) {
                    results.push_back({w.operation_id,Status::VersionConflict,current,false});continue;
                }
                auto next=current+1;
                Statement state(db_,"INSERT INTO state(tenant,actor,slot,version,snapshot_version,snapshot_schema,snapshot) VALUES(?,?,?,?,?,?,?) ON CONFLICT(tenant,actor,slot) DO UPDATE SET version=excluded.version,snapshot_version=CASE WHEN ?=1 THEN excluded.snapshot_version ELSE state.snapshot_version END,snapshot_schema=CASE WHEN ?=1 THEN excluded.snapshot_schema ELSE state.snapshot_schema END,snapshot=CASE WHEN ?=1 THEN excluded.snapshot ELSE state.snapshot END");
                bind_identity(state,w.id);state.integer(4,next);state.integer(5,w.kind==WriteKind::Snapshot?next:0);state.integer(6,w.kind==WriteKind::Snapshot?w.schema_version:0);
                if(w.kind==WriteKind::Snapshot)state.blob(7,w.payload);else state.null(7);
                const auto is_snapshot=w.kind==WriteKind::Snapshot?1:0;
                state.integer(8,is_snapshot);state.integer(9,is_snapshot);state.integer(10,is_snapshot);state.step();
                if(w.kind==WriteKind::Event) {
                    Statement event(db_,"INSERT INTO events VALUES(?,?,?,?,?,?)");
                    bind_identity(event,w.id);event.integer(4,next);event.integer(5,w.schema_version);event.blob(6,w.payload);event.step();
                }
                Statement op(db_,"INSERT INTO ops VALUES(?,?,?,?,?,?,?,?,?,?)");
                bind_identity(op,w.id);op.text(4,w.operation_id);op.integer(5,w.epoch);
                op.integer(6,w.expected_version);op.integer(7,static_cast<std::uint64_t>(w.kind));op.integer(8,w.schema_version);
                op.blob(9,w.payload);op.integer(10,next);op.step();
                results.push_back({w.operation_id,Status::Committed,next,false});
            }
            commit(); // Only a successful SQLite WAL/FULL commit can authorize durability receipts.
            for(auto& r:results)if(r.applied())r.provider_confirmed_durable=true;
            return results;
        } catch(...) {rollback();throw;} // outcome unknown; caller must retry same op id.
    }
    // The durable command path is additive to the v1 one-event Store API.
    // These host-only methods MUST be called from trusted storage workers,
    // never from an untrusted guest or a blocking actor scheduler carrier.
    struct CommandEvent {
        std::uint32_t schema_version;
        std::vector<std::uint8_t> payload;
    };
    struct CommandReceipt {
        std::uint64_t first_version, last_version;
        std::vector<std::uint8_t> reply;
        bool duplicate;
    };
private:
    static void put_u32(std::vector<std::uint8_t>& out,std::uint32_t n) {
        for(int shift=24;shift>=0;shift-=8)
            out.push_back(static_cast<std::uint8_t>(n >> shift));
    }
    static std::uint32_t get_u32(const std::vector<std::uint8_t>& in,std::size_t& at) {
        if(at>in.size() || in.size()-at<4)
            throw std::logic_error("INVALID_COMMAND_EVENT_BATCH");
        std::uint32_t n=0;
        for(int i=0;i<4;++i)n=(n<<8)|in[at++];
        return n;
    }
    static std::vector<std::uint8_t> encode_events(const std::vector<CommandEvent>& events) {
        if(events.empty()||events.size()>256)
            throw std::invalid_argument("command requires 1..256 events");
        std::vector<std::uint8_t> result;
        result.reserve(1024);
        put_u32(result,static_cast<std::uint32_t>(events.size()));
        for(const auto& e:events){
            if(e.schema_version==0||e.payload.size()>65536||
               result.size()+8+e.payload.size()>1048576)
                throw std::invalid_argument("invalid bounded command event");
            put_u32(result,e.schema_version);
            put_u32(result,static_cast<std::uint32_t>(e.payload.size()));
            result.insert(result.end(),e.payload.begin(),e.payload.end());
        }
        return result;
    }
    static std::vector<CommandEvent> decode_events(const std::vector<std::uint8_t>& in) {
        if(in.size()<12||in.size()>1048576)
            throw std::logic_error("INVALID_COMMAND_EVENT_BATCH");
        std::size_t at=0;
        const auto count=get_u32(in,at);
        if(count<1||count>256)throw std::logic_error("INVALID_COMMAND_EVENT_BATCH");
        std::vector<CommandEvent> out;
        out.reserve(count);
        for(std::uint32_t i=0;i<count;++i){
            auto schema=get_u32(in,at), size=get_u32(in,at);
            if(schema==0||size>65536||at>in.size()||size>in.size()-at)
                throw std::logic_error("INVALID_COMMAND_EVENT_BATCH");
            CommandEvent event{schema,{}};
            event.payload.insert(event.payload.end(),in.begin()+static_cast<std::ptrdiff_t>(at),
                                 in.begin()+static_cast<std::ptrdiff_t>(at+size));
            at+=size;
            out.push_back(std::move(event));
        }
        if(at!=in.size())throw std::logic_error("INVALID_COMMAND_EVENT_BATCH");
        return out;
    }
    static std::vector<std::uint8_t> bounded_blob(sqlite3_stmt* p,int column,
                                                    std::size_t max_size) {
        const auto len=sqlite3_column_bytes(p,column);
        if(sqlite3_column_type(p,column)==SQLITE_NULL||len<0||
           static_cast<std::size_t>(len)>max_size)
            throw std::logic_error("INVALID_COMMAND_RECORD");
        const auto* data=static_cast<const std::uint8_t*>(sqlite3_column_blob(p,column));
        if(len==0)return {};
        if(!data)throw std::logic_error("INVALID_COMMAND_RECORD");
        return {data,data+len};
    }
    // Lookup must be under the SAME existing provider transaction.
    std::optional<CommandReceipt> resolve_command_locked(
            const Identity& id,std::uint64_t current_epoch,const std::string& op_id,
            const std::vector<std::uint8_t>& request) {
        Statement q(db_,"SELECT epoch,expected,length(request),substr(request,1,65537),"
            "first_version,last_version,length(event_batch),substr(event_batch,1,1048577),"
            "length(reply),substr(reply,1,65537) FROM command_inbox "
            "WHERE tenant=? AND actor=? AND slot=? AND op=?");
        bind_identity(q,id);q.text(4,op_id);
        if(q.step()==SQLITE_DONE)return std::nullopt;
        const auto origin=sqlite3_column_int64(q.p,0);
        const auto expected=sqlite3_column_int64(q.p,1);
        const auto first=sqlite3_column_int64(q.p,4);
        const auto last=sqlite3_column_int64(q.p,5);
        const auto request_size=sqlite3_column_int64(q.p,2);
        const auto batch_size=sqlite3_column_int64(q.p,6);
        const auto reply_size=sqlite3_column_int64(q.p,8);
        if(origin<=0||static_cast<std::uint64_t>(origin)>current_epoch ||
           expected<0||expected>=INT64_MAX||first!=expected+1||last<first||
           last-first>=256||static_cast<std::uint64_t>(last)>version(id)||
           request_size!=static_cast<sqlite3_int64>(request.size())||
           batch_size<12||batch_size>1048576||reply_size<0||reply_size>65536)
            throw std::logic_error("OP_ID_CONFLICT");
        auto stored_request=bounded_blob(q.p,3,65536);
        auto stored_batch=bounded_blob(q.p,7,1048576);
        auto reply=bounded_blob(q.p,9,65536);
        if(stored_request!=request)throw std::logic_error("OP_ID_CONFLICT");
        auto events=decode_events(stored_batch);
        if(events.size()!=static_cast<std::size_t>(last-first+1))
            throw std::logic_error("INVALID_COMMAND_EVENT_BATCH");
        Statement journal(db_,"SELECT version,schema,length(payload),substr(payload,1,65537)"
            " FROM events WHERE tenant=? AND actor=? AND slot=? AND version>=?"
            " AND version<=? ORDER BY version ASC");
        bind_identity(journal,id);
        journal.integer(4,static_cast<std::uint64_t>(first));
        journal.integer(5,static_cast<std::uint64_t>(last));
        for(std::size_t i=0;i<events.size();++i) {
            if(journal.step()!=SQLITE_ROW ||
               sqlite3_column_int64(journal.p,0)!=first+static_cast<sqlite3_int64>(i) ||
               sqlite3_column_int64(journal.p,1)!=static_cast<sqlite3_int64>(events[i].schema_version) ||
               sqlite3_column_int64(journal.p,2)!=static_cast<sqlite3_int64>(events[i].payload.size()) ||
               bounded_blob(journal.p,3,65536)!=events[i].payload)
                throw std::logic_error("INCOMPLETE_COMMAND_JOURNAL");
        }
        if(journal.step()==SQLITE_ROW)
            throw std::logic_error("INCOMPLETE_COMMAND_JOURNAL");
        return CommandReceipt{static_cast<std::uint64_t>(first),
                              static_cast<std::uint64_t>(last),std::move(reply),true};
    }
public:
    // Current activation epoch is checked BEFORE reading prior command/reply.
    // Event bytes + original request/reply are compared within one snapshot.
    std::optional<CommandReceipt> resolve_command_fenced(
            const Identity& id,std::uint64_t current_epoch,const std::string& op_id,
            const std::vector<std::uint8_t>& canonical_command) {
        if(!valid_id(id.tenant)||!valid_id(id.actor)||!valid_id(id.slot)||
           !valid_id(op_id)||!current_epoch||current_epoch>INT64_MAX||
           canonical_command.size()>65536)
            throw std::invalid_argument("INVALID_COMMAND_RESOLUTION");
        std::lock_guard<std::mutex> lock(mu_);
        sql("BEGIN");
        try{
            if(epoch(id)!=std::optional<std::uint64_t>(current_epoch))
                throw std::logic_error("STALE_EPOCH");
            auto result=resolve_command_locked(id,current_epoch,op_id,canonical_command);
            sql("COMMIT");
            return result;
        }catch(...){rollback();throw;}
    }
    // One SQLite IMMEDIATE transaction for 1..256 event rows + state head +
    // durable canonical command identity and its original reply. On unknown
    // commit outcome, retry resolution with the identical op ID/request.
    CommandReceipt commit_command(
            const Identity& id,std::uint64_t activation_epoch,
            const std::string& op_id,std::uint64_t expected_version,
            const std::vector<std::uint8_t>& canonical_command,
            const std::vector<CommandEvent>& events,
            const std::vector<std::uint8_t>& reply) {
        if(!valid_id(id.tenant)||!valid_id(id.actor)||!valid_id(id.slot)||
           !valid_id(op_id)||!activation_epoch||activation_epoch>INT64_MAX||
           expected_version>INT64_MAX||canonical_command.size()>65536||
           reply.size()>65536)
            throw std::invalid_argument("INVALID_COMMAND");
        const auto encoded=encode_events(events);
        if(expected_version>static_cast<std::uint64_t>(INT64_MAX)-events.size())
            throw std::invalid_argument("VERSION_EXHAUSTED");
        std::lock_guard<std::mutex> lock(mu_);
        begin();
        try {
            if(epoch(id)!=std::optional<std::uint64_t>(activation_epoch))
                throw std::logic_error("STALE_EPOCH");
            auto old=resolve_command_locked(id,activation_epoch,op_id,canonical_command);
            if(old) {commit();return *old;}
            Statement legacy(db_,"SELECT 1 FROM ops WHERE tenant=? AND actor=? AND slot=? AND op=?");
            bind_identity(legacy,id);legacy.text(4,op_id);
            if(legacy.step()==SQLITE_ROW)throw std::logic_error("OP_ID_CONFLICT");
            const auto current=version(id);
            if(current!=expected_version)throw std::logic_error("VERSION_CONFLICT");
            const auto first=current+1,last=current+events.size();
            Statement head(db_,"INSERT INTO state(tenant,actor,slot,version,snapshot_version,"
                "snapshot_schema,snapshot) VALUES(?,?,?,?,0,0,NULL) ON "
                "CONFLICT(tenant,actor,slot) DO UPDATE SET version=excluded.version");
            bind_identity(head,id);head.integer(4,last);head.step();
            Statement journal(db_,"INSERT INTO events VALUES(?,?,?,?,?,?)");
            for(std::size_t i=0;i<events.size();++i) {
                sqlite3_reset(journal.p);sqlite3_clear_bindings(journal.p);
                bind_identity(journal,id);journal.integer(4,first+i);
                journal.integer(5,events[i].schema_version);
                journal.blob(6,events[i].payload);journal.step();
            }
            Statement inbox(db_,"INSERT INTO command_inbox VALUES(?,?,?,?,?,?,?,?,?,?,?)");
            bind_identity(inbox,id);inbox.text(4,op_id);
            inbox.integer(5,activation_epoch);inbox.integer(6,expected_version);
            inbox.blob(7,canonical_command);inbox.integer(8,first);
            inbox.integer(9,last);inbox.blob(10,encoded);inbox.blob(11,reply);
            inbox.step();
            commit();
            return CommandReceipt{first,last,reply,false};
        }catch(...){rollback();throw;}
    }

    // Trusted host-only event identity reconciliation after an activation
    // restart. A newer epoch may verify prior immutable journal bytes, but
    // this is NOT an original command digest or durable client reply.
    struct VerifiedCommittedEvent {
        std::uint64_t version;
        std::uint32_t schema_version;
        std::vector<std::uint8_t> payload;
    };
    std::optional<VerifiedCommittedEvent> resolve_committed_event_fenced(
        const Identity& id, std::uint64_t current_epoch,
        const std::string& operation_id, std::uint32_t schema_version,
        const std::vector<std::uint8_t>& exact_event_bytes) {
        if (!valid_id(id.tenant) || !valid_id(id.actor) || !valid_id(id.slot) ||
            !valid_id(operation_id) || !current_epoch || current_epoch > INT64_MAX ||
            !schema_version || exact_event_bytes.size() > 1048576)
            throw std::invalid_argument("INVALID_EVENT_RESOLUTION");
        std::lock_guard<std::mutex> lock(mu_);
        sql("BEGIN");
        try {
            if (epoch(id) != std::optional<std::uint64_t>(current_epoch))
                throw std::logic_error("STALE_EPOCH");
            std::optional<VerifiedCommittedEvent> result;
            Statement op(db_,"SELECT epoch,expected,kind,schema,length(payload),"
                "substr(payload,1,1048577),version FROM ops "
                "WHERE tenant=? AND actor=? AND slot=? AND op=?");
            bind_identity(op,id); op.text(4,operation_id);
            if (op.step() == SQLITE_ROW) {
                const auto old_epoch = sqlite3_column_int64(op.p,0);
                const auto expected = sqlite3_column_int64(op.p,1);
                const auto kind = sqlite3_column_int(op.p,2);
                const auto schema = sqlite3_column_int64(op.p,3);
                const auto payload_size = sqlite3_column_int64(op.p,4);
                const auto* saved = static_cast<const std::uint8_t*>(sqlite3_column_blob(op.p,5));
                const auto old_version = sqlite3_column_int64(op.p,6);
                const auto matches = [&] {
                    return old_epoch > 0
                        && static_cast<std::uint64_t>(old_epoch) <= current_epoch
                        && expected >= 0 && expected < INT64_MAX
                        && old_version > 0 && old_version == expected + 1
                        && kind == static_cast<int>(WriteKind::Event)
                        && schema == static_cast<sqlite3_int64>(schema_version)
                        && payload_size == static_cast<sqlite3_int64>(exact_event_bytes.size())
                        && (payload_size == 0 || (saved != nullptr &&
                             std::memcmp(saved,exact_event_bytes.data(),
                                 exact_event_bytes.size()) == 0))
                        && static_cast<std::uint64_t>(old_version) <= version(id);
                };
                if (!matches()) throw std::logic_error("OP_ID_CONFLICT");
                Statement journal(db_,"SELECT schema,length(payload),substr(payload,1,1048577)"
                    " FROM events WHERE tenant=? AND actor=? AND slot=? AND version=?");
                bind_identity(journal,id); journal.integer(4,static_cast<std::uint64_t>(old_version));
                if (journal.step() != SQLITE_ROW
                    || sqlite3_column_int64(journal.p,0) != static_cast<sqlite3_int64>(schema_version)
                    || sqlite3_column_int64(journal.p,1) != payload_size)
                    throw std::logic_error("OPERATION_EVENT_UNAVAILABLE");
                const auto* actual=static_cast<const std::uint8_t*>(
                    sqlite3_column_blob(journal.p,2));
                if (payload_size > 0 && (actual == nullptr
                    || std::memcmp(actual,exact_event_bytes.data(),
                        exact_event_bytes.size()) != 0))
                    throw std::logic_error("OPERATION_EVENT_UNAVAILABLE");
                result = VerifiedCommittedEvent{
                    static_cast<std::uint64_t>(old_version),schema_version,exact_event_bytes};
            }
            sql("COMMIT");
            return result;
        } catch (...) {
            rollback();
            throw;
        }
    }
    // Internal restore: one read transaction binds directory epoch and state.
    // The unfenced overload is privileged host/migration inspection only.
    Recovery load_impl(const Identity& id, std::optional<std::uint64_t> required_epoch) {
        if(!valid_id(id.tenant)||!valid_id(id.actor)||!valid_id(id.slot))
            throw std::invalid_argument("invalid restore identity");
        std::lock_guard<std::mutex> lock(mu_);
        // Reconstruct from one consistent SQLite read snapshot even if another
        // connection is committing a new event or activation concurrently.
        sql("BEGIN");
        try {
        if(required_epoch && epoch(id)!=required_epoch)
            throw std::logic_error("STALE_EPOCH");
        Recovery out;
        Statement q(db_,"SELECT version,snapshot_version,snapshot_schema,snapshot FROM state WHERE tenant=? AND actor=? AND slot=?");
        bind_identity(q,id);
        if(q.step()==SQLITE_ROW) {
            const auto head=sqlite3_column_int64(q.p,0);
            const auto snapshot_head=sqlite3_column_int64(q.p,1);
            const auto snapshot_schema=sqlite3_column_int64(q.p,2);
            const auto len=sqlite3_column_bytes(q.p,3);
            const bool snapshot_null=sqlite3_column_type(q.p,3)==SQLITE_NULL;
            if(head<0 || snapshot_head<0 || snapshot_head>head ||
               snapshot_schema<0 || snapshot_schema>UINT32_MAX || len<0 || len>1048576 ||
               (snapshot_head>0 && (snapshot_schema==0 || snapshot_null)) ||
               (snapshot_head==0 && (snapshot_schema!=0 || !snapshot_null)))
                throw std::runtime_error("INVALID_ACTOR_SNAPSHOT");
            out.version=static_cast<std::uint64_t>(head);
            out.snapshot_version=static_cast<std::uint64_t>(snapshot_head);
            out.snapshot_schema=static_cast<std::uint32_t>(snapshot_schema);
            if(len){const auto* src=static_cast<const std::uint8_t*>(sqlite3_column_blob(q.p,3));out.snapshot.assign(src,src+len);}
        }
        Statement ev(db_,"SELECT version,schema,payload FROM events WHERE tenant=? AND actor=? AND slot=? AND version>? ORDER BY version ASC");
        bind_identity(ev,id);ev.integer(4,out.snapshot_version);
        std::size_t replay_bytes=0;
        while(ev.step()==SQLITE_ROW) {
            const auto version=sqlite3_column_int64(ev.p,0);
            const auto schema=sqlite3_column_int64(ev.p,1);
            const auto len=sqlite3_column_bytes(ev.p,2);
            if(version<1 || schema<1 || schema>UINT32_MAX || len<0 || len>1048576 ||
               sqlite3_column_type(ev.p,2)==SQLITE_NULL ||
               static_cast<std::size_t>(len)>16777216-replay_bytes)
                throw std::runtime_error("INVALID_ACTOR_EVENT");
            replay_bytes+=static_cast<std::size_t>(len);
            Event e;
            e.version=static_cast<std::uint64_t>(version);
            e.schema_version=static_cast<std::uint32_t>(schema);
            if(len){const auto* src=static_cast<const std::uint8_t*>(sqlite3_column_blob(ev.p,2));e.payload.assign(src,src+len);}
            // Reject corrupted, truncated or out-of-order journals; never
            // silently publish an actor state missing an acknowledged event.
            if(e.schema_version==0 || e.version!=out.snapshot_version+out.events.size()+1)
                throw std::runtime_error("INCOMPLETE_ACTOR_JOURNAL");
            out.events.push_back(std::move(e));
            if(out.events.size()>10000)
                throw std::runtime_error("ACTOR_JOURNAL_REPLAY_LIMIT");
        }
        if(out.snapshot_version>out.version ||
           (out.snapshot_version>0 && out.snapshot_schema==0) ||
           out.snapshot_version+out.events.size()!=out.version)
            throw std::runtime_error("INCOMPLETE_ACTOR_JOURNAL");
        sql("COMMIT");
        return out;
        } catch (...) { rollback(); throw; }
    }
    Recovery load(const Identity& id) { return load_impl(id,std::nullopt); }
    Recovery load_fenced(const Identity& id, std::uint64_t trusted_epoch) {
        if(trusted_epoch==0 || trusted_epoch>INT64_MAX)
            throw std::invalid_argument("invalid trusted activation epoch");
        return load_impl(id,trusted_epoch);
    }
};
} // namespace oreslang::runtime::persistence
