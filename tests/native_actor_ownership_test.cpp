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
        admit("return inspect(rt borrow mut n);", mut);
        admit("return pair(rt borrow n, n);", pair);
        admit("return pair(rt borrow mut n, total);", exclusive);
        admit("return pair(x: rt borrow mut n, y: total);", exclusive);
        deny("return (rt take n) + n;", "use of explicitly moved int");
        deny("total = rt take n; return n;", "use of explicitly moved int");
        deny("total = rt take n; total = n; return total;", "use of explicitly moved int");
        deny("total = (rt take total) + total; return total;", "use of explicitly moved int");
        deny("return rt take total;", "left moved at handler return");
        deny("total = rt take base; return total;", "cannot take immutable actor field");
        deny("return inspect(rt borrow mut total);", "actor field mutable borrow", mut);
        deny("return inspect(rt borrow mut base);", "actor field mutable borrow", mut);
        deny("return rt borrow total;", "borrowed reference may only be passed");
        deny("return rt borrow mut n;", "borrowed reference may only be passed");
        deny("return pair(rt borrow mut n, n);", "read conflicts with active mutable borrow", exclusive);
        deny("return pair(rt borrow n, rt take n);", "cannot move int while borrow is active", pair);
        deny("return pair(x: rt borrow mut n, y: n);", "read conflicts with active mutable borrow", exclusive);
        deny("return pair(y: rt take n, x: rt borrow n);", "borrow of explicitly moved int", pair);
        deny("return pair(rt borrow n, 1);", "borrow mode does not match", exclusive);
        deny("return read(rt borrow (n + 1));", "addressable named int", read);
        deny("total = rt take n; return read(rt borrow n);", "borrow of explicitly moved int", read);
        // The skipped-handler bug applied to every field name, independent
        // of declaration order. Exercise distinct places and writeback paths.
        for (int i = 0; i < 32; ++i) {
            const auto constant = std::to_string(i);
            admit("total = (rt take total) + " + constant + "; return total;");
            deny("total = (rt take n) + " + constant + "; return n;", "use of explicitly moved int");
        }
        std::printf("native actor ownership: %u accepted, %u rejected source programs\n", accepted, rejected);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what()); return 1;
    }
}
