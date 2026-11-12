project(GTProxy_tests)

find_package(GTest REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(glm REQUIRED)
find_package(magic_enum REQUIRED)

add_executable(GTProxy_tests
    utils/test_text_parse.cpp
    utils/test_byte_stream.cpp
    utils/test_byte_stream_bounds.cpp
    item/test_item_database.cpp
    world/test_world_parsing.cpp
    packet/test_packet_id.cpp
    ${CMAKE_SOURCE_DIR}/src/item/item_database.cpp
    ${CMAKE_SOURCE_DIR}/src/world/tile_extra.cpp)

target_include_directories(GTProxy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(GTProxy_tests PRIVATE
    GTest::gtest_main
    fmt::fmt
    spdlog::spdlog
    glm::glm
    magic_enum::magic_enum)

target_compile_definitions(GTProxy_tests PRIVATE
    SPDLOG_FMT_EXTERNAL
    GTPROXY_RESOURCES_DIR="${CMAKE_SOURCE_DIR}/resources")

include(GoogleTest)
gtest_discover_tests(GTProxy_tests)
