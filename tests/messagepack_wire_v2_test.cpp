#include "oreslang/runtime/MessagePackWireV2.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using oreslang::wire::Shape;
using oreslang::wire::Value;

static std::pair<Shape, Value> specimen(const std::string& name) {
    if (name == "demo.TupleBoolString.v1")
        return {Shape::tuple({Shape::boolean(),Shape::string()}),
                Value(Value::Items{Value(true),Value("foo")})};
    if (name == "demo.FixedArrayInts.v1")
        return {Shape::fixedArray({Shape::integer(),Shape::integer(),Shape::integer()}),
                Value(Value::Items{Value(1),Value(2),Value(3)})};
    if (name == "demo.FixedListNested.v1")
        return {Shape::fixedList({Shape::list(Shape::string()),Shape::boolean()}),
                Value(Value::Items{Value(Value::Items{Value("a"),Value("b")}),Value(false)})};
    if (name == "demo.ArrayStrings.v1")
        return {Shape::array(Shape::string()),
                Value(Value::Items{Value("a"),Value("b")})};
    if (name == "demo.ListStrings.v1")
        return {Shape::list(Shape::string()),
                Value(Value::Items{Value("a"),Value("b")})};
    if (name == "demo.String.v1")
        return {Shape::string(),Value("foo")};
    throw std::invalid_argument("unknown golden fixture " + name);
}

template<typename Fn>
static void rejection(const char* label, const Fn& fn) {
    try { fn(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(std::string("accepted invalid wire: ") + label);
}

static void negatives() {
    using namespace oreslang::wire;
    const std::string id="demo.Evil.v1";
    const Shape good=Shape::list(Shape::string());
    auto bytes=encode(id,good,Value(Value::Items{Value("abc")}));
    rejection("Array/List type substitution", [&]{
        (void)decode(id,Shape::array(Shape::string()),bytes);
    });
    rejection("Tuple/List type substitution", [&]{
        (void)decode(id,Shape::tuple({Shape::string()}),bytes);
    });
    rejection("element type confusion", [&]{
        (void)decode(id,Shape::list(Shape::integer()),bytes);
    });
    rejection("unknown schema", [&]{
        (void)decode("other.Schema.v1",good,bytes);
    });
    auto tampered=bytes;
    for (std::size_t i=0;i+34<tampered.size();++i) {
        if (tampered[i]==0xc4 && tampered[i+1]==32) {
            tampered[i+2]^=1; break;
        }
    }
    rejection("altered schema digest", [&]{
        (void)decode(id,good,tampered);
    });
    auto downgrade=bytes;
    downgrade[1]=1;
    rejection("wire v1 downgrade", [&]{
        (void)decode(id,good,downgrade);
    });
    auto trailing=bytes;
    trailing.push_back(0);
    rejection("trailing bytes", [&]{
        (void)decode(id,good,trailing);
    });
    auto truncated=bytes;
    truncated.pop_back();
    rejection("truncated bytes", [&]{
        (void)decode(id,good,truncated);
    });
    auto scalar=encode(id,Shape::integer(),Value(1));
    scalar.back()=0xcc;
    scalar.push_back(1);
    rejection("noncanonical positive integer", [&]{
        (void)decode(id,Shape::integer(),scalar);
    });
    auto empty=encode(id,good,Value(Value::Items{}));
    auto big=empty;
    big.pop_back();
    big.insert(big.end(), {0xdc,0x00,0x64});
    rejection("forged array length vs bytes", [&]{
        (void)decode(id,good,big);
    });
    rejection("unsupported host-type string bytes", [&]{
        (void)encode(id,Shape::string(),Value(std::string("\xc0\xaf",2)));
    });
    rejection("nonfinite floating value", [&]{
        (void)encode(id,Shape::floating(),Value(std::numeric_limits<double>::infinity()));
    });
}

int main(int argc, char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--negative") {
            negatives();
            std::cout<<"PASS MessagePack v2 hostile binary cases\n";
            return 0;
        }
        if (argc!=3 || std::string(argv[1])!="--vector") {
            std::cerr << "usage: messagepack_wire_v2_test --vector <schemaid> | --negative\n";
            return 2;
        }
        std::string schema=argv[2];
        auto [shape,source]=specimen(schema);
        auto bytes=oreslang::wire::encode(schema,shape,source);
        auto result=oreslang::wire::decode(schema,shape,bytes);
        if (!(result==source)) throw std::runtime_error("failed native roundtrip");
        if (oreslang::wire::encode(schema,shape,result)!=bytes)
            throw std::runtime_error("noncanonical native re-encode");
        std::cout << oreslang::wire::hex(bytes) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "native MessagePack v2 error: " << e.what() << '\n';
        return 1;
    }
}
