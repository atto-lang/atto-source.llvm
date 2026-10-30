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
    Recovery load(const Identity& id) {
        if(!valid_id(id.tenant)||!valid_id(id.actor)||!valid_id(id.slot))
            throw std::invalid_argument("invalid restore identity");
        std::lock_guard<std::mutex> lock(mu_);
        // Reconstruct from one consistent SQLite read snapshot even if another
        // connection is committing a new event or activation concurrently.
        sql("BEGIN");
        try {
        Recovery out;
        Statement q(db_,"SELECT version,snapshot_version,snapshot_schema,snapshot FROM state WHERE tenant=? AND actor=? AND slot=?");
        bind_identity(q,id);
        if(q.step()==SQLITE_ROW) {
            out.version=static_cast<std::uint64_t>(sqlite3_column_int64(q.p,0));
            out.snapshot_version=static_cast<std::uint64_t>(sqlite3_column_int64(q.p,1));
            out.snapshot_schema=static_cast<std::uint32_t>(sqlite3_column_int(q.p,2));
            const auto len=sqlite3_column_bytes(q.p,3);
            if(len){const auto* src=static_cast<const std::uint8_t*>(sqlite3_column_blob(q.p,3));out.snapshot.assign(src,src+len);}
        }
        Statement ev(db_,"SELECT version,schema,payload FROM events WHERE tenant=? AND actor=? AND slot=? AND version>? ORDER BY version ASC");
        bind_identity(ev,id);ev.integer(4,out.snapshot_version);
        while(ev.step()==SQLITE_ROW) {
            Event e;
            e.version=static_cast<std::uint64_t>(sqlite3_column_int64(ev.p,0));
            e.schema_version=static_cast<std::uint32_t>(sqlite3_column_int(ev.p,1));
            const auto len=sqlite3_column_bytes(ev.p,2);
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
};
} // namespace oreslang::runtime::persistence
