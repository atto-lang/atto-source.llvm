#pragma once
#include <openssl/evp.h>
#include <openssl/crypto.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang::wire {

constexpr int kWireVersion = 2;
constexpr std::size_t kMaxBytes = 1'048'576;
constexpr std::size_t kMaxNodes = 100'000;
constexpr std::size_t kMaxDepth = 64;
constexpr std::size_t kMaxElements = 100'000;

inline void require(bool condition, const char* error) {
    if (!condition) throw std::invalid_argument(error);
}

struct Shape {
    enum class Kind { Bool, Int64, Float64, String, Tuple, FixedArray, FixedList, Array, List };
    Kind kind;
    std::vector<Shape> members;
    static Shape boolean() { return {Kind::Bool, {}}; }
    static Shape integer() { return {Kind::Int64, {}}; }
    static Shape floating() { return {Kind::Float64, {}}; }
    static Shape string() { return {Kind::String, {}}; }
    static Shape tuple(std::vector<Shape> values) { return {Kind::Tuple, std::move(values)}; }
    static Shape fixedArray(std::vector<Shape> values) { return {Kind::FixedArray, std::move(values)}; }
    static Shape fixedList(std::vector<Shape> values) { return {Kind::FixedList, std::move(values)}; }
    static Shape array(Shape member) { return {Kind::Array, {std::move(member)}}; }
    static Shape list(Shape member) { return {Kind::List, {std::move(member)}}; }
};

struct Value {
    using Items = std::vector<Value>;
    std::variant<bool, std::int64_t, double, std::string, Items> payload;
    Value(bool v) : payload(v) {}
    Value(std::int64_t v) : payload(v) {}
    Value(int v) : payload(static_cast<std::int64_t>(v)) {}
    Value(double v) : payload(v) {}
    Value(std::string v) : payload(std::move(v)) {}
    Value(const char* v) : payload(std::string(v)) {}
    Value(Items v) : payload(std::move(v)) {}
    bool operator==(const Value& other) const { return payload == other.payload; }
};

inline bool fixed(Shape::Kind k) {
    return k == Shape::Kind::Tuple || k == Shape::Kind::FixedArray
        || k == Shape::Kind::FixedList;
}
inline bool collection(Shape::Kind k) {
    return fixed(k) || k == Shape::Kind::Array || k == Shape::Kind::List;
}

inline void appendDescriptor(std::string& out, const Shape& shape,
                             std::size_t depth, std::size_t& nodes) {
    require(depth <= kMaxDepth && ++nodes <= kMaxNodes, "wire shape depth/nodes exceeded");
    switch (shape.kind) {
        case Shape::Kind::Bool: require(shape.members.empty(), "scalar with members"); out += "B"; return;
        case Shape::Kind::Int64: require(shape.members.empty(), "scalar with members"); out += "I"; return;
        case Shape::Kind::Float64: require(shape.members.empty(), "scalar with members"); out += "F"; return;
        case Shape::Kind::String: require(shape.members.empty(), "scalar with members"); out += "S"; return;
        case Shape::Kind::Tuple: out += "T["; break;
        case Shape::Kind::FixedArray: out += "FA["; break;
        case Shape::Kind::FixedList: out += "FL["; break;
        case Shape::Kind::Array: out += "A["; break;
        case Shape::Kind::List: out += "L["; break;
    }
    require(shape.members.size() <= kMaxElements, "wire shape arity exceeded");
    if (!fixed(shape.kind)) require(shape.members.size() == 1, "dynamic sequence expects one element shape");
    for (std::size_t i = 0; i < shape.members.size(); ++i) {
        if (i > 0) out += ',';
        appendDescriptor(out, shape.members[i], depth + 1, nodes);
    }
    out += ']';
}

inline std::array<unsigned char, 32> shapeDigest(const Shape& shape) {
    std::string descriptor = "oreslang.actor.shape.v2/";
    std::size_t nodes = 0;
    appendDescriptor(descriptor, shape, 0, nodes);
    std::array<unsigned char, 32> hash{};
    unsigned int len = 0;
    require(EVP_Digest(descriptor.data(), descriptor.size(), hash.data(),
                       &len, EVP_sha256(), nullptr) == 1 && len == hash.size(),
            "SHA-256 digest unavailable");
    return hash;
}

