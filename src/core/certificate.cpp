#include "certificate.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <spdlog/spdlog.h>

namespace core::certificate {
namespace {
constexpr std::array GROWTOPIA_HOSTS{
    "www.growtopia1.com",
    "www.growtopia2.com",
    "growtopia1.com",
    "growtopia2.com",
};

constexpr long DAY{ 24L * 60L * 60L };
constexpr long CA_VALIDITY{ 10L * 365L * DAY };
constexpr long SERVER_VALIDITY{ 825L * DAY };
constexpr long RENEW_BEFORE{ 30L * DAY };

struct PkeyDeleter { void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); } };
struct X509Deleter { void operator()(X509* p) const { X509_free(p); } };
struct FileDeleter { void operator()(FILE* p) const { std::fclose(p); } };
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using FilePtr = std::unique_ptr<FILE, FileDeleter>;

[[noreturn]] void fail(const std::string& what)
{
    throw std::runtime_error{ "Certificate generation failed: " + what };
}

PkeyPtr generate_key()
{
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx{
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
        &EVP_PKEY_CTX_free
    };
    EVP_PKEY* key{ nullptr };
    if (
        !ctx
        || EVP_PKEY_keygen_init(ctx.get()) <= 0
        || EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0
        || EVP_PKEY_keygen(ctx.get(), &key) <= 0
    ) {
        fail("RSA key generation");
    }
    return PkeyPtr{ key };
}

void add_extension(X509* cert, X509* issuer, const int nid, const std::string& value)
{
    X509V3_CTX ctx{};
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext{ X509V3_EXT_conf_nid(nullptr, &ctx, nid, const_cast<char*>(value.c_str())) };
    if (!ext || X509_add_ext(cert, ext, -1) != 1) {
        X509_EXTENSION_free(ext);
        fail("extension " + value);
    }
    X509_EXTENSION_free(ext);
}

X509Ptr new_certificate(EVP_PKEY* key, const std::string& common_name, const long validity)
{
    X509Ptr cert{ X509_new() };
    if (!cert) {
        fail("X509_new");
    }

    X509_set_version(cert.get(), 2); // v3

    std::array<unsigned char, 16> serial{};
    RAND_bytes(serial.data(), static_cast<int>(serial.size()));
    serial[0] &= 0x7F; // keep it positive
    BIGNUM* bn{ BN_bin2bn(serial.data(), static_cast<int>(serial.size()), nullptr) };
    if (!bn || !BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert.get()))) {
        BN_free(bn);
        fail("serial number");
    }
    BN_free(bn);

    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -DAY);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), validity);
    X509_set_pubkey(cert.get(), key);

    X509_NAME* name{ X509_get_subject_name(cert.get()) };
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0
    );
    X509_NAME_add_entry_by_txt(
        name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("GTProxy"), -1, -1, 0
    );
    return cert;
}

void write_pem(const std::filesystem::path& path, const std::function<bool(FILE*)>& writer, const bool secret)
{
    FilePtr file{ std::fopen(path.string().c_str(), "wb") };
    if (!file || !writer(file.get())) {
        fail("cannot write " + path.string());
    }

    if (secret) {
        std::error_code ec;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace,
            ec
        );
    }
}

X509Ptr read_cert(const std::filesystem::path& path)
{
    FilePtr file{ std::fopen(path.string().c_str(), "rb") };
    return X509Ptr{ file ? PEM_read_X509(file.get(), nullptr, nullptr, nullptr) : nullptr };
}

PkeyPtr read_key(const std::filesystem::path& path)
{
    FilePtr file{ std::fopen(path.string().c_str(), "rb") };
    return PkeyPtr{ file ? PEM_read_PrivateKey(file.get(), nullptr, nullptr, nullptr) : nullptr };
}

bool expires_soon(X509* cert)
{
    time_t limit{ std::time(nullptr) + RENEW_BEFORE };
    return X509_cmp_time(X509_get0_notAfter(cert), &limit) < 0;
}

