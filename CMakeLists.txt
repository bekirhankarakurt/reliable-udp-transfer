cmake_minimum_required(VERSION 3.16)
project(reliable_udp_transfer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RUDP_BUILD_TESTS "Build the unit tests" ON)
option(RUDP_BUILD_BENCH "Build the throughput benchmark" ON)
option(RUDP_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RUDP_WERROR "Treat compiler warnings as errors" OFF)

find_package(Threads REQUIRED)

add_library(rudp_flags INTERFACE)
target_compile_options(rudp_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
if(RUDP_WERROR)
  target_compile_options(rudp_flags INTERFACE -Werror)
endif()
if(RUDP_SANITIZE)
  target_compile_options(rudp_flags INTERFACE -fsanitize=address,undefined
                         -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(rudp_flags INTERFACE -fsanitize=address,undefined)
endif()

add_library(rudp STATIC
  src/channel.cpp
  src/packet.cpp
  src/progress.cpp
  src/receiver.cpp
  src/sender.cpp
  src/sha256.cpp
  src/transfer.cpp
  src/udp_socket.cpp)
target_include_directories(rudp PUBLIC include)
target_link_libraries(rudp PUBLIC Threads::Threads PRIVATE rudp_flags)

add_executable(rudp_cli apps/rudp.cpp)
set_target_properties(rudp_cli PROPERTIES OUTPUT_NAME rudp)
target_link_libraries(rudp_cli PRIVATE rudp rudp_flags)

if(RUDP_BUILD_BENCH)
  add_executable(rudp_bench bench/bench.cpp)
  target_link_libraries(rudp_bench PRIVATE rudp rudp_flags)
endif()

if(RUDP_BUILD_TESTS)
  enable_testing()
  foreach(name sha256 packet protocol progress transfer udp)
    add_executable(test_${name} tests/test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE rudp rudp_flags)
    add_test(NAME ${name} COMMAND test_${name})
  endforeach()
endif()
