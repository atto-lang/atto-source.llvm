#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace oreslang {

struct AllocJsonNumber final {
    std::string lexeme;
};

struct AllocJsonValue final {
    using Array = std::vector<AllocJsonValue>;
    using Object = std::vector<std::pair<std::string, AllocJsonValue>>;
    using Storage = std::variant<std::monostate, bool, AllocJsonNumber,
                                 std::string, Array, Object>;

    Storage value{};

    AllocJsonValue() = default;
    explicit AllocJsonValue(bool v) : value(v) {}
    explicit AllocJsonValue(AllocJsonNumber v) : value(std::move(v)) {}
    explicit AllocJsonValue(std::string v) : value(std::move(v)) {}
    explicit AllocJsonValue(Array v) : value(std::move(v)) {}
    explicit AllocJsonValue(Object v) : value(std::move(v)) {}

    static AllocJsonValue null() { return AllocJsonValue(); }
};

/**
 * OS-independent allocator-backed JSON text/tree substrate for the native backend.
 *
 * This is deliberately not a guest-source Json.decode<T> implementation: the
 * current native frontend has no general string/map/record value lowering.
 * The substrate preserves exact JSON number spellings and object insertion
 * order so later typed lowering does not inherit host floating-point or map
 * ordering behavior.
 */
class AllocJson final {
public:
    using Value = AllocJsonValue;
    using Number = AllocJsonNumber;

    static constexpr std::size_t max_input_bytes = 8u * 1024u * 1024u;
    static constexpr std::size_t max_output_bytes = 16u * 1024u * 1024u;
    static constexpr std::size_t max_depth = 128u;
    static constexpr std::size_t max_nodes = 1'000'000u;
    static constexpr std::size_t max_container_entries = 200'000u;
    static constexpr std::size_t max_number_chars = 4096u;

    static Value parse(std::string_view source) {
        if (source.size() > max_input_bytes)
            throw std::runtime_error("JSON input exceeds configured byte limit");
        Parser parser(source);
        Value result = parser.parse_value(0);
        parser.skip_ws();
        if (!parser.at_end()) parser.fail("trailing data after JSON value");
        return result;
    }

    static std::string encode(const Value& value) {
        Encoder encoder;
        encoder.encode_value(value, 0);
        return std::move(encoder.out_);
    }

