// HPC finance risk parity: native C++17 HOST ORACLE only, not LLVM guest execution.
// The production Oreslang compiler must separately compile and execute this contract.
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct RiskDecision { bool approved; int reason; };
struct RiskInput { int side; double quantity; double price; double max_quantity; };

static RiskDecision decide(const RiskInput& x) {
    int code = 0;
    if (!std::isfinite(x.quantity) || !std::isfinite(x.price) ||
        !std::isfinite(x.max_quantity) || x.max_quantity < 0.0) code = 4;
    else if (x.side == 0) code = 1;
    else if (x.side != 1 && x.side != -1) code = 5;
    else if (x.quantity < 0.0) code = 2;
    else if (x.quantity == 0.0) code = 7;
    else if (x.price <= 0.0) code = 6;
    else if (x.quantity > x.max_quantity) code = 3;
    return {code == 0, code};
}

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static double value(const std::string& text) {
    if (text == "NaN") return std::numeric_limits<double>::quiet_NaN();
    if (text == "+Infinity") return std::numeric_limits<double>::infinity();
    if (text == "-Infinity") return -std::numeric_limits<double>::infinity();
    require(!text.empty() && text.size() < 48, "bad fixture numeric width");
    std::size_t used = 0;
    const double parsed = std::stod(text, &used);
    require(used == text.size() && std::isfinite(parsed), "unrecognized finite scalar");
    return parsed;
}

static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> columns;
    std::istringstream input(line);
    std::string cell;
    while (std::getline(input, cell, '|')) columns.push_back(cell);
    return columns;
}

static std::vector<std::string> fixture_lines(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "cannot open finance risk corpus");
    std::vector<std::string> result;
    std::string line;
    while (std::getline(stream, line)) {
        require(line.size() <= 256, "oversized corpus line");
        if (!line.empty() && line[0] != '#') result.push_back(line);
    }
    require(stream.eof(), "corpus read failure");
    return result;
}

static void check_inventory(const std::vector<std::string>& lines) {
    require(lines.size() == 23, "wrong finance inventory count");
    std::set<std::string> ids;
    std::size_t accept = 0, deny = 0;
    for (const auto& line : lines) {
        const auto fields = split(line);
        require(fields.size() == 7, "wrong finance corpus column count");
        require(!fields[0].empty() && ids.insert(fields[0]).second, "duplicate fixture ID");
        const int side = std::stoi(fields[1]);
        const auto result = decide({side, value(fields[2]), value(fields[3]), value(fields[4])});
        require(fields[5] == "true" || fields[5] == "false", "bad approved flag");
        const bool expected_approval = fields[5] == "true";
        require(result.approved == expected_approval, "finance approval drift");
        require(result.reason == std::stoi(fields[6]), "finance reason-code drift");
        result.approved ? ++accept : ++deny;
    }
    require(accept == 3 && deny == 20, "finance inventory outcome count drift");
}

template<typename F> static void must_reject(F&& f) {
    try { f(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("malformed finance corpus was admitted");
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: hpc-finance-risk-contract fixture.tsv");
        const auto lines = fixture_lines(argv[1]);
        check_inventory(lines);
        auto duplicate = lines;
        duplicate.back() = duplicate.front();
        must_reject([&] { check_inventory(duplicate); });
        auto missing = lines;
        missing.pop_back();
        must_reject([&] { check_inventory(missing); });
        auto corrupt_reason = lines;
        corrupt_reason[0].replace(corrupt_reason[0].find_last_of('|') + 1, std::string::npos, "5");
        must_reject([&] { check_inventory(corrupt_reason); });
        auto corrupt_bool = lines;
        const auto pos = corrupt_bool[0].find("|true|");
        require(pos != std::string::npos, "unexpected first fixture schema");
        corrupt_bool[0].replace(pos, 6, "|false|");
        must_reject([&] { check_inventory(corrupt_bool); });

        std::array<int,8> counts{};
        std::size_t total = 0;
        for (int side : {-2, -1, 0, 1, 2})
            for (double q : {-1.0, 0.0, 1.0, 3.0})
                for (double p : {-1.0, 0.0, 10.0})
                    for (double maximum : {-1.0, 0.0, 2.0}) {
                        const auto out = decide({side,q,p,maximum});
                        require(out.reason >= 0 && out.reason <= 7, "bad risk reason");
                        require(out.approved == (out.reason == 0), "risk code/approval drift");
                        ++counts[static_cast<std::size_t>(out.reason)];
                        ++total;
                    }
        require(total == 180, "wrong cross-product size");
        require(counts == std::array<int,8>{2,24,12,6,60,48,16,12},
                "risk precedence distribution drift");
        std::cout << "HPC_FINANCE_HOST_ONLY=PASS 23-fixture 180-cross-product 4-mutations"
                  << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "HPC_FINANCE_HOST_ONLY=FAIL: " << e.what() << '\n';
        return 1;
    }
}
