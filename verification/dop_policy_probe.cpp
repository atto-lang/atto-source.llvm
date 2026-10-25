#include "oreslang/types/DopLogicalAccess.hpp"
#include <iostream>
#include <sstream>
#include <functional>
using namespace oreslang::types::dop_access;
std::optional<std::string> optional(const std::string& s) {
    if (s == "-") return std::nullopt;
    std::string value;
    for (std::size_t i = 0; i < s.size(); i += 2)
        value.push_back(static_cast<char>(std::stoi(s.substr(i,2),nullptr,16)));
    return value;
}
Loan loan(const std::vector<std::string>& s, std::size_t i) {
    return {Place(static_cast<oreslang::dop::Layout>(std::stoi(s[i])), *optional(s[i+1]),
        optional(s[i+2]), optional(s[i+3]), optional(s[i+4]),
        s[i+5] == "-" ? std::nullopt : std::optional<Range>(Range(std::stoull(s[i+5]), std::stoull(s[i+6]))),
        std::stoull(s[i+7])), static_cast<Access>(std::stoi(s[i+8]))};
}
void rejected(const std::function<void()>& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid descriptor accepted");
}
int main() {
    auto valid = Loan{Place(oreslang::dop::Layout::AoS,"world",{},"P","x",Range(0,8),0),Access::Read};
    for (int mutation = 0; mutation < 5; ++mutation) {
        auto bad = valid;
        switch (mutation) {
            case 0: bad.place.store = " "; break;
            case 1: bad.place.rows->end = 0; break;
            case 2: bad.place.component.reset(); break;
            case 3: bad.access = static_cast<Access>(99); break;
            case 4: bad.place.layout = static_cast<oreslang::dop::Layout>(99); break;
        }
        rejected([&]{can_run_concurrently({bad},{});});
        rejected([&]{can_run_concurrently({},{bad});});
        rejected([&]{require_internally_valid({bad});});
        rejected([&]{conflicts(bad,valid);});
        if (mutation != 3) {
            rejected([&]{overlaps(bad.place,valid.place);});
            rejected([&]{require_current_epoch(bad.place,0);});
        }
    }
    // Native unsigned domains are intentional extensions of the portable corpus.
    auto wide = valid;
    wide.place.epoch = UINT64_MAX;
    wide.place.rows = Range(UINT64_MAX - 1, UINT64_MAX);
    require_current_epoch(wide.place,UINT64_MAX);
    if (!overlaps(wide.place,wide.place)) throw std::runtime_error("u64 extension lost");
    std::string line;
    while (std::getline(std::cin,line)) {
        try {
            std::vector<std::string> s;
            std::istringstream input(line);
            std::string field;
            while(std::getline(input,field,'\t')) s.push_back(field);
            if (s.size()!=18) throw std::invalid_argument("probe columns");
            auto a = loan(s,0), b = loan(s,9);
            const bool conflict = conflicts(a,b);
            if (can_run_concurrently({a},{b}) == conflict)
                throw std::runtime_error("set/pair policy mismatch");
            bool invalid = false;
            try { require_internally_valid({a,b}); }
            catch (const std::invalid_argument&) { invalid = true; }
            if (invalid != conflict) throw std::runtime_error("internal/pair policy mismatch");
            require_current_epoch(a.place,a.place.epoch);
            rejected([&]{require_current_epoch(a.place,a.place.epoch ^ 1);});
            std::cout << (conflict ? "conflict" : "independent") << '\n';
        } catch (const std::invalid_argument&) { std::cout << "invalid\n"; }
    }
}
