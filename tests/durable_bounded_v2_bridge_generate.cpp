#include "DurableBoundedV2Emit.hpp"
#include <cassert>
#include <iostream>
int main(){using oreslang::emit_bounded_v2_bridge;using oreslang::BoundedFieldV2;
 const std::vector<BoundedFieldV2> f{{"count",1},{"open",2},{"name",3},{"payload",4}};
 auto code=emit_bounded_v2_bridge("Ledger",f);assert(code==emit_bounded_v2_bridge("Ledger",f));
 for(const auto& bad:std::vector<std::vector<BoundedFieldV2>>{{{"x",1},{"x",2}},{{"bad name",3}},{{"x",255}}}){bool rejected=false;try{(void)emit_bounded_v2_bridge("Ledger",bad);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);}
 std::cout<<code;
}
