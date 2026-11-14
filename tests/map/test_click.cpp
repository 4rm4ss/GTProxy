#include <map>
#include <string>

#include <gtest/gtest.h>

#include "map/click.hpp"

using map::ClickAction;
using map::ClickError;
using map::MouseButton;
using map::TilePos;
using Params = std::multimap<std::string, std::string>;

namespace {
Params params(
    const std::string& x,
    const std::string& y,
    const std::string& button = "0",
    const std::string& ox = "0",
    const std::string& oy = "0",
    const std::string& zoom = "1",
    const std::string& w = "800",
    const std::string& h = "600"
)
{
    return { { "x", x }, { "y", y }, { "button", button }, { "ox", ox }, { "oy", oy }, { "zoom", zoom }, { "w", w }, { "h", h } };
}
}

TEST(MapClickTest, ParsesAClick)
{
    const auto click{ map::parse_click(params("40.5", "30", "2", "-64", "128", "0.5", "1024", "768")) };
    ASSERT_TRUE(click);
    EXPECT_FLOAT_EQ(click->screen.x, 40.5f);
    EXPECT_FLOAT_EQ(click->screen.y, 30.0f);
    EXPECT_EQ(click->button, MouseButton::Right);
    EXPECT_FLOAT_EQ(click->view.origin.x, -64.0f);
    EXPECT_FLOAT_EQ(click->view.origin.y, 128.0f);
    EXPECT_FLOAT_EQ(click->view.zoom, 0.5f);
    EXPECT_FLOAT_EQ(click->view.size.x, 1024.0f);
    EXPECT_FLOAT_EQ(click->view.size.y, 768.0f);
}

TEST(MapClickTest, RejectsMissingOrBadValues)
{
    auto missing{ params("1", "2") };
    missing.erase("zoom");
    EXPECT_FALSE(map::parse_click(missing));
    EXPECT_FALSE(map::parse_click(params("abc", "2")));
    EXPECT_FALSE(map::parse_click(params("1px", "2")));
    EXPECT_FALSE(map::parse_click(params("", "2")));
    EXPECT_FALSE(map::parse_click(params("1", "2", "3")));
    EXPECT_FALSE(map::parse_click(params("1", "2", "0.5")));
    EXPECT_FALSE(map::parse_click(params("1", "2", "0", "inf")));
}

TEST(MapClickTest, ButtonsToActions)
{
    EXPECT_EQ(map::action_for(MouseButton::Left), ClickAction::Walk);
    EXPECT_EQ(map::action_for(MouseButton::Right), ClickAction::Inspect);
    EXPECT_FALSE(map::action_for(MouseButton::Middle));
}

TEST(MapClickTest, ResolvesCursorToTile)
{
    // 64 world px scrolled, zoom 2: screen x 100 -> world 114 -> tile 3.
    const auto click{ map::parse_click(params("100", "20", "0", "64", "0", "2")) };
    ASSERT_TRUE(click);
    const auto target{ map::resolve_click(*click, { 100, 60 }) };
    ASSERT_TRUE(target);
    EXPECT_EQ(target->tile, (TilePos{ 3, 0 }));
    EXPECT_EQ(target->action, ClickAction::Walk);
}

TEST(MapClickTest, ResolveErrors)
{
    const auto click{ map::parse_click(params("10", "10")) };
    ASSERT_TRUE(click);
    EXPECT_EQ(map::resolve_click(*click, { 0, 0 }).error(), ClickError::NoWorld);

    const auto outside{ map::parse_click(params("700", "10")) };
    ASSERT_TRUE(outside);
    EXPECT_EQ(map::resolve_click(*outside, { 10, 10 }).error(), ClickError::OutsideWorld);

    const auto middle{ map::parse_click(params("10", "10", "1")) };
    ASSERT_TRUE(middle);
    EXPECT_EQ(map::resolve_click(*middle, { 10, 10 }).error(), ClickError::NoAction);

    const auto bad_zoom{ map::parse_click(params("10", "10", "0", "0", "0", "0")) };
    ASSERT_TRUE(bad_zoom);
    EXPECT_EQ(map::resolve_click(*bad_zoom, { 10, 10 }).error(), ClickError::BadView);
}
