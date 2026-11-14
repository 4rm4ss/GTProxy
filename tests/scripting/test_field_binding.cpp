#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include "scripting/bindings/field.hpp"

using scripting::bindings::field;

namespace {
enum class Kind : std::uint8_t { A = 1, B = 2 };

#pragma pack(push, 1)
struct Header {
    std::uint8_t type;
    std::uint32_t net_id;
    float value;
};
#pragma pack(pop)

struct Packet {
    Header header{};
    std::string name;
    std::int32_t count{};
    Kind kind{ Kind::A };

    [[nodiscard]] int doubled() const { return count * 2; }
};

sol::state make_state()
{
    sol::state lua;
    lua.open_libraries(sol::lib::base);

    lua.new_usertype<Header>("Header",
        "type", field<&Header::type>(),
        "net_id", field<&Header::net_id>(),
        "value", field<&Header::value>()
    );

    lua.new_usertype<Packet>("Packet",
        "header", field<&Packet::header>(),
        "name", field<&Packet::name>(),
        "count", field<&Packet::count>(),
        "kind", field<&Packet::kind>(),
        "doubled", field<&Packet::doubled>()
    );

    return lua;
}
}

TEST(FieldBindingTest, ReadsAndWritesScalarsAndStrings)
{
    auto lua{ make_state() };
    Packet pkt{};
    pkt.count = 21;
    pkt.name = "hello";
    lua["pkt"] = &pkt;

    EXPECT_EQ(lua.script("return pkt.count").get<int>(), 21);
    EXPECT_EQ(lua.script("return pkt.name").get<std::string>(), "hello");
    EXPECT_EQ(lua.script("return pkt:doubled()").get<int>(), 42);

    lua.script("pkt.count = 7; pkt.name = 'world'");
    EXPECT_EQ(pkt.count, 7);
    EXPECT_EQ(pkt.name, "world");
}

TEST(FieldBindingTest, EnumsAreNumbers)
{
    auto lua{ make_state() };
    Packet pkt{};
    pkt.kind = Kind::B;
    lua["pkt"] = &pkt;

    EXPECT_EQ(lua.script("return pkt.kind").get<int>(), 2);
    lua.script("pkt.kind = 1");
    EXPECT_EQ(pkt.kind, Kind::A);
}

TEST(FieldBindingTest, NestedPackedStructIsEditableInPlace)
{
    auto lua{ make_state() };
    Packet pkt{};
    pkt.header.net_id = 10;
    pkt.header.value = 1.5f;
    lua["pkt"] = &pkt;

    EXPECT_EQ(lua.script("return pkt.header.net_id").get<std::uint32_t>(), 10u);
    lua.script("pkt.header.net_id = 99; pkt.header.type = 4; pkt.header.value = 2.5");
    EXPECT_EQ(pkt.header.net_id, 99u);
    EXPECT_EQ(pkt.header.type, 4);
    EXPECT_FLOAT_EQ(pkt.header.value, 2.5f);
}
