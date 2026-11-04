#include "oreslang/runtime/McpDevControlM0.hpp"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <limits>

using namespace oreslang::runtime::mcp_m0;
namespace {
int count = 0;
void check_at(bool ok, int line) { ++count; if (!ok) { std::cerr << "failed check " << count << " line " << line << '\n'; std::abort(); } }
#define check(expr) check_at((expr), __LINE__)
struct AuditLog { bool permit=true; int allow=0, deny=0; Reason last=Reason::ok; };
bool audit(Reason reason,bool allowed,void* opaque) noexcept {
    auto& log = *static_cast<AuditLog*>(opaque);
    log.last=reason; if(allowed) ++log.allow; else ++log.deny;
    return log.permit;
}
constexpr std::uint8_t all = bit(Scope::catalog)|bit(Scope::summary)|bit(Scope::metrics)|bit(Scope::upgrade_status);
Host host{true,true,"boot",3,100000,1000};
Principal principal{true,"dev","boot","r","tenant",3,200000,all};
Request request{"resources/read","one","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",kProtocol,kVersion,"r","tenant",
                "atto-dev://runtime/r/tenant/tenant/process/summary",100000,3,256,4};
}
int main() {
    check(kDescriptors.size()==5);
    check(kDescriptors[0].suffix=="process/summary");
    check(kDescriptors[0].id==stable_id("atto.mcp.dev-control.m0.v1/process/summary"));
    check(kFrameBytes==32768 && kResponseBytes==65536 && kJsonDepth==12);
    check(kPerSecond==8 && kPerMinute==60 && kInflight==2 && kPrincipals==64);
    check(!identifier("..") && !identifier(".") && !identifier("bad/") && identifier("tenant"));
    check(nonce_valid(request.nonce) && !nonce_valid("deadbeef"));
    Descriptor descriptor{};
    check(uri_match(request.uri,"r","tenant",descriptor) && descriptor.scope==Scope::summary);
    check(!uri_match("atto-dev://runtime/../tenant/tenant/process/summary","r","tenant",descriptor));
    check(!uri_match("atto-dev://runtime/r/tenant/tenant/%2e%2e","r","tenant",descriptor));
    AuditLog log;
    auto ledger = std::make_unique<Ledger>(audit,&log);
    auto h=host; h.enabled=false;
    check(ledger->begin(h,principal,request).reason==Reason::disabled);
    h=host; h.peer_verified=false;
    check(ledger->begin(h,principal,request).reason==Reason::unauthenticated);
    auto p=principal; p.scopes=bit(Scope::catalog);
    check(ledger->begin(host,p,request).reason==Reason::scope);
    p=principal; p.tenant="other";
    check(ledger->begin(host,p,request).reason==Reason::tenant);
    p=principal; p.generation=4;
    check(ledger->begin(host,p,request).reason==Reason::generation);
    p=principal; p.expires_at_ms=100000;
    check(ledger->begin(host,p,request).reason==Reason::unauthenticated);
    auto r=request; r.method="guest/invoke";
    check(ledger->begin(host,principal,r).reason==Reason::unsupported);
    r=request; r.expected_generation=2;
    check(ledger->begin(host,principal,r).reason==Reason::generation);
    r=request; r.claimed_tenant="other";
    check(ledger->begin(host,principal,r).reason==Reason::tenant);
    r=request; r.schema="v2";
    check(ledger->begin(host,principal,r).reason==Reason::version);
    r=request; r.frame_bytes=32769;
    check(ledger->begin(host,principal,r).reason==Reason::overflow);
    r=request; r.json_depth=13;
    check(ledger->begin(host,principal,r).reason==Reason::overflow);
    r=request; r.nonce="bad";
    check(ledger->begin(host,principal,r).reason==Reason::malformed);
    // The above denied attempts consumed the rate window, as on the JS model.
    check(ledger->begin(host,principal,request).reason==Reason::quota);
    host.monotonic_ms+=1000;
    auto first=ledger->begin(host,principal,request);
    check(first.ok() && first.ticket.token);
    check(ledger->live(first.ticket,host));
    check(ledger->begin(host,principal,request).reason==Reason::replay);
    r=request; r.nonce="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    check(ledger->begin(host,principal,r).reason==Reason::duplicate_id);
    r.id="two";
    auto second=ledger->begin(host,principal,r);
    check(second.ok());
    r.id="three";r.nonce="cccccccccccccccccccccccccccccccc";
    check(ledger->begin(host,principal,r).reason==Reason::quota);
    check(ledger->finish(first.ticket));
    check(!ledger->finish(first.ticket));
    check(ledger->begin(host,principal,request).reason==Reason::replay);
    check(ledger->finish(second.ticket));
    auto otherHost=host;otherHost.generation=4;
    check(!ledger->live(second.ticket,otherHost));
    r=request;r.id="four";r.nonce="dddddddddddddddddddddddddddddddd";
    auto third=ledger->begin(host,principal,r);
    check(third.ok());
    check(!ledger->live(third.ticket,otherHost));
    otherHost=host;otherHost.monotonic_ms+=501;
    check(!ledger->live(third.ticket,otherHost));
    otherHost=host;otherHost.wall_ms=200000;
    check(!ledger->live(third.ticket,otherHost));
    check(ledger->finish(third.ticket));
    log.permit=false;
    r.id="five";r.nonce="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    check(ledger->begin(host,principal,r).reason==Reason::audit_failure);
    log.permit=true;
    host.monotonic_ms+=1000;
    check(ledger->begin(host,principal,r).ok());
    check(log.allow>0 && log.deny>0);
    // The standard JSON-RPC integer ID "1" and string ID "1" must not alias.
    {
        AuditLog a; auto bound = std::make_unique<Ledger>(audit,&a);
        auto h=host; h.monotonic_ms=20000;
        auto t=request; t.id="1"; t.numeric_id=false;
        auto one=bound->begin(h,principal,t);check(one.ok());
        t.numeric_id=true;t.nonce="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        auto two=bound->begin(h,principal,t);check(two.ok());
        check(one.ticket.token != two.ticket.token);
        check(bound->finish(one.ticket));check(bound->finish(two.ticket));
        t.nonce="cccccccccccccccccccccccccccccccc";t.id="01";
        check(bound->begin(h,principal,t).reason==Reason::malformed);
        t.id="9007199254740992";
        check(bound->begin(h,principal,t).reason==Reason::malformed);
        t.id="12e3";
        check(bound->begin(h,principal,t).reason==Reason::malformed);
    }
    // Classification preserves tenant/runtime boundaries, not generic parse errors.
    {
        Descriptor out{};
        check(classify_uri("atto-dev://runtime/other/tenant/tenant/process/summary",
                           "r","tenant",out)==Reason::runtime);
        check(classify_uri("atto-dev://runtime/r/tenant/other/process/summary",
                           "r","tenant",out)==Reason::tenant);
        check(classify_uri("atto-dev://runtime/r/tenant/../process/summary",
                           "r","tenant",out)==Reason::malformed);
    }
    // Successful requests as well as denials share the rolling admission limit.
    {
        AuditLog a; auto bound=std::make_unique<Ledger>(audit,&a);
        auto h=host;h.monotonic_ms=1000;
        for(int i=0;i<60;i++) {
            h.monotonic_ms=1000+static_cast<std::uint64_t>(i/8)*1000;
            auto q=request;
            const std::string id=std::to_string(i);
            std::string nonce(32,'0');nonce[31]=static_cast<char>('a'+(i%6));
            // Nonces must be unique; use a deterministic 32-byte hexadecimal suffix.
            const char hex[]="0123456789abcdef";
            nonce[29]=hex[(i >> 8)&15];nonce[30]=hex[(i >> 4)&15];nonce[31]=hex[i&15];
            q.id=id;q.nonce=nonce;
            const auto accepted=bound->begin(h,principal,q);
            check(accepted.ok());
            check(bound->finish(accepted.ticket));
        }
        h.monotonic_ms=9000;
        auto q=request;q.id="after";q.nonce="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
        check(bound->begin(h,principal,q).reason==Reason::quota);
        h.monotonic_ms=61000;
        check(bound->begin(h,principal,q).ok());
    }
    // Never evict live tickets or unexpired nonce/rate evidence at 64 principals.
    {
        AuditLog a;auto bound=std::make_unique<Ledger>(audit,&a);
        auto h=host;h.monotonic_ms=1000;
        for(int i=0;i<64;i++) {
            auto p=principal;
            const std::string name="p"+std::to_string(i);p.principal=name;
            const auto res=bound->begin(h,p,request);
            check(res.ok());
            check(bound->finish(res.ticket));
        }
        auto p=principal;p.principal="overflow";
        check(bound->begin(h,p,request).reason==Reason::quota);
        h.monotonic_ms=61000;
        check(bound->begin(h,p,request).ok());
    }
    // Linearization under native C++ threads: exactly one same nonce may admit.
    {
        AuditLog a;auto bound=std::make_unique<Ledger>(audit,&a);
        auto h=host;h.monotonic_ms=1000;
        std::atomic<int> admitted{0};
        std::vector<std::thread> threads;
        threads.reserve(16);
        for(int i=0;i<16;i++) threads.emplace_back([&]{
            if(bound->begin(h,principal,request).ok()) admitted.fetch_add(1);
        });
        for(auto& t:threads)t.join();
        check(admitted.load()==1);
    }
    // No signed / unsigned wrap should admit malformed future timestamps.
    {
        AuditLog a;auto bound=std::make_unique<Ledger>(audit,&a);
        auto h=host;h.wall_ms=std::numeric_limits<std::uint64_t>::max()-10;
        auto p=principal;p.expires_at_ms=std::numeric_limits<std::uint64_t>::max();
        auto q=request;q.issued_at_ms=std::numeric_limits<std::uint64_t>::max()-5;
        check(bound->begin(h,p,q).ok());
        q.issued_at_ms=10;q.nonce="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
        check(bound->begin(h,p,q).reason==Reason::malformed);
    }
    // A genuinely absent audit sink cannot mint tickets.
    {
        auto bound=std::make_unique<Ledger>(nullptr,nullptr);
        check(bound->begin(host,principal,request).reason==Reason::audit_failure);
    }
    std::cout << "MCP M0 LLVM native host policy passed " << count << " checks (not guest parity)\n";
}
