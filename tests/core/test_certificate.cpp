#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "core/certificate.hpp"

namespace fs = std::filesystem;

namespace {
struct X509Deleter { void operator()(X509* p) const { X509_free(p); } };
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

X509Ptr read_first_cert(const fs::path& path)
{
    std::unique_ptr<FILE, decltype(&std::fclose)> file{ std::fopen(path.string().c_str(), "rb"), &std::fclose };
    return X509Ptr{ file ? PEM_read_X509(file.get(), nullptr, nullptr, nullptr) : nullptr };
}

std::string read_file(const fs::path& path)
{
    std::ifstream in{ path, std::ios::binary };
    return { std::istreambuf_iterator<char>{ in }, {} };
}

class CertificateTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dir_ = fs::temp_directory_path() / ("gtproxy-cert-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
    }

    void TearDown() override { fs::remove_all(dir_); }

    fs::path dir_;
};
}

TEST_F(CertificateTest, CreatesCaAndServerCertificateThatVerifies)
{
    const auto paths{ core::certificate::ensure(dir_) };
    ASSERT_TRUE(fs::exists(paths.ca_cert));
    ASSERT_TRUE(fs::exists(paths.ca_key));
    ASSERT_TRUE(fs::exists(paths.server_cert));
    ASSERT_TRUE(fs::exists(paths.server_key));

    const auto ca{ read_first_cert(paths.ca_cert) };
    const auto server{ read_first_cert(paths.server_cert) };
    ASSERT_TRUE(ca && server);

    // CA: may sign end-entity certificates only.
    EXPECT_EQ(X509_check_ca(ca.get()), 1);
    const auto* constraints{ static_cast<BASIC_CONSTRAINTS*>(X509_get_ext_d2i(ca.get(), NID_basic_constraints, nullptr, nullptr)) };
    ASSERT_NE(constraints, nullptr);
    ASSERT_NE(constraints->pathlen, nullptr);
    EXPECT_EQ(ASN1_INTEGER_get(constraints->pathlen), 0);
    BASIC_CONSTRAINTS_free(const_cast<BASIC_CONSTRAINTS*>(constraints));

    // Server certificate is not a CA and chains to the local CA.
    EXPECT_EQ(X509_check_ca(server.get()), 0);
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store{ X509_STORE_new(), &X509_STORE_free };
    X509_STORE_add_cert(store.get(), ca.get());
    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx{ X509_STORE_CTX_new(), &X509_STORE_CTX_free };
    ASSERT_EQ(X509_STORE_CTX_init(ctx.get(), store.get(), server.get(), nullptr), 1);
    X509_STORE_CTX_set_purpose(ctx.get(), X509_PURPOSE_SSL_SERVER);
    EXPECT_EQ(X509_verify_cert(ctx.get()), 1) << X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx.get()));

    // Valid for the Growtopia host names only.
    EXPECT_EQ(X509_check_host(server.get(), "www.growtopia1.com", 0, 0, nullptr), 1);
    EXPECT_EQ(X509_check_host(server.get(), "www.growtopia2.com", 0, 0, nullptr), 1);
    EXPECT_EQ(X509_check_host(server.get(), "growtopia1.com", 0, 0, nullptr), 1);
    EXPECT_NE(X509_check_host(server.get(), "www.google.com", 0, 0, nullptr), 1);

    // The served chain includes the CA after the server certificate.
    EXPECT_EQ(read_file(paths.server_cert).find(read_file(paths.ca_cert)) != std::string::npos, true);
}

#ifndef _WIN32
TEST_F(CertificateTest, PrivateKeysAreOwnerOnly)
{
    const auto paths{ core::certificate::ensure(dir_) };
    for (const auto& key : { paths.ca_key, paths.server_key }) {
        const auto perms{ fs::status(key).permissions() };
        EXPECT_EQ(perms & (fs::perms::group_all | fs::perms::others_all), fs::perms::none) << key;
    }
}
#endif

TEST_F(CertificateTest, ReusesExistingCertificates)
{
    const auto first{ core::certificate::ensure(dir_) };
    const auto ca{ read_file(first.ca_cert) };
    const auto server{ read_file(first.server_cert) };

    const auto second{ core::certificate::ensure(dir_) };
    EXPECT_EQ(read_file(second.ca_cert), ca);
    EXPECT_EQ(read_file(second.server_cert), server);
}

TEST_F(CertificateTest, RecreatesServerCertificateWithSameCa)
{
    const auto first{ core::certificate::ensure(dir_) };
    const auto ca{ read_file(first.ca_cert) };
    fs::remove(first.server_cert);

    const auto second{ core::certificate::ensure(dir_) };
    EXPECT_EQ(read_file(second.ca_cert), ca);   // the trusted CA stays the same
    EXPECT_TRUE(fs::exists(second.server_cert));
}

TEST_F(CertificateTest, EachMachineGetsItsOwnCa)
{
    const auto a{ core::certificate::ensure(dir_ / "a") };
    const auto b{ core::certificate::ensure(dir_ / "b") };
    EXPECT_NE(read_file(a.ca_key), read_file(b.ca_key));
}
