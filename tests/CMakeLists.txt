project(GTProxy_tests)

find_package(GTest REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(glm REQUIRED)
find_package(magic_enum REQUIRED)
find_package(ZLIB REQUIRED)
find_package(glaze REQUIRED)
find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(LibreSSL REQUIRED)

add_executable(GTProxy_tests
    utils/test_text_parse.cpp
    utils/test_byte_stream.cpp
    utils/test_byte_stream_bounds.cpp
    utils/test_zlib.cpp
    utils/test_network.cpp
    utils/test_dns_response.cpp
    item/test_item_database.cpp
    world/test_world_parsing.cpp
    packet/test_packet_id.cpp
    packet/test_on_send_to_server.cpp
    packet/test_console_message.cpp
    scripting/test_field_binding.cpp
    core/test_certificate.cpp
    core/test_session_log.cpp
    network/test_enet_packet.cpp
    ${CMAKE_SOURCE_DIR}/src/item/item_database.cpp
    ${CMAKE_SOURCE_DIR}/src/world/tile_extra.cpp
    ${CMAKE_SOURCE_DIR}/src/core/certificate.cpp
    ${CMAKE_SOURCE_DIR}/src/core/session_log.cpp)

target_include_directories(GTProxy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/enet/include)

target_link_libraries(GTProxy_tests PRIVATE
    GTest::gtest_main
    enet
    fmt::fmt
    spdlog::spdlog
    glm::glm
    magic_enum::magic_enum
    ZLIB::ZLIB
    glaze::glaze
    lua::lua
    sol2::sol2
    LibreSSL::Crypto)

target_compile_definitions(GTProxy_tests PRIVATE
    SPDLOG_FMT_EXTERNAL
    GTPROXY_RESOURCES_DIR="${CMAKE_SOURCE_DIR}/resources")

include(GoogleTest)
gtest_discover_tests(GTProxy_tests)

# End-to-end harness (fake client + fake game servers), driven by
# tests/e2e/run_e2e.py. Needs port 443, so it is not part of ctest.
option(GTPROXY_BUILD_E2E "Build the end-to-end proxy harness" OFF)
if (GTPROXY_BUILD_E2E)
    add_executable(GTProxy_e2e
        e2e/e2e_harness.cpp
        ${CMAKE_SOURCE_DIR}/src/item/item_database.cpp)

    target_include_directories(GTProxy_e2e PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/lib/enet/include)

    target_link_libraries(GTProxy_e2e PRIVATE
        enet
        fmt::fmt
        spdlog::spdlog
        glm::glm
        magic_enum::magic_enum
        ZLIB::ZLIB)

    target_compile_definitions(GTProxy_e2e PRIVATE
        SPDLOG_FMT_EXTERNAL)
endif ()