    static std::int64_t exact_i64(const Value& value, std::string_view where = "JSON value") {
        const auto* number = std::get_if<Number>(&value.value);
        if (!number)
            throw std::runtime_error(std::string(where) + " must be an integral JSON number");

        // Programmatic Number values are not trusted to contain parser-produced
        // lexemes. Re-validate the complete JSON-number grammar and length
        // bound before target conversion.
        const Value validated = parse(number->lexeme);
        const auto* parsed_number = std::get_if<Number>(&validated.value);
        if (!parsed_number || parsed_number->lexeme != number->lexeme)
            throw std::runtime_error(std::string(where) + " is not a canonical JSON number token");
        const std::string_view text(parsed_number->lexeme);

        std::size_t at = 0;
        bool negative = false;
        if (text[at] == '-') {
            negative = true;
            ++at;
        }

        const std::size_t exponent_at = text.find_first_of("eE", at);
        const std::size_t mantissa_end =
            exponent_at == std::string_view::npos ? text.size() : exponent_at;
        const std::size_t dot_at = text.find('.', at);
        const std::size_t fraction_digits =
            dot_at == std::string_view::npos || dot_at >= mantissa_end
                ? 0u
                : mantissa_end - dot_at - 1u;

        std::string digits;
        digits.reserve(mantissa_end - at);
        for (std::size_t i = at; i < mantissa_end; ++i) {
            if (text[i] != '.') digits.push_back(text[i]);
        }

        // The JSON grammar permits an exponent much larger than any native
        // integer. Saturate it beyond the only range that can affect an i64
        // decision instead of overflowing a host integer while parsing it.
        std::int64_t exponent = 0;
        if (exponent_at != std::string_view::npos) {
            std::size_t exp_at = exponent_at + 1u;
            bool exponent_negative = false;
            if (text[exp_at] == '+' || text[exp_at] == '-') {
                exponent_negative = text[exp_at] == '-';
                ++exp_at;
            }
            constexpr std::int64_t exponent_cap =
                static_cast<std::int64_t>(max_number_chars) + 32;
            for (; exp_at < text.size(); ++exp_at) {
                const int digit_value = text[exp_at] - '0';
                if (exponent <= exponent_cap) {
                    exponent = exponent * 10 + digit_value;
                    if (exponent > exponent_cap) exponent = exponent_cap + 1;
                }
            }
            if (exponent_negative) exponent = -exponent;
        }

        std::size_t first_nonzero = 0;
        while (first_nonzero < digits.size() && digits[first_nonzero] == '0')
            ++first_nonzero;
        if (first_nonzero == digits.size()) return 0;
        digits.erase(0, first_nonzero);

        const std::int64_t scale =
            static_cast<std::int64_t>(fraction_digits) - exponent;
        if (scale > 0) {
            const auto fractional = static_cast<std::uint64_t>(scale);
            if (fractional > digits.size())
                throw std::runtime_error(std::string(where) + " must be an integral JSON number");
            const std::size_t cut = digits.size() - static_cast<std::size_t>(fractional);
            for (std::size_t i = cut; i < digits.size(); ++i) {
                if (digits[i] != '0')
                    throw std::runtime_error(std::string(where) + " must be an integral JSON number");
            }
            digits.resize(cut);
            if (digits.empty()) return 0;
        } else if (scale < 0) {
            const auto zeros = static_cast<std::uint64_t>(-scale);
            // A non-zero signed i64 never needs more than 19 decimal digits.
            if (zeros > 19u || digits.size() > 19u - static_cast<std::size_t>(zeros))
                throw std::runtime_error(std::string(where) + " is outside signed 64-bit integer range");
            digits.append(static_cast<std::size_t>(zeros), '0');
        }

        if (digits.size() > 19u)
            throw std::runtime_error(std::string(where) + " is outside signed 64-bit integer range");

        std::string integer;
        integer.reserve(digits.size() + (negative ? 1u : 0u));
        if (negative) integer.push_back('-');
        integer.append(digits);

        std::int64_t result = 0;
        const char* begin = integer.data();
        const char* end = begin + integer.size();
        const auto converted = std::from_chars(begin, end, result, 10);
        if (converted.ec == std::errc::result_out_of_range)
            throw std::runtime_error(std::string(where) + " is outside signed 64-bit integer range");
        if (converted.ec != std::errc{} || converted.ptr != end)
            throw std::runtime_error(std::string(where) + " is not a valid signed integer");
        return result;
    }

private:
    class Parser final {
    public:
        explicit Parser(std::string_view source) : source_(source) {}

        bool at_end() const noexcept { return at_ >= source_.size(); }

        void skip_ws() noexcept {
            while (!at_end()) {
                const char c = source_[at_];
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') ++at_;
                else break;
            }
        }

        [[noreturn]] void fail(std::string_view message) const {
            throw std::runtime_error("invalid JSON at byte " + std::to_string(at_) +
                                     ": " + std::string(message));
        }

        Value parse_value(std::size_t depth) {
            count_node(depth);
            skip_ws();
            if (at_end()) fail("expected JSON value");
            switch (source_[at_]) {
                case '"': return Value(parse_string());
                case '{': return Value(parse_object(depth + 1));
                case '[': return Value(parse_array(depth + 1));
                case 't': consume_literal("true"); return Value(true);
                case 'f': consume_literal("false"); return Value(false);
                case 'n': consume_literal("null"); return Value::null();
                default: return Value(Number{parse_number()});
            }
        }

    private:
        std::string_view source_;
        std::size_t at_ = 0;
        std::size_t nodes_ = 0;

        static bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
        static bool digit19(char c) noexcept { return c >= '1' && c <= '9'; }
        static bool continuation(unsigned char c) noexcept { return (c & 0xC0u) == 0x80u; }

        void count_node(std::size_t depth) {
            if (depth > max_depth) fail("maximum nesting depth exceeded");
            ++nodes_;
            if (nodes_ > max_nodes) fail("maximum JSON node count exceeded");
        }

