#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "core/session_log.hpp"

namespace fs = std::filesystem;
using core::session_log::redact;

TEST(SessionLogTest, RedactsLoginPacketSecrets)
{
    const std::string message{
        "[2026-11-14 12:00:00.000] [1] [info] NET_MESSAGE_GENERIC_TEXT (300 bytes):\n"
        "protocol: 225\n"
        "ltoken: eyJhbGciOiJIUzI1NiJ9.secret.token\n"
        "tankIDName: Player\n"
        "tankIDPass: hunter2\n"
        "mac: 02:00:00:00:00:00\n"
        "rid: 0123456789ABCDEF0123456789ABCDEF\n"
        "wk: NONE0\n"
        "hash: -123456\n"
        "game_version: 5.61"
    };
    const auto out{ redact(message) };
    EXPECT_EQ(out.find("eyJhbGci"), std::string::npos);
    EXPECT_EQ(out.find("hunter2"), std::string::npos);
    EXPECT_EQ(out.find("02:00:00"), std::string::npos);
    EXPECT_EQ(out.find("0123456789ABCDEF"), std::string::npos);
    EXPECT_NE(out.find("ltoken: ***"), std::string::npos);
    EXPECT_NE(out.find("protocol: 225"), std::string::npos);
    EXPECT_NE(out.find("tankIDName: Player"), std::string::npos);
    EXPECT_NE(out.find("game_version: 5.61"), std::string::npos);
    EXPECT_NE(out.find("[info] NET_MESSAGE_GENERIC_TEXT"), std::string::npos);
}

TEST(SessionLogTest, RedactsRawTextAndServerData)
{
    EXPECT_EQ(redact("ltoken|abc\nprotocol|225"), "ltoken|***\nprotocol|225");
    EXPECT_EQ(redact("server: 1.2.3.4\nmeta: ABCDEF=="), "server: 1.2.3.4\nmeta: ***");
}

TEST(SessionLogTest, RedactsSendToServerTokens)
{
    const std::string message{
        "[ts] [1] [info] [0] [STRING]: OnSendToServer\n"
        "[1] [SIGNED]: 17092\n"
        "[2] [SIGNED]: 4744778\n"
        "[3] [SIGNED]: 184573909\n"
        "[4] [STRING]: 213.179.209.175|0|02560A89C0D794A90985ADA5C18D8AA6\n"
        "[5] [SIGNED]: 1"
    };
    const auto out{ redact(message) };
    EXPECT_EQ(out.find("4744778"), std::string::npos);
    EXPECT_EQ(out.find("02560A89"), std::string::npos);
    EXPECT_NE(out.find("[1] [SIGNED]: 17092"), std::string::npos);
    EXPECT_NE(out.find("213.179.209.175|0|***"), std::string::npos);
}

TEST(SessionLogTest, LeavesNormalLinesAlone)
{
    const std::string line{ "[2026-11-14 12:00:00.000] [1] [info] Hash matches, loading cached items.dat (hash: 123)" };
    EXPECT_EQ(redact(line), line);
    EXPECT_EQ(redact("action|input\n|text|hello"), "action|input\n|text|hello");
}

namespace {
class SessionLogFileTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() / ("gtproxy-session-test-" + std::string{ ::testing::UnitTest::GetInstance()->current_test_info()->name() });
        fs::remove_all(dir_);
    }
    void TearDown() override { fs::remove_all(dir_); }

    std::size_t count_logs() const
    {
        std::size_t n{ 0 };
        for (const auto& e : fs::directory_iterator{ dir_ }) {
            n += e.path().filename().string().starts_with("session-") ? 1 : 0;
        }
        return n;
    }

    fs::path dir_;
};

std::string read_all(const fs::path& path)
{
    std::ifstream in{ path, std::ios::binary };
    return { std::istreambuf_iterator<char>{ in }, {} };
}
}

TEST_F(SessionLogFileTest, WritesRedactedLines)
{
    const auto sink{ core::session_log::create(dir_) };
    spdlog::logger logger{ "test", sink };
    logger.set_pattern("%v");
    logger.info("hello");
    logger.info("login:\nltoken: secret-value");
    logger.flush();

    const auto text{ read_all(sink->path()) };
    EXPECT_NE(text.find("hello"), std::string::npos);
    EXPECT_NE(text.find("ltoken: ***"), std::string::npos);
    EXPECT_EQ(text.find("secret-value"), std::string::npos);
    EXPECT_TRUE(sink->path().filename().string().starts_with("session-"));
    EXPECT_EQ(sink->path().extension(), ".txt");
}

TEST_F(SessionLogFileTest, KeepsOnlyTheNewestLogs)
{
    fs::create_directories(dir_);
    for (int i = 0; i < 25; ++i) {
        std::ofstream{ dir_ / ("session-2020-01-01_00-00-" + std::to_string(10 + i) + ".txt") } << "old";
    }
    std::ofstream{ dir_ / "notes.txt" } << "not a session log";

    const auto sink{ core::session_log::create(dir_, 20) };
    EXPECT_EQ(count_logs(), 20u);                       // 19 old + the new one
    EXPECT_FALSE(fs::exists(dir_ / "session-2020-01-01_00-00-10.txt"));
    EXPECT_TRUE(fs::exists(dir_ / "session-2020-01-01_00-00-34.txt"));
    EXPECT_TRUE(fs::exists(dir_ / "notes.txt"));
}

TEST_F(SessionLogFileTest, StopsVerboseLoggingAtSizeLimit)
{
    const auto sink{ core::session_log::create(dir_, 20, 1000) };
    spdlog::logger logger{ "test", sink };
    logger.set_pattern("%v");
    for (int i = 0; i < 100; ++i) {
        logger.info("filler line number {} with some padding text", i);
    }
    logger.warn("important warning");
    logger.flush();

    const auto text{ read_all(sink->path()) };
    EXPECT_LT(text.size(), 2000u);
    EXPECT_NE(text.find("size limit reached"), std::string::npos);
    EXPECT_NE(text.find("important warning"), std::string::npos);
}