inline void schemaId(const std::string& id) {
    require(!id.empty() && id.size() <= 256, "invalid wire schema length");
    for (unsigned char c : id) {
        require(c >= 33 && c <= 126, "wire schema must be printable ASCII");
    }
}

inline void utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto a = static_cast<unsigned char>(s[i]);
        if (a <= 0x7f) { ++i; continue; }
        int trailing = 0;
        if (a >= 0xc2 && a <= 0xdf) trailing = 1;
        else if (a >= 0xe0 && a <= 0xef) trailing = 2;
        else if (a >= 0xf0 && a <= 0xf4) trailing = 3;
        else throw std::invalid_argument("malformed UTF-8 lead byte");
        require(static_cast<std::size_t>(trailing) <= s.size() - i - 1,
                "truncated UTF-8 sequence");
        const unsigned char b = static_cast<unsigned char>(s[i + 1]);
        require(b >= 0x80 && b <= 0xbf, "malformed UTF-8 continuation");
        require(!(a == 0xe0 && b < 0xa0) && !(a == 0xed && b > 0x9f)
                && !(a == 0xf0 && b < 0x90) && !(a == 0xf4 && b > 0x8f),
                "invalid Unicode scalar/overlong UTF-8");
        for (int j = 2; j <= trailing; ++j) {
            unsigned char c = static_cast<unsigned char>(s[i + j]);
            require(c >= 0x80 && c <= 0xbf, "malformed UTF-8 continuation");
        }
        i += static_cast<std::size_t>(trailing + 1);
    }
}

class Writer {
public:
    std::vector<unsigned char> out;
    std::size_t nodes = 0;
    void add(unsigned char b) {
        require(out.size() < kMaxBytes, "wire exceeds byte budget");
        out.push_back(b);
    }
    void raw(const unsigned char* b, std::size_t n) {
        require(n <= kMaxBytes - out.size(), "wire exceeds byte budget");
        out.insert(out.end(), b, b + n);
    }
    void uint(std::uint64_t n, int width) {
        for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
            add(static_cast<unsigned char>((n >> shift) & 0xff));
    }
    void array(std::size_t n) {
        require(n <= kMaxElements, "wire sequence length exceeded");
        if (n <= 15) add(static_cast<unsigned char>(0x90u | n));
        else if (n <= 65535) { add(0xdc); uint(n, 2); }
        else { add(0xdd); uint(n, 4); }
    }
    void integer(std::int64_t v) {
        if (v >= 0 && v <= 127) add(static_cast<unsigned char>(v));
        else if (v >= -32 && v < 0) add(static_cast<unsigned char>(v));
        else if (v >= 0 && v <= 255) { add(0xcc); uint(v, 1); }
        else if (v >= 0 && v <= 65535) { add(0xcd); uint(v, 2); }
        else if (v >= 0 && v <= 0xffffffffLL) { add(0xce); uint(v, 4); }
        else if (v >= 0) { add(0xcf); uint(static_cast<std::uint64_t>(v), 8); }
        else if (v >= -128) { add(0xd0); uint(static_cast<std::uint64_t>(v), 1); }
        else if (v >= -32768) { add(0xd1); uint(static_cast<std::uint64_t>(v), 2); }
        else if (v >= -2147483648LL) { add(0xd2); uint(static_cast<std::uint64_t>(v), 4); }
        else { add(0xd3); uint(static_cast<std::uint64_t>(v), 8); }
    }
    void string(const std::string& v) {
        require(v.size() <= kMaxBytes, "string exceeds byte budget");
        utf8(v);
        if (v.size() <= 31) add(static_cast<unsigned char>(0xa0u | v.size()));
        else if (v.size() <= 255) { add(0xd9); uint(v.size(), 1); }
        else if (v.size() <= 65535) { add(0xda); uint(v.size(), 2); }
        else { add(0xdb); uint(v.size(), 4); }
        raw(reinterpret_cast<const unsigned char*>(v.data()), v.size());
    }
    void value(const Shape& type, const Value& v, std::size_t depth) {
        require(depth <= kMaxDepth && ++nodes <= kMaxNodes, "wire node/depth quota");
        switch (type.kind) {
            case Shape::Kind::Bool:
                require(std::holds_alternative<bool>(v.payload), "expected bool");
                add(std::get<bool>(v.payload) ? 0xc3 : 0xc2); return;
            case Shape::Kind::Int64:
                require(std::holds_alternative<std::int64_t>(v.payload), "expected i64");
                integer(std::get<std::int64_t>(v.payload)); return;
            case Shape::Kind::Float64: {
                require(std::holds_alternative<double>(v.payload), "expected f64");
                double d = std::get<double>(v.payload);
                require(std::isfinite(d), "nonfinite float not supported");
                std::uint64_t bits = 0;
                std::memcpy(&bits, &d, 8);
                add(0xcb); uint(bits, 8); return;
            }
            case Shape::Kind::String:
                require(std::holds_alternative<std::string>(v.payload), "expected string");
                string(std::get<std::string>(v.payload)); return;
            default: break;
        }
        require(std::holds_alternative<Value::Items>(v.payload), "expected sequence");
        const auto& items = std::get<Value::Items>(v.payload);
        require(!fixed(type.kind) || items.size() == type.members.size(),
                "wrong fixed sequence arity");
        array(items.size());
        for (std::size_t i = 0; i < items.size(); ++i)
            value(type.members[fixed(type.kind) ? i : 0], items[i], depth + 1);
    }
};

