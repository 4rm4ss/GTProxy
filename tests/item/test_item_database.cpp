#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "item/item_database.hpp"
#include "utils/byte_stream.hpp"

using utils::ByteStream;

namespace {
constexpr std::string_view NAME_KEY{ "PBG892FXX982ABC*" };

std::string encrypt_item_name(std::string_view name, const std::uint32_t item_id)
{
    std::string out;
    out.reserve(name.size());

    for (std::size_t i = 0; i < name.size(); ++i) {
        out += static_cast<char>(name[i] ^ NAME_KEY[(i + item_id) % NAME_KEY.size()]);
    }

    return out;
}

struct TestItem {
    std::uint32_t id;
    std::string name;
    std::string hit_sound_file;
    std::uint32_t hit_sound_file_hash;
    std::uint8_t unk_v26;
    std::string unk_v27;
};

void write_item(ByteStream<>& bs, const std::uint16_t version, const TestItem& item)
{
    const std::array<std::uint8_t, 60> unk_60{};
    const std::array<std::uint8_t, 9> unk_9{};

    bs.write<std::uint32_t>(item.id);
    bs.write<std::uint16_t>(0);                         // flags
    bs.write<std::uint8_t>(17);                         // item type (Normal)
    bs.write<std::uint8_t>(0);                          // material
    bs.write(encrypt_item_name(item.name, item.id));
    bs.write("tiles_page1.rttex");
    bs.write<std::uint32_t>(0x11223344);                // texture hash
    bs.write<std::uint8_t>(0);                          // visual effect
    bs.write<std::uint32_t>(0);                         // cooking time
    bs.write<std::uint8_t>(1);                          // texture x
    bs.write<std::uint8_t>(2);                          // texture y
    bs.write<std::uint8_t>(0);                          // texture type
    bs.write<std::uint8_t>(0);                          // stripey wallpaper
    bs.write<std::uint8_t>(1);                          // collision type
    bs.write<std::uint8_t>(18);                         // health
    bs.write<std::uint32_t>(3600);                      // reset time
    bs.write<std::uint8_t>(0);                          // clothing type
    bs.write<std::uint16_t>(1);                         // rarity
    bs.write<std::uint8_t>(200);                        // max amount
    bs.write("");                                       // extra file
    bs.write<std::uint32_t>(0);                         // extra file hash
    bs.write<std::uint32_t>(0);                         // animation time
    bs.write("");                                       // pet name
    bs.write("");                                       // pet prefix
    bs.write("");                                       // pet suffix
    bs.write("");                                       // pet ability
    bs.write<std::uint8_t>(0);                          // seed base
    bs.write<std::uint8_t>(0);                          // seed overlay
    bs.write<std::uint8_t>(0);                          // tree base
    bs.write<std::uint8_t>(0);                          // tree leaves
    bs.write<std::uint32_t>(0);                         // seed color
    bs.write<std::uint32_t>(0);                         // seed overlay color
    bs.write<std::uint32_t>(0);                         // ingredient
    bs.write<std::uint32_t>(31);                        // grow time
    bs.write<std::uint32_t>(0);                         // fx flags
    bs.write("");                                       // animating coordinates
    bs.write("");                                       // animating texture file
    bs.write("");                                       // animating coordinates 2
    bs.write<std::uint32_t>(0);                         // unk 1
    bs.write<std::uint32_t>(0);                         // unk 2
    bs.write<std::uint32_t>(0);                         // flags 2
    bs.write(unk_60);
    bs.write<std::uint32_t>(0);                         // tile range
    bs.write<std::uint32_t>(0);                         // vault capacity
    bs.write("");                                       // punch options
    bs.write<std::uint32_t>(0);                         // unk 4
    bs.write(unk_9);                                    // body part list
    bs.write<std::uint32_t>(0);                         // light range
    bs.write<std::uint32_t>(0);                         // unk 5
    bs.write<std::uint8_t>(0);                          // can sit
    for (int i = 0; i < 6; ++i) {
        bs.write<std::uint32_t>(0);                     // player offset / chair texture / chair leg offset
    }
    bs.write("");                                       // chair texture file
    bs.write("");                                       // renderer data file
    bs.write<std::uint32_t>(0);                         // unk 6
    bs.write<std::int32_t>(0);                          // renderer data file hash
    bs.write(unk_9);                                    // unk 7
    bs.write<std::uint16_t>(0);                         // unk 8
    bs.write("This is a test item.");                   // info
    bs.write<std::uint16_t>(0);                         // ingredients[0]
    bs.write<std::uint16_t>(0);                         // ingredients[1]

    if (version >= 24) {
        bs.write<std::uint8_t>(0);
    }

    if (version >= 25) {
        bs.write(item.hit_sound_file);
        bs.write<std::uint32_t>(item.hit_sound_file_hash);
    }

    if (version >= 26) {
        bs.write<std::uint8_t>(item.unk_v26);
    }

    if (version >= 27) {
        bs.write(item.unk_v27);
    }
}

std::vector<std::byte> build_items_dat(const std::uint16_t version, const std::vector<TestItem>& items)
{
    ByteStream<> bs{};
    bs.write<std::uint16_t>(version);
    bs.write<std::uint32_t>(static_cast<std::uint32_t>(items.size()));

    for (const auto& item : items) {
        write_item(bs, version, item);
    }

    const auto* begin{ bs.get_raw_ptr() };
    return { begin, begin + bs.get_size() };
}

const std::vector<TestItem> TEST_ITEMS{
    { 0, "Blank", "", 0, 0, "" },
    { 1, "Blank Seed", "", 0, 0, "" },
    { 2, "Dirt", "audio/punch_dirt.wav", 0xDEADBEEF, 7, "v27-extra" },
};
}

