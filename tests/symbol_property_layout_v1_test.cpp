#include "oreslang/runtime/SymbolPropertyLayoutV1.hpp"
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::symbol_property_v1;

void check(bool condition) {
    if (!condition) throw std::runtime_error("Symbol property-key assertion failed");
}

template <class Exception, class Func>
void throws(Func&& f) {
    bool seen = false;
    try { f(); } catch (const Exception&) { seen = true; }
    check(seen);
}

int main() {
    const auto ordinary = Key::string("status");
    const auto tag = Key::process_symbol("status");
    const auto literal_string = Key::string("#status");
    check(!(ordinary == tag) && !(literal_string == tag));
    check(static_cast<unsigned>(ordinary.domain()) == 0);
    check(static_cast<unsigned>(tag.domain()) == 1);
    check(Layout::declared({ordinary, tag, literal_string}).size() == 3);
    check(Layout::declared({ordinary, tag}).require_slot(tag) == 1);
    throws<std::out_of_range>([&] { (void)Layout::declared({ordinary}).require_slot(tag); });

    const std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    const auto stable = Key::stable_symbol(uuid);
    const auto same_name_process = Key::process_symbol(uuid);
    check(!(stable == same_name_process));
    check(static_cast<unsigned>(stable.domain()) == 2);
    throws<std::invalid_argument>([&] { (void)Layout::declared({tag, tag}); });
    throws<std::invalid_argument>([] { (void)Key::stable_symbol("550E8400-e29b-41d4-a716-446655440000"); });

    const auto a = Layout::canonical({tag, Key::string("z"), stable, Key::string("a")});
    const auto b = Layout::canonical({Key::string("a"), stable, Key::string("z"), tag});
    check(a.keys() == b.keys());
    check(a.require_slot(Key::string("a")) == 0);
    check(a.require_slot(Key::string("z")) == 1);
    check(a.require_slot(tag) == 2);
    check(a.require_slot(stable) == 3);

    // Valid non-ASCII UTF-8 remains valid for String properties, even when
    // native Symbol NFC tables are unavailable. String keys are not NFC-folded.
    const auto nfc = Key::string("caf\xc3\xa9");
    const auto nfd = Key::string("cafe\xcc\x81");
    check(!(nfc == nfd));
    check(Layout::canonical({nfc, nfd}).size() == 2);
    throws<std::invalid_argument>([] { (void)Key::string("\xc0\xaf"); });
    throws<std::invalid_argument>([] { (void)Key::string("\xed\xa0\x80"); });
    throws<std::invalid_argument>([] { (void)Key::string("\xf4\x90\x80\x80"); });
    throws<std::invalid_argument>([] { (void)Key::string("\xf0\x9f\x92"); });
    throws<std::invalid_argument>([] { (void)Key::process_symbol("caf\xc3\xa9"); });
    throws<std::invalid_argument>([] { (void)Key::process_symbol(""); });
    throws<std::invalid_argument>([] { (void)Key::process_symbol(std::string(257, 'x')); });
    check(Key::string("\xf0\x9f\x8c\x8d").name().size() == 4);
    check(Key::string(std::string(16384, 'x')).name().size() == 16384);
    throws<std::invalid_argument>([] { (void)Key::string(std::string(16385, 'x')); });
    std::vector<Key> fields;
    for (unsigned i = 0; i < MAX_STATIC_FIELDS; ++i)
        fields.push_back(Key::string("field_" + std::to_string(i)));
    check(Layout::declared(fields).size() == MAX_STATIC_FIELDS);
    fields.push_back(Key::string("overflow"));
    throws<std::invalid_argument>([&] { (void)Layout::canonical(fields); });

    std::vector<Key> long_fields;
    for (unsigned i = 0; i < 63; ++i) {
        auto suffix = std::to_string(i);
        long_fields.push_back(Key::string(std::string(16384 - suffix.size(), 'x') + suffix));
    }
    check(Layout::declared(long_fields).size() == 63);
    long_fields.push_back(Key::string(std::string(16384, 'z')));
    throws<std::invalid_argument>([&] { (void)Layout::declared(long_fields); });

}