class Reader {
public:
    const std::vector<unsigned char>& input;
    std::size_t at = 0;
    std::size_t nodes = 0;
    explicit Reader(const std::vector<unsigned char>& bytes) : input(bytes) {
        require(input.size() <= kMaxBytes, "oversized wire message");
    }
    unsigned char take() {
        require(at < input.size(), "truncated MessagePack");
        return input[at++];
    }
    std::uint64_t uint(int width) {
        require(static_cast<std::size_t>(width) <= input.size() - at,
                "truncated MessagePack number");
        std::uint64_t n = 0;
        for (int i = 0; i < width; ++i) n = (n << 8) | take();
        return n;
    }
    std::size_t array() {
        unsigned char b = take();
        std::uint64_t n;
        if ((b & 0xf0) == 0x90) n = b & 15;
        else if (b == 0xdc) { n = uint(2); require(n > 15, "noncanonical array16"); }
        else if (b == 0xdd) { n = uint(4); require(n > 65535, "noncanonical array32"); }
        else throw std::invalid_argument("expected array");
        require(n <= kMaxElements, "oversized array");
        return static_cast<std::size_t>(n);
    }
    std::int64_t integer() {
        unsigned char b = take();
        if (b <= 0x7f) return b;
        if (b >= 0xe0) return static_cast<std::int8_t>(b);
        std::uint64_t n = 0;
        switch (b) {
            case 0xcc: n=uint(1); require(n > 127, "noncanonical uint8"); break;
            case 0xcd: n=uint(2); require(n > 255, "noncanonical uint16"); break;
            case 0xce: n=uint(4); require(n > 65535, "noncanonical uint32"); break;
            case 0xcf:
                n=uint(8);
                require(n <= static_cast<std::uint64_t>(INT64_MAX) && n > UINT32_MAX,
                        "out-of-range or noncanonical uint64"); break;
            case 0xd0: {
                auto v=static_cast<std::int8_t>(uint(1));
                require(v < -32, "noncanonical int8"); return v;
            }
            case 0xd1: {
                auto v=static_cast<std::int16_t>(uint(2));
                require(v < -128, "noncanonical int16"); return v;
            }
            case 0xd2: {
                auto v=static_cast<std::int32_t>(uint(4));
                require(v < -32768, "noncanonical int32"); return v;
            }
            case 0xd3: {
                std::uint64_t bits = uint(8);
                std::int64_t v = 0;
                std::memcpy(&v, &bits, 8);
                require(v < INT32_MIN, "noncanonical int64");
                return v;
            }
            default: throw std::invalid_argument("expected signed integer");
        }
        return static_cast<std::int64_t>(n);
    }
    std::string string() {
        unsigned char b = take();
        std::uint64_t n = 0;
        if ((b & 0xe0) == 0xa0) n = b & 31;
        else if (b == 0xd9) { n=uint(1); require(n > 31, "noncanonical str8"); }
        else if (b == 0xda) { n=uint(2); require(n > 255, "noncanonical str16"); }
        else if (b == 0xdb) { n=uint(4); require(n > 65535, "noncanonical str32"); }
        else throw std::invalid_argument("expected string");
        require(n <= kMaxBytes && n <= input.size() - at, "truncated/oversized string");
        std::string result(reinterpret_cast<const char*>(input.data()+at),
                           static_cast<std::size_t>(n));
        utf8(result);
        at += static_cast<std::size_t>(n);
        return result;
    }
    std::array<unsigned char,32> digest() {
        require(take() == 0xc4 && take() == 32, "expected bin8(32) digest");
        require(input.size() - at >= 32, "truncated schema hash");
        std::array<unsigned char,32> d{};
        std::memcpy(d.data(),input.data()+at,32);
        at += 32;
        return d;
    }
    Value value(const Shape& shape, std::size_t depth) {
        require(depth <= kMaxDepth && ++nodes <= kMaxNodes, "wire depth/node quota");
        switch (shape.kind) {
            case Shape::Kind::Bool: {
                unsigned char b=take();
                require(b == 0xc2 || b == 0xc3, "expected bool");
                return Value(b == 0xc3);
            }
            case Shape::Kind::Int64: return Value(integer());
            case Shape::Kind::Float64: {
                require(take() == 0xcb, "expected f64");
                std::uint64_t bits=uint(8);
                double d=0;
                std::memcpy(&d,&bits,8);
                require(std::isfinite(d), "nonfinite float not supported");
                return Value(d);
            }
            case Shape::Kind::String: return Value(string());
            default: break;
        }
        std::size_t count = array();
        require(!fixed(shape.kind) || count == shape.members.size(), "fixed array arity mismatch");
        require(count <= input.size() - at && count <= kMaxNodes - nodes,
                "array length exceeds remaining bytes/nodes");
        Value::Items values;
        values.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            values.push_back(value(shape.members[fixed(shape.kind) ? i : 0], depth+1));
        return Value(std::move(values));
    }
};

