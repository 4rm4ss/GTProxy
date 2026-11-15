#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "item/item_database.hpp"
#include "map/bridge.hpp"
#include "map/snapshot.hpp"
#include "packet/game/world.hpp"
#include "player/player.hpp"
#include "world/world.hpp"

namespace {
void make_world(world::World& world)
{
    world.clear();
    auto& tiles{ world.get_tile_map() };
    tiles.get_size() = { 2, 1 };
    tiles.get_tiles().resize(2);
    tiles.get_tiles()[1].foreground = 2;
    tiles.get_tiles()[0].background = 14;

    packet::game::OnSpawn spawn{};
    spawn.net_id = 7;
    spawn.name = "`wHero``";
    spawn.position = { 32, 0 };
    spawn.type = "local";
    world.add_player(player::Player::from_on_spawn(spawn));

    world::Object gem{};
    gem.item_id = 112;
    gem.amount = 3;
    gem.pos = { 8.0f, 4.0f };
    world.get_object_map().get_objects().push_back(gem);
}
}

TEST(MapSnapshotTest, TilesJson)
{
    item::ItemDatabase::instance().clear();
    world::World world{};
    make_world(world);
    const map::Grid grid{ { 2, 1 }, { map::Cell::Open, map::Cell::Solid } };

    const auto json{ map::tiles_json(4, world, grid) };
    EXPECT_NE(json.find("\"rev\":4"), std::string::npos) << json;
    EXPECT_NE(json.find("\"width\":2,\"height\":1"), std::string::npos) << json;
    EXPECT_NE(json.find("\"fg\":[0,2]"), std::string::npos) << json;
    EXPECT_NE(json.find("\"bg\":[14,0]"), std::string::npos) << json;
    EXPECT_NE(json.find("\"cell\":[0,1]"), std::string::npos) << json;
    // Without items.dat: a stable placeholder name and colour per item.
    EXPECT_NE(json.find("\"2\":{\"name\":\"Item 2\",\"color\":\"#"), std::string::npos) << json;
    EXPECT_EQ(map::item_color(2), map::item_color(2));
}

TEST(MapSnapshotTest, TilesJsonWithoutAWorld)
{
    world::World world{};
    world.clear();
    const auto json{ map::tiles_json(1, world, map::Grid{}) };
    EXPECT_NE(json.find("\"width\":0"), std::string::npos) << json;
    EXPECT_NE(json.find("\"fg\":[]"), std::string::npos) << json;
}

TEST(MapSnapshotTest, StateJson)
{
    item::ItemDatabase::instance().clear();
    world::World world{};
    make_world(world);

    map::StateView view{};
    view.connected = true;
    view.in_world = true;
    view.world_name = "TEST";
    view.tiles_revision = 9;
    view.walk.walking = true;
    view.walk.path = { { 1, 0 }, { 0, 0 } };
    view.walk.target = map::TilePos{ 0, 0 };
    view.status = "Walking to 0, 0 (1 tile).";

    const auto json{ map::state_json(view, world) };
    EXPECT_NE(json.find("\"world\":\"TEST\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"tiles_rev\":9"), std::string::npos) << json;
    // Colour codes are removed from names.
    EXPECT_NE(json.find("{\"id\":7,\"name\":\"Hero\",\"x\":32,\"y\":0,\"tx\":1,\"ty\":0,\"me\":true}"), std::string::npos) << json;
    EXPECT_NE(json.find("{\"item\":112,\"amount\":3,\"x\":8,\"y\":4}"), std::string::npos) << json;
    EXPECT_NE(json.find("\"walking\":true,\"path\":[{\"x\":1,\"y\":0},{\"x\":0,\"y\":0}]"), std::string::npos) << json;
    EXPECT_NE(json.find("\"has_target\":true,\"target\":{\"x\":0,\"y\":0}"), std::string::npos) << json;
}

TEST(MapSnapshotTest, StateJsonOutsideAWorldHasNoPlayers)
{
    world::World world{};
    make_world(world);
    const auto json{ map::state_json(map::StateView{}, world) };
    EXPECT_NE(json.find("\"players\":[]"), std::string::npos) << json;
    EXPECT_NE(json.find("\"in_world\":false"), std::string::npos) << json;
}

TEST(MapSnapshotTest, ResultJsonEscapesText)
{
    const auto json{ map::result_json(false, "walk", std::nullopt, "Can't \"go\"\nthere") };
    EXPECT_EQ(json, R"({"ok":false,"action":"walk","has_tile":false,"tile":{"x":0,"y":0},"message":"Can't \"go\"\nthere","reach":"","detail":""})");

    const auto reach{ map::result_json(true, "reach", map::TilePos{ 1, 2 }, "Цель доступна", "reachable", "3 шага") };
    EXPECT_EQ(reach, R"({"ok":true,"action":"reach","has_tile":true,"tile":{"x":1,"y":2},"message":"Цель доступна","reach":"reachable","detail":"3 шага"})");
}

TEST(MapBridgeTest, RequestsReachTheMainLoopAndBack)
{
    map::Bridge bridge{};
    map::Click click{};
    click.screen = { 5.0f, 6.0f };

    auto reply{ bridge.submit(map::Bridge::Request::Kind::Click, click) };
    ASSERT_TRUE(reply);

    // The "HTTP thread" waits while the "main loop" answers.
    std::thread main_loop{ [&] {
        auto requests{ bridge.take_requests() };
        ASSERT_EQ(requests.size(), 1u);
        EXPECT_EQ(requests[0].kind, map::Bridge::Request::Kind::Click);
        EXPECT_FLOAT_EQ(requests[0].click.screen.x, 5.0f);
        requests[0].reply.set_value("{\"ok\":true}");
    } };
    ASSERT_EQ(reply->wait_for(std::chrono::seconds{ 2 }), std::future_status::ready);
    EXPECT_EQ(reply->get(), "{\"ok\":true}");
    main_loop.join();

    EXPECT_TRUE(bridge.take_requests().empty());
}

TEST(MapBridgeTest, LimitsPendingRequests)
{
    map::Bridge bridge{};
    std::vector<std::future<std::string>> replies;
    for (std::size_t i{ 0 }; i < map::Bridge::MAX_PENDING; ++i) {
        auto reply{ bridge.submit(map::Bridge::Request::Kind::Stop) };
        ASSERT_TRUE(reply);
        replies.push_back(std::move(*reply));
    }
    EXPECT_FALSE(bridge.submit(map::Bridge::Request::Kind::Stop));
    EXPECT_EQ(bridge.take_requests().size(), map::Bridge::MAX_PENDING);
}

TEST(MapBridgeTest, PublishedSnapshotsAndViewers)
{
    map::Bridge bridge{};
    EXPECT_EQ(*bridge.state(), "{}");
    EXPECT_FALSE(bridge.viewed_within(std::chrono::seconds{ 5 }));

    bridge.publish_state("{\"a\":1}");
    bridge.publish_tiles("{\"b\":2}");
    bridge.mark_viewed();
    EXPECT_EQ(*bridge.state(), "{\"a\":1}");
    EXPECT_EQ(*bridge.tiles(), "{\"b\":2}");
    EXPECT_TRUE(bridge.viewed_within(std::chrono::seconds{ 5 }));
}
