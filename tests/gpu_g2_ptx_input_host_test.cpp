#include "oreslang/gpu/G2PtxInput.hpp"
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
using oreslang::gpu::g2::read_bounded_ptx;
static int checks = 0;
static void check(bool ok) {
    if (!ok) throw std::runtime_error("GPU-G2 PTX input host test failed");
    ++checks;
}
template <typename F> static void rejects(F f) {
    bool threw=false;
    try { f(); } catch (const std::invalid_argument&) { threw=true; }
    check(threw);
}
int main() {
    const std::string expected=".version 7.0\n.visible .entry saxpy() { ret; }\n";
    {std::istringstream in(expected); check(read_bounded_ptx(in)==expected);}
    {std::istringstream in(expected+"\n// EOF\n"); check(read_bounded_ptx(in).size()>expected.size());}
    {std::istringstream in(""); rejects([&]{(void)read_bounded_ptx(in);});}
    {std::istringstream in(".entry unsafe_kernel\n"); rejects([&]{(void)read_bounded_ptx(in);});}
    {std::istringstream in(expected+std::string(1,'\0')); rejects([&]{(void)read_bounded_ptx(in);});}
    {std::istringstream in(expected); in.setstate(std::ios::badbit);
     rejects([&]{(void)read_bounded_ptx(in);});}
    constexpr std::size_t limit=512*1024;
    {std::string ptx=expected+std::string(limit-expected.size(),' ');
     std::istringstream in(ptx);check(read_bounded_ptx(in).size()==limit);}
    {std::string ptx=expected+std::string(limit+1-expected.size(),' ');
     std::istringstream in(ptx); rejects([&]{(void)read_bounded_ptx(in);});}
    std::cout << "PASS " << checks << " GPU-G2 host-only bounded PTX input checks\n";
}