        void consume_literal(std::string_view literal) {
            if (source_.substr(at_, literal.size()) != literal) fail("unexpected token");
            at_ += literal.size();
        }

        Value::Object parse_object(std::size_t depth) {
            ++at_;
            skip_ws();
            Value::Object result;
            std::unordered_set<std::string> seen;
            if (!at_end() && source_[at_] == '}') { ++at_; return result; }
            for (;;) {
                skip_ws();
                if (at_end() || source_[at_] != '"') fail("object key must be a JSON string");
                std::string key = parse_string();
                if (!seen.emplace(key).second) fail("duplicate object key");
                skip_ws();
                if (at_end() || source_[at_] != ':') fail("expected ':' after object key");
                ++at_;
                Value member = parse_value(depth);
                result.emplace_back(std::move(key), std::move(member));
                if (result.size() > max_container_entries) fail("JSON object exceeds entry limit");
                skip_ws();
                if (at_end()) fail("unterminated JSON object");
                const char delimiter = source_[at_++];
                if (delimiter == '}') return result;
                if (delimiter != ',') fail("expected ',' or '}' in object");
            }
        }

        Value::Array parse_array(std::size_t depth) {
            ++at_;
            skip_ws();
            Value::Array result;
            if (!at_end() && source_[at_] == ']') { ++at_; return result; }
            for (;;) {
                result.emplace_back(parse_value(depth));
                if (result.size() > max_container_entries) fail("JSON array exceeds entry limit");
                skip_ws();
                if (at_end()) fail("unterminated JSON array");
                const char delimiter = source_[at_++];
                if (delimiter == ']') return result;
                if (delimiter != ',') fail("expected ',' or ']' in array");
            }
        }

        std::string parse_string() {
            if (at_end() || source_[at_++] != '"') fail("expected string");
            std::string result;
            while (!at_end()) {
                const unsigned char c = static_cast<unsigned char>(source_[at_++]);
                if (c == '"') return result;
                if (c < 0x20u) fail("unescaped control character in string");
                if (c == '\\') {
                    if (at_end()) fail("unterminated escape sequence");
                    const char escaped = source_[at_++];
                    switch (escaped) {
                        case '"': result.push_back('"'); break;
                        case '\\': result.push_back('\\'); break;
                        case '/': result.push_back('/'); break;
                        case 'b': result.push_back('\b'); break;
                        case 'f': result.push_back('\f'); break;
                        case 'n': result.push_back('\n'); break;
                        case 'r': result.push_back('\r'); break;
                        case 't': result.push_back('\t'); break;
                        case 'u': append_unicode_escape(result); break;
                        default: fail("invalid string escape");
                    }
                    continue;
                }
                if (c < 0x80u) {
                    result.push_back(static_cast<char>(c));
                    continue;
                }
                append_validated_utf8(result, c);
            }
            fail("unterminated JSON string");
        }

        void append_validated_utf8(std::string& out, unsigned char first) {
            const std::size_t sequence_start = at_ - 1;
            std::size_t continuation_count = 0;
            unsigned char second_min = 0x80u;
            unsigned char second_max = 0xBFu;
            if (first >= 0xC2u && first <= 0xDFu) {
                continuation_count = 1;
            } else if (first == 0xE0u) {
                continuation_count = 2; second_min = 0xA0u;
            } else if (first >= 0xE1u && first <= 0xECu) {
                continuation_count = 2;
            } else if (first == 0xEDu) {
                continuation_count = 2; second_max = 0x9Fu;
            } else if (first >= 0xEEu && first <= 0xEFu) {
                continuation_count = 2;
            } else if (first == 0xF0u) {
                continuation_count = 3; second_min = 0x90u;
            } else if (first >= 0xF1u && first <= 0xF3u) {
                continuation_count = 3;
            } else if (first == 0xF4u) {
                continuation_count = 3; second_max = 0x8Fu;
            } else {
                fail("invalid UTF-8 leading byte in string");
            }
            if (at_ + continuation_count > source_.size()) fail("truncated UTF-8 sequence in string");
            const unsigned char second = static_cast<unsigned char>(source_[at_]);
            if (second < second_min || second > second_max) fail("invalid UTF-8 continuation byte in string");
            for (std::size_t i = 1; i < continuation_count; ++i) {
                const unsigned char next = static_cast<unsigned char>(source_[at_ + i]);
                if (!continuation(next)) fail("invalid UTF-8 continuation byte in string");
            }
            at_ += continuation_count;
            out.append(source_.substr(sequence_start, continuation_count + 1));
        }

