#include "oreslang/runtime/HttpResponseFrameGuard.hpp"

#include <stdexcept>
#include <string_view>

using oreslang::HttpResponseFrameGuard;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while(false)

int main() {
    using Guard = HttpResponseFrameGuard;
    using A = Guard::Admission;
    Guard html{"actor-1", "req-1", 128, 512};
    CHECK(html.valid());
    CHECK(html.grant_credit(128) == A::Ok);
    CHECK(html.begin("forged", "req-1", 0, 200) == A::Unauthorized);
    CHECK(html.begin("actor-1", "other", 0, 200) == A::Unauthorized);
    CHECK(html.begin("actor-1", "req-1", 0, 200) == A::Ok);
    CHECK(Guard::valid_guest_header("content-type", "text/html; charset=utf-8"));
    CHECK(!Guard::valid_guest_header("transfer-encoding", "chunked"));
    CHECK(!Guard::valid_guest_header("content-type", "text/html\r\nsmuggled: yes"));
    CHECK(html.chunk("actor-1", "req-1", 1, 64) == A::Ok);
    CHECK(html.total_bytes() == 64 && html.remaining_credit() == 64);
    CHECK(html.chunk("actor-1", "req-1", 1, 10) == A::StaleSequence);
    CHECK(html.chunk("actor-1", "req-1", 2, 65) == A::CreditExceeded);
    CHECK(html.chunk("actor-1", "req-1", 2, 64) == A::Ok);
    CHECK(html.chunk("actor-1", "req-1", 3, 1) == A::CreditExceeded);
    CHECK(html.end("actor-1", "req-1", 3) == A::Ok);
    CHECK(html.state() == Guard::State::Ended);
    CHECK(html.chunk("actor-1", "req-1", 4, 1) == A::InvalidOrder);
    html.abort();
    CHECK(html.state() == Guard::State::Ended);

    Guard json{"actor-2", "req-2", 128, 512};
    CHECK(json.valid());
    CHECK(json.begin("actor-2", "req-2", 22, 200) == A::Ok);
    CHECK(json.grant_credit(20) == A::Ok);
    CHECK(json.chunk("actor-2", "req-2", 23, 11) == A::Ok);
    CHECK(json.end("actor-2", "req-2", 24) == A::Ok);
    CHECK(json.total_bytes() == 11);

    Guard no_body{"actor-3", "req-3", 100, 500};
    CHECK(no_body.begin("actor-3", "req-3", 0, 205) == A::Ok);
    CHECK(no_body.grant_credit(50) == A::Ok);
    CHECK(no_body.chunk("actor-3", "req-3", 1, 1) == A::InvalidOrder);
    CHECK(no_body.end("actor-3", "req-3", 1) == A::Ok);

    Guard exhausted{"actor-4", "req-4", 2, 3};
    CHECK(exhausted.grant_credit(3) == A::Ok);
    CHECK(exhausted.begin("actor-4", "req-4", 0, 200) == A::Ok);
    CHECK(exhausted.chunk("actor-4", "req-4", 1, 3) == A::InvalidBody);
    CHECK(exhausted.chunk("actor-4", "req-4", 1, 2) == A::Ok);
    CHECK(exhausted.chunk("actor-4", "req-4", 2, 2) == A::CreditExceeded);
    exhausted.abort();
    CHECK(exhausted.state() == Guard::State::Aborted);
    CHECK(exhausted.end("actor-4", "req-4", 3) == A::InvalidOrder);

    Guard malicious{"actor-5", "req-5\n", 10, 100};
    CHECK(!malicious.valid());
    CHECK(malicious.begin("actor-5", "req-5\n", 0, 200) == A::Unauthorized);
    return 0;
}
