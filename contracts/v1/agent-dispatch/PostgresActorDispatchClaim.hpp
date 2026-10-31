#pragma once
// HOST ONLY. libpq parameterized call into a PostgreSQL SECURITY DEFINER
// function; no guest DB access, no retry after unknown outcomes.
#include "AgentHostDispatchGate.hpp"
#include <libpq-fe.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace oreslang::agent_dispatch {
class PostgresActorDispatchClaim final {
 public:
  explicit PostgresActorDispatchClaim(std::string trusted_conninfo)
      : conninfo_(std::move(trusted_conninfo)) {}
  bool claim_once(const Scope& s) const {
    using Max = std::numeric_limits<std::int64_t>;
    if (!safe_id(s.tenant_id) || !safe_id(s.actor_id) ||
        !safe_id(s.dispatch_id) || !lower_hex(s.context_sha256, 64) ||
        !lower_hex(s.exact_revision, 40) || s.generation == 0 ||
        s.generation > static_cast<std::uint64_t>(Max::max()) ||
        s.epoch == 0 || s.epoch > static_cast<std::uint64_t>(Max::max()) ||
        s.expires_at_ms == 0 ||
        s.expires_at_ms > static_cast<std::uint64_t>(Max::max()) ||
        s.utf8_bytes == 0 || s.utf8_bytes > 65536 ||
        s.measured_tokens == 0 || s.measured_tokens > 16384 ||
        conninfo_.empty()) return false;
    // Override any unbounded/disabled timeout supplied in conninfo or env.
    // These bounds are server-side only for SQL execution; hostile network
    // outages still require isolation from actor scheduler threads.
    const char* keywords[] = {"dbname", "connect_timeout", "options", nullptr};
    const char* connection_values[] = {conninfo_.c_str(), "3",
        "-c statement_timeout=10000 -c lock_timeout=5000", nullptr};
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection(
        PQconnectdbParams(keywords, connection_values, 1), &PQfinish);
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK ||
        PQtransactionStatus(connection.get()) != PQTRANS_IDLE) return false;
    const std::string gen = std::to_string(s.generation);
    const std::string epoch = std::to_string(s.epoch);
    const std::string expiry = std::to_string(s.expires_at_ms);
    const std::string bytes = std::to_string(s.utf8_bytes);
    const std::string tokens = std::to_string(s.measured_tokens);
    const char* values[] = {s.tenant_id.c_str(), s.actor_id.c_str(),
        s.dispatch_id.c_str(), s.context_sha256.c_str(), gen.c_str(),
        epoch.c_str(), s.exact_revision.c_str(), expiry.c_str(),
        bytes.c_str(), tokens.c_str()};
    constexpr char sql[] = "SELECT ores_dispatch.claim_once_v1("
        "$1::text,$2::text,$3::text,$4::text,$5::bigint,$6::bigint,"
        "$7::text,$8::bigint,$9::integer,$10::integer)";
    std::unique_ptr<PGresult, decltype(&PQclear)> result(
        PQexecParams(connection.get(), sql, 10, nullptr, values,
                     nullptr, nullptr, 0), &PQclear);
    // An ambiguous commit or connection loss returns FALSE, never retries.
    return result && PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
        PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1 &&
        !PQgetisnull(result.get(), 0, 0) &&
        std::string(PQgetvalue(result.get(), 0, 0)) == "t";
  }
 private:
  std::string conninfo_;
};
} // namespace oreslang::agent_dispatch