        static int hex_digit(char c) noexcept {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        std::uint16_t read_hex_unit() {
            if (at_ + 4 > source_.size()) fail("short unicode escape");
            std::uint16_t result = 0;
            for (int i = 0; i < 4; ++i) {
                const int digit_value = hex_digit(source_[at_++]);
                if (digit_value < 0) fail("non-hex digit in unicode escape");
                result = static_cast<std::uint16_t>((result << 4) |
                         static_cast<std::uint16_t>(digit_value));
            }
            return result;
        }

        void append_unicode_escape(std::string& out) {
            const std::uint16_t first = read_hex_unit();
            std::uint32_t codepoint = first;
            if (first >= 0xD800u && first <= 0xDBFFu) {
                if (at_ + 6 > source_.size() || source_[at_] != '\\' || source_[at_ + 1] != 'u')
                    fail("high surrogate escape must be followed by low surrogate escape");
                at_ += 2;
                const std::uint16_t second = read_hex_unit();
                if (second < 0xDC00u || second > 0xDFFFu)
                    fail("high surrogate escape is not followed by low surrogate");
                codepoint = 0x10000u +
                    ((static_cast<std::uint32_t>(first) - 0xD800u) << 10) +
                    (static_cast<std::uint32_t>(second) - 0xDC00u);
            } else if (first >= 0xDC00u && first <= 0xDFFFu) {
                fail("unpaired low surrogate escape");
            }
            append_codepoint(out, codepoint);
        }

        static void append_codepoint(std::string& out, std::uint32_t cp) {
            if (cp <= 0x7Fu) {
                out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FFu) {
                out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else if (cp <= 0xFFFFu) {
                out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            }
        }

        std::string parse_number() {
            const std::size_t start = at_;
            if (!at_end() && source_[at_] == '-') ++at_;
            if (at_end()) fail("incomplete JSON number");
            if (source_[at_] == '0') {
                ++at_;
                if (!at_end() && digit(source_[at_])) fail("leading zero in JSON number");
            } else {
                if (!digit19(source_[at_])) fail("expected JSON value");
                while (!at_end() && digit(source_[at_])) ++at_;
            }
            if (!at_end() && source_[at_] == '.') {
                ++at_;
                const std::size_t digits = at_;
                while (!at_end() && digit(source_[at_])) ++at_;
                if (digits == at_) fail("fraction requires at least one digit");
            }
            if (!at_end() && (source_[at_] == 'e' || source_[at_] == 'E')) {
                ++at_;
                if (!at_end() && (source_[at_] == '+' || source_[at_] == '-')) ++at_;
                const std::size_t digits = at_;
                while (!at_end() && digit(source_[at_])) ++at_;
                if (digits == at_) fail("exponent requires at least one digit");
            }
            if (at_ - start > max_number_chars) fail("JSON number exceeds configured character limit");
            return std::string(source_.substr(start, at_ - start));
        }
    };

    class Encoder final {
    public:
        std::string out_;
        std::size_t nodes_ = 0;

        void encode_value(const Value& value, std::size_t depth) {
            if (depth > max_depth) fail("JSON encode maximum nesting depth exceeded");
            ++nodes_;
            if (nodes_ > max_nodes) fail("JSON encode maximum node count exceeded");
            if (std::holds_alternative<std::monostate>(value.value)) { append("null"); return; }
            if (const auto* boolean = std::get_if<bool>(&value.value)) { append(*boolean ? "true" : "false"); return; }
            if (const auto* number = std::get_if<Number>(&value.value)) {
                validate_number(number->lexeme);
                append(number->lexeme);
                return;
            }
            if (const auto* string = std::get_if<std::string>(&value.value)) { encode_string(*string); return; }
            if (const auto* array = std::get_if<Value::Array>(&value.value)) {
                if (array->size() > max_container_entries) fail("JSON array exceeds entry limit");
                append("[");
                for (std::size_t i = 0; i < array->size(); ++i) {
                    if (i != 0) append(",");
                    encode_value((*array)[i], depth + 1);
                }
                append("]");
                return;
            }
            const auto& object = std::get<Value::Object>(value.value);
            if (object.size() > max_container_entries) fail("JSON object exceeds entry limit");
            std::unordered_set<std::string_view> seen;
            append("{");
            for (std::size_t i = 0; i < object.size(); ++i) {
                const auto& member = object[i];
                if (!seen.emplace(member.first).second) fail("duplicate JSON object key in value");
                if (i != 0) append(",");
                encode_string(member.first);
                append(":");
                encode_value(member.second, depth + 1);
            }
            append("}");
        }

    private:
        [[noreturn]] static void fail(std::string_view message) {
            throw std::runtime_error(std::string(message));
        }

        void append(std::string_view text) {
            if (text.size() > max_output_bytes - out_.size())
                fail("JSON output exceeds configured byte limit");
            out_.append(text);
        }

        static void validate_number(std::string_view lexeme) {
            if (lexeme.empty() || lexeme.size() > max_number_chars)
                fail("invalid JSON number value");
            Parser parser(lexeme);
            const Value parsed = parser.parse_value(0);
            parser.skip_ws();
            const auto* number = std::get_if<Number>(&parsed.value);
            if (!parser.at_end() || !number || number->lexeme != lexeme)
                fail("invalid JSON number value");
        }

        void encode_string(std::string_view text) {
            append("\"");
            for (std::size_t i = 0; i < text.size();) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                switch (c) {
                    case '"': append("\\\""); ++i; continue;
                    case '\\': append("\\\\"); ++i; continue;
                    case '\b': append("\\b"); ++i; continue;
                    case '\f': append("\\f"); ++i; continue;
                    case '\n': append("\\n"); ++i; continue;
                    case '\r': append("\\r"); ++i; continue;
                    case '\t': append("\\t"); ++i; continue;
                    default: break;
                }
                if (c < 0x20u) {
                    static constexpr char hex[] = "0123456789abcdef";
                    char escaped[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0x0Fu], hex[c & 0x0Fu]};
                    append(std::string_view(escaped, sizeof(escaped)));
                    ++i;
                    continue;
                }
                if (c < 0x80u) {
                    append(text.substr(i, 1));
                    ++i;
                    continue;
                }
                const std::size_t width = utf8_width(text, i);
                append(text.substr(i, width));
                i += width;
            }
            append("\"");
        }

