#include <gtest/gtest.h>

#include "network/dns_response.hpp"

using network::DnsStatus;
using network::parse_dns_json;

TEST(DnsResponseTest, ReturnsARecord)
{
    const auto result{ parse_dns_json(
        R"({"Status":0,"TC":false,"Answer":[{"name":"www.growtopia2.com.","type":1,"TTL":300,"data":"213.179.209.175"}]})"
    ) };
    EXPECT_EQ(result.status, DnsStatus::NoError);
    EXPECT_EQ(result.ip, "213.179.209.175");
}

TEST(DnsResponseTest, SkipsCnameBeforeARecord)
{
    const auto result{ parse_dns_json(
        R"({"Status":0,"Answer":[)"
        R"({"name":"www.growtopia1.com.","type":5,"TTL":60,"data":"gt.example.net."},)"
        R"({"name":"gt.example.net.","type":1,"TTL":60,"data":"10.0.0.7"}]})"
    ) };
    EXPECT_EQ(result.status, DnsStatus::NoError);
    EXPECT_EQ(result.ip, "10.0.0.7");
}

TEST(DnsResponseTest, NoARecordIsNameError)
{
    const auto result{ parse_dns_json(R"({"Status":0,"Answer":[{"type":5,"data":"alias.example."}]})") };
    EXPECT_EQ(result.status, DnsStatus::NameError);
    EXPECT_TRUE(result.ip.empty());
}

TEST(DnsResponseTest, MissingAnswerIsNameError)
{
    EXPECT_EQ(parse_dns_json(R"({"Status":0})").status, DnsStatus::NameError);
}

TEST(DnsResponseTest, PropagatesDnsErrorStatus)
{
    EXPECT_EQ(parse_dns_json(R"({"Status":3})").status, DnsStatus::NameError);
    EXPECT_EQ(parse_dns_json(R"({"Status":2})").status, DnsStatus::ServerFail);
}

TEST(DnsResponseTest, RejectsInvalidJson)
{
    EXPECT_EQ(parse_dns_json("not json").status, DnsStatus::ServerFail);
    EXPECT_EQ(parse_dns_json("").status, DnsStatus::ServerFail);
}
