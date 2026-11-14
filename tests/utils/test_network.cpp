#include <gtest/gtest.h>

#include "utils/network.hpp"

using utils::network::classify_host;
using utils::network::HostType;
using utils::network::is_valid_ip_address;

TEST(NetworkTest, AcceptsIpAddressesWithZeroOctets)
{
    EXPECT_TRUE(is_valid_ip_address("127.0.0.1"));
    EXPECT_TRUE(is_valid_ip_address("127.0.0.2"));
    EXPECT_TRUE(is_valid_ip_address("10.0.0.5"));
    EXPECT_TRUE(is_valid_ip_address("0.0.0.0"));
    EXPECT_TRUE(is_valid_ip_address("213.179.209.175"));
    EXPECT_TRUE(is_valid_ip_address("255.255.255.255"));
}

TEST(NetworkTest, RejectsInvalidAddresses)
{
    EXPECT_FALSE(is_valid_ip_address("256.1.1.1"));
    EXPECT_FALSE(is_valid_ip_address("1.2.3"));
    EXPECT_FALSE(is_valid_ip_address("1.2.3.4.5"));
    EXPECT_FALSE(is_valid_ip_address("1..2.3"));
    EXPECT_FALSE(is_valid_ip_address("1.2.3.4a"));
    EXPECT_FALSE(is_valid_ip_address("www.growtopia1.com"));
    EXPECT_FALSE(is_valid_ip_address(""));
}

TEST(NetworkTest, ClassifiesHosts)
{
    EXPECT_EQ(classify_host("127.0.0.2"), HostType::IpAddress);
    EXPECT_EQ(classify_host("www.growtopia2.com"), HostType::Hostname);
}
