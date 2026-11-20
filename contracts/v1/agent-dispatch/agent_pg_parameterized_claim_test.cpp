// Exact production libpq adapter, exercised with a fake transport boundary.
// HOST ONLY. Does not prove a real PostgreSQL commit or durability.
#include "PostgresActorDispatchClaim.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
using namespace oreslang::agent_dispatch;
namespace {
char connection_storage, result_storage;
PGconn* fake_conn = reinterpret_cast<PGconn*>(&connection_storage);
PGresult* fake_result = reinterpret_cast<PGresult*>(&result_storage);
int connects=0, executes=0, closes=0, clears=0;
enum class Sim { admitted, denied, db_error, connection_bad, transaction_open } mode=Sim::admitted;
}
extern "C" PGconn* __wrap_PQconnectdbParams(const char* const* keys, const char* const* vals, int expanded) {
  ++connects;
  assert(expanded==1);
  assert(std::strcmp(keys[0], "dbname")==0);
  assert(std::strstr(vals[0], "host=127.0.0.1")!=nullptr);
  assert(std::strcmp(keys[1], "connect_timeout")==0 && std::strcmp(vals[1],"3")==0);
  assert(std::strcmp(keys[2], "options")==0);
  assert(std::strstr(vals[2], "statement_timeout=10000")!=nullptr);
  assert(std::strstr(vals[2], "lock_timeout=5000")!=nullptr);
  assert(keys[3]==nullptr && vals[3]==nullptr);
  return fake_conn;
}
extern "C" ConnStatusType __wrap_PQstatus(const PGconn* conn) {
  assert(conn==fake_conn);
  return mode==Sim::connection_bad ? CONNECTION_BAD : CONNECTION_OK;
}
extern "C" PGTransactionStatusType __wrap_PQtransactionStatus(const PGconn* conn) {
  assert(conn==fake_conn);
  return mode==Sim::transaction_open ? PQTRANS_INTRANS : PQTRANS_IDLE;
}
extern "C" PGresult* __wrap_PQexecParams(PGconn* conn,const char* sql,int n,const Oid* types,
    const char* const* values,const int* lens,const int* fmts,int format){
  assert(conn==fake_conn);++executes;
  assert(std::strcmp(sql, "SELECT ores_dispatch.claim_once_v1($1::text,$2::text,$3::text,$4::text,$5::bigint,$6::bigint,$7::text,$8::bigint,$9::integer,$10::integer)")==0);
  assert(n==10 && types==nullptr && lens==nullptr && fmts==nullptr && format==0);
  const char* expected[]={"tenant","worker","dispatch",nullptr,"7","9",nullptr,"9999999999999","1024","24"};
  for(int i=0;i<10;++i)if(expected[i])assert(std::strcmp(values[i],expected[i])==0);
  assert(std::strlen(values[3])==64 && std::strspn(values[3],"b")==64);
  assert(std::strlen(values[6])==40 && std::strspn(values[6],"a")==40);
  return mode==Sim::db_error ? nullptr : fake_result;
}
extern "C" ExecStatusType __wrap_PQresultStatus(const PGresult* r){assert(r==fake_result);return PGRES_TUPLES_OK;}
extern "C" int __wrap_PQntuples(const PGresult* r){assert(r==fake_result);return 1;}
extern "C" int __wrap_PQnfields(const PGresult* r){assert(r==fake_result);return 1;}
extern "C" int __wrap_PQgetisnull(const PGresult* r,int row,int column){assert(r==fake_result&&row==0&&column==0);return 0;}
extern "C" char* __wrap_PQgetvalue(const PGresult* r,int row,int column){
  assert(r==fake_result&&row==0&&column==0);
  static char yes[]="t", no[]="f";
  return mode==Sim::admitted ? yes : no;
}
extern "C" void __wrap_PQclear(PGresult* r){assert(r==fake_result);++clears;}
extern "C" void __wrap_PQfinish(PGconn* c){assert(c==fake_conn);++closes;}
int main(){
  Scope s{"tenant","worker",std::string(40,'a'),"dispatch",std::string(64,'b'),7,9,9999999999999ULL,1024,24};
  PostgresActorDispatchClaim broker("host=127.0.0.1 connect_timeout=0");
  assert(broker.claim_once(s));assert(connects==1&&executes==1&&closes==1&&clears==1);
  mode=Sim::denied;assert(!broker.claim_once(s));assert(connects==2&&executes==2&&closes==2&&clears==2);
  mode=Sim::db_error;assert(!broker.claim_once(s));assert(executes==3&&connects==3&&closes==3&&clears==2); // No retry
  mode=Sim::connection_bad;assert(!broker.claim_once(s));assert(executes==3&&closes==4);
  mode=Sim::transaction_open;assert(!broker.claim_once(s));assert(executes==3&&closes==5);
  s.context_sha256="not-hex";assert(!broker.claim_once(s));assert(connects==5); // Reject before DB
  std::cout<<"LIBPQ_PARAMETERIZED_HOST_CONTRACT_PASS; database_executed=false\n";
}
