#include "oreslang/frontend/ChannelGuestGate.hpp"

#include <stdexcept>
#include <string_view>
#include <iostream>

int main() {
    using oreslang::frontend::looks_like_channel_int;
    std::size_t checks = 0;
    auto check = [&](std::string_view text, bool expected) {
        // Lexer has just consumed the initial identifier token "Channel".
        if (text.substr(0, 7) != "Channel" ||
                looks_like_channel_int(text, 7) != expected)
            throw std::runtime_error("Channel<int> native gate mismatch");
        ++checks;
    };
    check("Channel<int>", true);
    check("Channel <int>", true);
    check("Channel< int >", true);
    check("Channel <  int  >", true);
    check("Channel<int>(2)", true);
    check("Channel<int>.bounded(2)", true);
    check("Channel<int>>", true); // generic closer is not a read by itself
    check("Channel<string>", false);
    check("Channel<integer>", false);
    check("Channel<int32>", false);
    check("Channel< i n t >", false);
    check("Channel<", false);
    check("Channel < > ", false);
    check("Channel < 2", false);
    check("Channel", false);
    check("Channel /*comment*/ <int>", false);
    std::cout << "LLVM_CHANNEL_INT_LEXICAL_GATE_PASS checks=" << checks << '\n';
}
