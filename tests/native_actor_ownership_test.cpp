#include "oreslang/frontend/Parser.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

static unsigned accepted, rejected;
static std::string source(const std::string& body, const std::string& helpers = "") {
    return helpers + "\ndefine actor Worker as { let total: int = 0; const base: int = 2; "
        "pub fnc on_message(int n): int { " + body + " } }\n";
}
static void admit(const std::string& body, const std::string& helpers = "") {
    try { (void) oreslang::parse_program(source(body, helpers)); ++accepted; }
    catch (const std::exception& e) {
        throw std::runtime_error("unexpected actor rejection: " + body + " : " + e.what());
    }
}
static void deny(const std::string& body, const std::string& diagnostic,
                 const std::string& helpers = "") {
    try { (void) oreslang::parse_program(source(body, helpers)); }
    catch (const std::exception& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos) { ++rejected; return; }
        throw std::runtime_error("wrong actor rejection: " + body + " : " + e.what());
    }
    throw std::runtime_error("unsafe actor admitted: " + body);
}
int main() {
    try {
        const std::string read = "fnc read(borrow int x): int { return x; }";
        const std::string mut = "fnc inspect(borrow mut int x): int { return x; }";
        const std::string pair = "fnc pair(borrow int x, int y): int { return x + y; }";
        const std::string exclusive = "fnc pair(borrow mut int x, int y): int { return x + y; }";
        admit("total = total + n; return total + base;");
        admit("return (rt take n) + 1;");
        admit("total = rt take n; return total;");
        admit("total = (rt take total) + n; return total;");
        admit("total = rt take total; total = total + 1; return total;");
        admit("return read(rt borrow total) + read(rt borrow base);", read);
        admit("return inspect(rt mut borrow n);", mut);
        admit("return pair(rt borrow n, n);", pair);
        admit("return pair(rt mut borrow n, total);", exclusive);
        admit("return pair(x: rt mut borrow n, y: total);", exclusive);
        deny("return (rt take n) + n;", "use of explicitly moved int");
        deny("total = rt take n; return n;", "use of explicitly moved int");
        deny("total = rt take n; total = n; return total;", "use of explicitly moved int");
        deny("total = (rt take total) + total; return total;", "use of explicitly moved int");
        deny("return rt take total;", "left moved at handler return");
        deny("total = rt take base; return total;", "cannot take immutable actor field");
        deny("return inspect(rt mut borrow total);", "actor field mutable borrow", mut);
        deny("return inspect(rt mut borrow base);", "actor field mutable borrow", mut);
        deny("return rt borrow total;", "borrowed reference may only be passed");
        deny("return rt mut borrow n;", "borrowed reference may only be passed");
        deny("return pair(rt mut borrow n, n);", "read conflicts with active mutable borrow", exclusive);
        deny("return pair(rt borrow n, rt take n);", "cannot move int while borrow is active", pair);
        deny("return pair(x: rt mut borrow n, y: n);", "read conflicts with active mutable borrow", exclusive);
        deny("return pair(y: rt take n, x: rt borrow n);", "borrow of explicitly moved int", pair);
        deny("return pair(rt borrow n, 1);", "borrow mode does not match", exclusive);
        deny("return read(rt borrow (n + 1));", "addressable named int", read);
        deny("total = rt take n; return read(rt borrow n);", "borrow of explicitly moved int", read);
        // Distinct ownership paths: Copy/Share retain scalar ownership,
        // multiple shared loans coexist, and a taken field may be restored
        // by the return value of a checked synchronous call.
        admit("return (rt copy n) + n;");
        admit("return (rt share n) + n;");
        admit("return twice(rt borrow n, rt borrow n);",
              "fnc twice(borrow int x, borrow int y): int { return x + y; }");
        admit("total = pair(rt borrow n, rt take total); return total;", pair);
        admit("total = rt take n; return base;");
        // Nested calls must retain earlier outer-argument loans until the
        // outer call returns; their own lexical scope is not permission to
        // reacquire an incompatible alias of the same actor/message place.
        deny("return pair(rt borrow n, inspect(rt mut borrow n));",
             "conflicting active native borrow", pair + mut);
        deny("return pair(rt mut borrow n, read(rt borrow n));",
             "conflicting active native borrow", exclusive + read);
        deny("return twice(rt mut borrow n, rt mut borrow n);",
             "conflicting active native borrow",
             "fnc twice(borrow mut int x, borrow mut int y): int { return x + y; }");
        deny("return pair(rt borrow total, rt take total);", "cannot move int while borrow is active", pair);
        deny("total = (rt take n) + (rt take n); return total;", "use of explicitly moved int");
        deny("return rt take base;", "cannot take immutable actor field");
        std::printf("native actor ownership: %u accepted, %u rejected source programs\n", accepted, rejected);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
