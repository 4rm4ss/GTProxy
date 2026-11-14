project(GTProxy-Library)

# TLS is provided by LibreSSL from Conan (see src/CMakeLists.txt). Do not let
# cpp-httplib pick up a system OpenSSL as well: linking both mixes two
# incompatible libssl/libcrypto ABIs and crashes on startup (e.g. on Linux).
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

add_subdirectory(cpp-httplib)
add_subdirectory(enet)
