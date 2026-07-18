cmake_minimum_required(VERSION 3.16)

project(pair_with_peer VERSION 0.2.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(pwp_options INTERFACE)
target_compile_features(pwp_options INTERFACE cxx_std_17)
target_include_directories(pwp_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pwp_options INTERFACE Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pwp_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
    )
endif()

add_executable(pwp-tracker src/tracker.cpp)
target_link_libraries(pwp-tracker PRIVATE pwp_options)

add_executable(pwp-peer src/peer.cpp)
target_link_libraries(pwp-peer PRIVATE pwp_options)

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol-tests tests/protocol_tests.cpp)
    target_link_libraries(protocol-tests PRIVATE pwp_options)
    add_test(NAME protocol-tests COMMAND protocol-tests)
endif()