void create_ca(const Paths& paths)
{
    const auto key{ generate_key() };
    auto cert{ new_certificate(key.get(), CA_COMMON_NAME, CA_VALIDITY) };
    X509_set_issuer_name(cert.get(), X509_get_subject_name(cert.get()));

    // pathlen:0 - this CA can sign server certificates but no further CAs.
    add_extension(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
    add_extension(cert.get(), cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign,digitalSignature");
    add_extension(cert.get(), cert.get(), NID_subject_key_identifier, "hash");

    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) {
        fail("signing CA");
    }

    write_pem(paths.ca_key, [&](FILE* f) {
        return PEM_write_PrivateKey(f, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1;
    }, true);
    write_pem(paths.ca_cert, [&](FILE* f) { return PEM_write_X509(f, cert.get()) == 1; }, false);
}

void create_server_certificate(const Paths& paths)
{
    const auto ca_cert{ read_cert(paths.ca_cert) };
    const auto ca_key{ read_key(paths.ca_key) };
    if (!ca_cert || !ca_key) {
        fail("cannot read the local CA");
    }

    const auto key{ generate_key() };
    auto cert{ new_certificate(key.get(), GROWTOPIA_HOSTS[0], SERVER_VALIDITY) };
    X509_set_issuer_name(cert.get(), X509_get_subject_name(ca_cert.get()));

    std::string san;
    for (const auto* host : GROWTOPIA_HOSTS) {
        san += san.empty() ? "DNS:" : ",DNS:";
        san += host;
    }

    add_extension(cert.get(), ca_cert.get(), NID_basic_constraints, "critical,CA:FALSE");
    add_extension(cert.get(), ca_cert.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment");
    add_extension(cert.get(), ca_cert.get(), NID_ext_key_usage, "serverAuth");
    add_extension(cert.get(), ca_cert.get(), NID_subject_alt_name, san);
    add_extension(cert.get(), ca_cert.get(), NID_subject_key_identifier, "hash");
    add_extension(cert.get(), ca_cert.get(), NID_authority_key_identifier, "keyid:always");

    if (X509_sign(cert.get(), ca_key.get(), EVP_sha256()) == 0) {
        fail("signing server certificate");
    }

    write_pem(paths.server_key, [&](FILE* f) {
        return PEM_write_PrivateKey(f, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1;
    }, true);
    // Serve the chain (server certificate followed by the CA).
    write_pem(paths.server_cert, [&](FILE* f) {
        return PEM_write_X509(f, cert.get()) == 1 && PEM_write_X509(f, ca_cert.get()) == 1;
    }, false);
}

int run(const std::string& command)
{
    spdlog::info("Running: {}", command);
    return std::system(command.c_str());
}
}

Paths paths_in(const std::filesystem::path& directory)
{
    return Paths{
        directory / "ca.pem",
        directory / "ca.key.pem",
        directory / "server.pem",
        directory / "server.key.pem",
    };
}

Paths ensure(const std::filesystem::path& directory)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    const auto paths{ paths_in(directory) };

    const auto ca{ read_cert(paths.ca_cert) };
    if (!ca || !read_key(paths.ca_key) || expires_soon(ca.get())) {
        spdlog::info("Creating a local CA for this machine in {}", directory.string());
        create_ca(paths);
        create_server_certificate(paths);
        spdlog::warn(
            "New local CA created. Trust it once so the game accepts the proxy: run the proxy with "
            "--install-cert (or import {} into your trusted root certificates)",
            paths.ca_cert.string()
        );
        return paths;
    }

    const auto server{ read_cert(paths.server_cert) };
    if (!server || !read_key(paths.server_key) || expires_soon(server.get())) {
        spdlog::info("Creating the proxy HTTPS certificate in {}", directory.string());
        create_server_certificate(paths);
    }

    return paths;
}

bool install_ca(const Paths& paths)
{
    const std::string ca{ std::filesystem::absolute(paths.ca_cert).string() };
#ifdef _WIN32
    // Current user's Root store; Windows asks for confirmation.
    return run("certutil -user -addstore Root \"" + ca + "\"") == 0;
#elif defined(__APPLE__)
    return run("security add-trusted-cert -r trustRoot -k \"$HOME/Library/Keychains/login.keychain-db\" \"" + ca + "\"") == 0;
#else
    spdlog::warn("Automatic install is not supported here; add {} to your system or browser trust store", ca);
    return false;
#endif
}

bool uninstall_ca()
{
#ifdef _WIN32
    return run(std::string{ "certutil -user -delstore Root \"" } + CA_COMMON_NAME + "\"") == 0;
#elif defined(__APPLE__)
    return run(std::string{ "security delete-certificate -c \"" } + CA_COMMON_NAME + "\"") == 0;
#else
    spdlog::warn("Remove \"{}\" from your trust store manually", CA_COMMON_NAME);
    return false;
#endif
}
}