class ItemDatabaseVersionTest : public ::testing::TestWithParam<std::uint16_t> {
protected:
    void TearDown() override { item::ItemDatabase::instance().clear(); }
};

TEST_P(ItemDatabaseVersionTest, ParsesAllItems)
{
    const auto version{ GetParam() };
    const auto data{ build_items_dat(version, TEST_ITEMS) };

    auto& db{ item::ItemDatabase::instance() };
    ASSERT_TRUE(db.parse(data));

    EXPECT_EQ(db.get_version(), version);
    EXPECT_EQ(db.get_count(), TEST_ITEMS.size());

    for (const auto& expected : TEST_ITEMS) {
        const auto* item{ db.get_item(expected.id) };
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->item_id, expected.id);
        EXPECT_EQ(item->item_name, expected.name);
        EXPECT_EQ(item->texture_file_path, "tiles_page1.rttex");
        EXPECT_EQ(item->info, "This is a test item.");
        EXPECT_EQ(item->grow_time, 31u);
    }

    const auto* dirt{ db.get_item(2) };
    ASSERT_NE(dirt, nullptr);

    if (version >= 25) {
        EXPECT_EQ(dirt->hit_sound_file, "audio/punch_dirt.wav");
        EXPECT_EQ(dirt->hit_sound_file_hash, 0xDEADBEEFu);
    }

    if (version >= 26) {
        EXPECT_EQ(dirt->__unk_10, 7);
    }

    if (version >= 27) {
        EXPECT_EQ(dirt->__unk_11, "v27-extra");
    }
}

INSTANTIATE_TEST_SUITE_P(
    SupportedVersions,
    ItemDatabaseVersionTest,
    ::testing::Values<std::uint16_t>(23, 24, 25, 26, 27)
);

TEST(ItemDatabaseTest, FailsOnTruncatedData)
{
    auto data{ build_items_dat(27, TEST_ITEMS) };
    data.resize(data.size() - 5);

    auto& db{ item::ItemDatabase::instance() };
    EXPECT_FALSE(db.parse(data));
    db.clear();
}

TEST(ItemDatabaseTest, FailsOnMissingHeader)
{
    const std::vector<std::byte> data{ std::byte{ 0x1B } };

    auto& db{ item::ItemDatabase::instance() };
    EXPECT_FALSE(db.parse(data));
    db.clear();
}

TEST(ItemDatabaseTest, ParsesBundledItemsDat)
{
    const std::filesystem::path path{ std::filesystem::path{ GTPROXY_RESOURCES_DIR } / "items.dat" };
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "Bundled items.dat not found at " << path;
    }

    auto& db{ item::ItemDatabase::instance() };
    ASSERT_TRUE(db.load_from_file(path.string()));
    EXPECT_GT(db.get_count(), 0u);

    const auto* blank{ db.get_item(0) };
    ASSERT_NE(blank, nullptr);
    EXPECT_EQ(blank->item_name, "Blank");

    const auto* last{ db.get_item(db.get_count() - 1) };
    ASSERT_NE(last, nullptr);
    EXPECT_EQ(last->item_id, db.get_count() - 1);

    db.clear();
}

TEST(ItemDatabaseTest, RejectsImpossibleItemCount)
{
    ByteStream<> bs{};
    bs.write<std::uint16_t>(27);
    bs.write<std::uint32_t>(0xFFFFFFFFu);
    bs.write<std::uint32_t>(0);
    const auto* begin{ bs.get_raw_ptr() };
    const std::vector<std::byte> data{ begin, begin + bs.get_size() };

    auto& db{ item::ItemDatabase::instance() };
    EXPECT_FALSE(db.parse(data));
    EXPECT_TRUE(db.empty());
}

TEST(ItemDatabaseTest, FailureLeavesDatabaseEmpty)
{
    auto data{ build_items_dat(27, TEST_ITEMS) };
    data.resize(data.size() - 3);

    auto& db{ item::ItemDatabase::instance() };
    EXPECT_FALSE(db.parse(data));
    EXPECT_TRUE(db.empty());
    EXPECT_EQ(db.get_item(0), nullptr);
}