inline std::vector<unsigned char> encode(const std::string& id, const Shape& shape,
                                          const Value& value) {
    schemaId(id);
    auto digest = shapeDigest(shape);
    Writer w;
    w.array(4);
    w.integer(kWireVersion);
    w.string(id);
    w.add(0xc4); w.add(32);
    w.raw(digest.data(),digest.size());
    w.value(shape, value, 0);
    return w.out;
}
inline Value decode(const std::string& id, const Shape& shape,
                    const std::vector<unsigned char>& bytes) {
    schemaId(id);
    auto expected = shapeDigest(shape);
    Reader r(bytes);
    require(r.array() == 4, "incorrect envelope arity");
    require(r.integer() == kWireVersion, "unsupported MessagePack wire version");
    require(r.string() == id, "wrong schema ID");
    auto actual = r.digest();
    require(CRYPTO_memcmp(actual.data(),expected.data(),32) == 0, "schema shape mismatch");
    auto value = r.value(shape,0);
    require(r.at == bytes.size(), "trailing MessagePack bytes");
    return value;
}
inline std::string hex(const std::vector<unsigned char>& bytes) {
    const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size()*2);
    for (unsigned char b : bytes) {
        out += digits[b >> 4]; out += digits[b & 15];
    }
    return out;
}
} // namespace oreslang::wire
