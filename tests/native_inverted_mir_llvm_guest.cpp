// Standalone C++17 compiler-MIR to checked LLVM IR demonstration.
// Uses only fixed Pair<i64,i64> with local no-escape row indices.
#include "oreslang/types/InvertedMirLlvmI64.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
int main(int argc, char** argv) {
  if (argc != 3) { std::cerr << "Usage: mir_llvm_guest <positive|closed|bounds|unclosed|immutable|missing-operand> <out.ll>\n";return 2; }
  try {
    using namespace oreslang; using namespace oreslang::dop::hir; using namespace oreslang::dop::mir;
    InvertedStructDecl decl; decl.name = "Pair";
    decl.fields = {{"x",InvertedScalarKind::I64,true,0},{"id",InvertedScalarKind::I64,false,0}};
    const auto layout = Layout::from_decl(decl);
    const std::string mode(argv[1]); LinearPlan plan(layout,2);
    I64PairIrOperands operands;
    if (mode == "positive") {
      plan.push(2); plan.push(2);
      plan.borrow_read("id"); plan.release_read("id");
      plan.write_runtime_checked(1,"x"); plan.erase_swap(0);
      plan.read_runtime_checked(0,"x"); plan.close();
      operands.rows = {{{11,501},{21,502}}}; operands.writes = {42};
    } else if (mode == "closed") {
      // A detached SSA result is returned after revocation and volatile
      // teardown, with all physical payload/metadata slots overwritten.
      plan.push(2); plan.read_runtime_checked(0, "x"); plan.close();
      operands.rows = {{{42, 501}}};
    } else if (mode == "bounds") {
      plan.push(2);plan.read_runtime_checked(1,"x"); plan.close();
      operands.rows = {{{11,501}}};
    } else if (mode == "unclosed") {
      // A valid open MIR cannot become a returned standalone guest main.
      plan.push(2); plan.read_runtime_checked(0,"x");
      operands.rows = {{{42,501}}};
    } else if (mode == "immutable") {
      plan.push(2); plan.write_runtime_checked(0,"id");
    } else if (mode == "missing-operand") {
      plan.push(2); plan.close();
    } else { throw std::invalid_argument("unknown MIR fixture"); }
    const auto ir = emit_native_i64_pair_ir(Module::lower(plan), operands);
    std::ofstream output(argv[2]); output << ir;
    if (!output) throw std::runtime_error("could not write generated IR");
    return 0;
  } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 3; }
}
