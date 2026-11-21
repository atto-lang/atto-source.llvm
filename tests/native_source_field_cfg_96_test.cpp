#include "oreslang/types/OwnershipSourceFieldFlow.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang;
namespace own = oreslang::ownership;
constexpr std::uint64_t kTicketDomain = std::uint64_t{1} << 63;
void check(bool valid, const char* message, int& total) {
    ++total;
    if (!valid)
        throw std::runtime_error(std::string("mutation ") +
            std::to_string(total) + ": " + message);
}
std::unique_ptr<Expression> make_int(int value = 1) {
    auto e=std::make_unique<Expression>();
    e->kind=Expression::Kind::Integer;
    e->integer=value;
    return e;
}
std::unique_ptr<Expression> make_field(const std::string& field) {
    auto e=std::make_unique<Expression>();
    e->kind=Expression::Kind::Field;
    e->name=field;
    e->left=std::make_unique<Expression>();
    e->left->kind=Expression::Kind::Variable;
    e->left->name="p";
    return e;
}
Statement field_write(const std::string& field, std::uint64_t ticket) {
    Statement s;
    s.kind=Statement::Kind::FieldWrite;
    s.receiver="p";s.field=field;
    s.expression=make_int();
    s.reinitialize_write_id=ticket;
    return s;
}
Statement field_take(const std::string& field) {
    Statement s;
    s.kind=Statement::Kind::ScalarWrite;
    s.receiver="temporary";
    s.expression=std::make_unique<Expression>();
    s.expression->kind=Expression::Kind::Take;
    s.expression->left=make_field(field);
    return s;
}
Statement field_read(const std::string& field) {
    Statement s;
    s.kind=Statement::Kind::ScalarWrite;
    s.receiver="temporary";s.expression=make_field(field);
    return s;
}
Statement conditional(std::vector<Statement> yes,
        std::vector<Statement> no) {
    Statement s;
    s.kind=Statement::Kind::Conditional;
    s.condition=make_int(1);
    s.then_statements=std::move(yes);
    s.else_statements=std::move(no);
    return s;
}
Statement loop(std::vector<Statement> body) {
    Statement s;
    s.kind=Statement::Kind::ForIndex;
    s.branch_local=std::make_unique<LocalBinding>();
    s.branch_local->name="i";
    s.branch_local->initializer=make_int(0);
    s.condition=make_int(1);
    s.loop_step=make_int(1);
    s.then_statements=std::move(body);
    return s;
}
Statement source_return(std::unique_ptr<Expression> result) {
    Statement s;s.kind=Statement::Kind::Return;
    s.expression=std::move(result);
    return s;
}
std::vector<Statement> one(Statement stmt) {
    std::vector<Statement> v;v.push_back(std::move(stmt));return v;
}
Function program() {
    Function f;f.name="field_dominance";
    return f;
}
bool admitted(const Function& f, const std::string& field,
        bool initial_moved=false) {
    own::State entry;
    entry.emplace("p."+field,own::PlaceState{
        initial_moved ? own::kMoved : own::kOwned});
    try {
        const auto proof=own::verify_source_field_flow(f,
            std::move(entry),std::set<std::string>{"p"});
        if (proof.source_loan_use_complete ||
                proof.optimized_llvm_dominance_verified ||
                proof.guest_verified ||
                proof.reinitialization_sites.empty())
            throw std::runtime_error("ownership violation: illegal proof elevation");
        return true;
    } catch (const std::runtime_error& e) {
        const std::string text=e.what();
        if (text.find("ownership violation:")==std::string::npos)
            throw;
        return false;
    }
}
int main() {
    int count=0;
    try {
        // Sixteen individually named positive and negative source-CFG
        // mutations, including branch joins, loop backedges and exit edges.
        {
            auto f=program();
            f.statements.push_back(field_take("x"));
            f.statements.push_back(field_write("x",kTicketDomain|1));
            f.result=make_field("x");
            check(admitted(f,"x"),"move->restore->read",count);
        }
        {
            auto f=program();f.statements.push_back(field_take("x"));
            f.result=make_field("x");
            check(!admitted(f,"x"),"read after move without ticket",count);
        }
        {
            auto f=program();
            f.statements.push_back(field_write("x",0));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"missing ticket",count);
        }
        {
            auto f=program();
            f.statements.push_back(field_write("x",2));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"wrong domain",count);
        }
        {
            auto f=program();
            f.statements.push_back(field_write("x",kTicketDomain|4097));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"ticket beyond quota",count);
        }
        {
            auto f=program();
            f.statements.push_back(conditional(
                one(field_write("x",kTicketDomain|1)),
                one(field_write("x",kTicketDomain|1))));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"duplicate tickets across branches",count);
        }
        {
            auto f=program();
            f.statements.push_back(conditional(
                one(field_write("x",kTicketDomain|1)),{}));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"restoration only on then predecessor",count);
        }
        {
            auto f=program();
            f.statements.push_back(conditional(
                one(field_write("x",kTicketDomain|1)),
                one(field_write("x",kTicketDomain|2))));
            f.result=make_field("x");
            check(admitted(f,"x",true),"distinct restorations cover both paths",count);
        }
        {
            auto f=program();
            f.statements.push_back(conditional(
                one(source_return(make_int())),
                one(field_write("x",kTicketDomain|1))));
            f.result=make_field("x");
            check(admitted(f,"x",true),"terminal predecessor need not restore",count);
        }
        {
            auto f=program();
            std::vector<Statement> body;
            body.push_back(field_take("x"));
            body.push_back(field_write("x",kTicketDomain|1));
            f.statements.push_back(loop(std::move(body)));
            f.result=make_field("x");
            check(admitted(f,"x"),"loop preserves owned state at backedge",count);
        }
        {
            auto f=program();
            f.statements.push_back(loop(one(field_write("x",kTicketDomain|1))));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"zero-trip loop cannot restore",count);
        }
        {
            auto f=program();
            f.statements.push_back(field_write("y",kTicketDomain|1));
            f.result=make_field("x");
            check(!admitted(f,"x",true),"wrong projected field",count);
        }
        {
            auto f=program();
            Statement s;s.kind=Statement::Kind::Call;
            s.expression=std::make_unique<Expression>();
            s.expression->kind=Expression::Kind::Call;
            f.statements.push_back(std::move(s));
            f.statements.push_back(field_write("x",kTicketDomain|1));
            check(!admitted(f,"x",true),"opaque call denied",count);
        }
        {
            auto f=program();
            Statement s;s.kind=Statement::Kind::ScalarWrite;
            s.expression=std::make_unique<Expression>();
            s.expression->kind=Expression::Kind::Borrow;
            s.expression->left=make_field("x");
            f.statements.push_back(std::move(s));
            f.statements.push_back(field_write("x",kTicketDomain|1));
            check(!admitted(f,"x",true),"unclassified borrowed-use denied",count);
        }
        {
            auto f=program();
            f.statements.push_back(source_return(make_int()));
            f.statements.push_back(field_write("x",kTicketDomain|1));
            check(!admitted(f,"x",true),"unreachable minted write denied",count);
        }
        {
            auto f=program();
            f.statements.push_back(field_write("x",kTicketDomain|1));
            f.result=make_field("x");
            check(admitted(f,"x",true),"initially moved direct restore",count);
        }

        // Eighty deterministic property mutations, ten distinct projections
        // by eight path shapes. Every adversarial condition has an oracle.
        for (int i=0;i<80;++i) {
            const std::string field="field"+std::to_string(i/8);
            const int shape=i%8;
            auto f=program();
            bool expect=false, initial_moved=false;
            switch(shape) {
                case 0:
                    f.statements.push_back(field_take(field));
                    f.statements.push_back(field_write(field,kTicketDomain|1));
                    expect=true;
                    break;
                case 1:
                    f.statements.push_back(field_take(field));
                    f.statements.push_back(field_write(field,0));
                    break;
                case 2:
                    initial_moved=true;
                    f.statements.push_back(field_write(field,kTicketDomain|2));
                    expect=true;
                    break;
                case 3:
                    initial_moved=true;
                    f.statements.push_back(conditional(
                        one(field_write(field,kTicketDomain|1)),{}));
                    break;
                case 4:
                    initial_moved=true;
                    f.statements.push_back(conditional(
                        one(field_write(field,kTicketDomain|1)),
                        one(field_write(field,kTicketDomain|2))));
                    expect=true;
                    break;
                case 5: {
                    std::vector<Statement> body;
                    body.push_back(field_take(field));
                    body.push_back(field_write(field,kTicketDomain|1));
                    f.statements.push_back(loop(std::move(body)));
                    expect=true;
                    break;
                }
                case 6:
                    initial_moved=true;
                    f.statements.push_back(loop(
                        one(field_write(field,kTicketDomain|1))));
                    break;
                case 7:
                    f.statements.push_back(field_write(field,kTicketDomain|1));
                    break;
            }
            f.result=make_field(field);
            check(admitted(f,field,initial_moved)==expect,
                "deterministic field-path/loop/branch mutation",count);
        }
        if(count!=96)
            throw std::runtime_error("unexpected source-CFG mutation count");
        std::cout<<"PASS 96/96 compiler-source field-CFG path mutations "
                   "(HOST_CPP17, NOT GUEST SSA/DOMINATORS)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<"FAIL at "<<count<<": "<<e.what()<<'\n';return 1;
    }
}