        static std::size_t utf8_width(std::string_view text, std::size_t at) {
            const unsigned char first = static_cast<unsigned char>(text[at]);
            std::size_t width = 0;
            unsigned char second_min = 0x80u;
            unsigned char second_max = 0xBFu;
            if (first >= 0xC2u && first <= 0xDFu) width = 2;
            else if (first == 0xE0u) { width = 3; second_min = 0xA0u; }
            else if (first >= 0xE1u && first <= 0xECu) width = 3;
            else if (first == 0xEDu) { width = 3; second_max = 0x9Fu; }
            else if (first >= 0xEEu && first <= 0xEFu) width = 3;
            else if (first == 0xF0u) { width = 4; second_min = 0x90u; }
            else if (first >= 0xF1u && first <= 0xF3u) width = 4;
            else if (first == 0xF4u) { width = 4; second_max = 0x8Fu; }
            else fail("JSON string contains invalid UTF-8 leading byte");
            if (at + width > text.size()) fail("JSON string contains truncated UTF-8");
            const unsigned char second = static_cast<unsigned char>(text[at + 1]);
            if (second < second_min || second > second_max)
                fail("JSON string contains invalid UTF-8 continuation byte");
            for (std::size_t i = 2; i < width; ++i) {
                const unsigned char next = static_cast<unsigned char>(text[at + i]);
                if ((next & 0xC0u) != 0x80u)
                    fail("JSON string contains invalid UTF-8 continuation byte");
            }
            return width;
        }
    };
};

} // namespace oreslang
