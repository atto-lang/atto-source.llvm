// Standalone reference-width RSS experiment. NOT whole-Atto RSS or guest proof.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

std::uint64_t rss_kib() {
#ifdef __linux__
    std::ifstream f("/proc/self/status");
    for (std::string line; std::getline(f, line);) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            const auto pos = line.find_first_of("0123456789");
            if (pos != std::string::npos) return std::stoull(line.substr(pos));
        }
    }
#endif
    return 0;
}

template <class Ref>
int bench(std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(Ref)) return 2;
    std::vector<Ref> refs(count);
    std::uint64_t check = 0;
    for (std::size_t i = 0; i < count; ++i) refs[i] = static_cast<Ref>((i % 65521) + 1);
    for (std::size_t i = 0; i < count; i += 4096) check += refs[i];
    std::cout << "REFERENCE_RSS_SAMPLE width=" << sizeof(Ref) * 8
              << " count=" << count << " ref_bytes=" << count * sizeof(Ref)
              << " rss_kib=" << rss_kib() << " checksum=" << check << "\n";
    return 0;
}
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: benchmark_ref_width 32|64 count\n"; return 2; }
    try {
        const std::string mode = argv[1];
        const auto count = static_cast<std::size_t>(std::stoull(argv[2]));
        if (count == 0 || count > 20'000'000) return 2;
        if (mode == "32") return bench<std::uint32_t>(count);
        if (mode == "64") return bench<std::uint64_t>(count);
        return 2;
    } catch (const std::exception&) { return 2; }
}
