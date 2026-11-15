cmake_minimum_required(VERSION 3.21)
project(kvx CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)

option(KVX_SANITIZE "address,undefined | thread" "")
if(KVX_SANITIZE)
  add_compile_options(-fsanitize=${KVX_SANITIZE} -fno-omit-frame-pointer -O1 -g)
  add_link_options(-fsanitize=${KVX_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(kvx_core STATIC
  src/http/http.cpp
  src/http/handler.cpp
  src/store/store.cpp
  src/net/event_loop.cpp)
target_include_directories(kvx_core PUBLIC src)
target_link_libraries(kvx_core PUBLIC Threads::Threads)

add_executable(kvx src/main.cpp)
target_link_libraries(kvx PRIVATE kvx_core)

enable_testing()
add_executable(test_http tests/test_http.cpp)
target_link_libraries(test_http PRIVATE kvx_core)
add_test(NAME http_unit COMMAND test_http)
add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE kvx_core)
add_test(NAME store_unit COMMAND test_store)

add_executable(bench_store bench/bench_store.cpp)
target_link_libraries(bench_store PRIVATE kvx_core)
add_test(NAME functional
  COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/functional.py $<TARGET_FILE:kvx>)
if(KVX_SANITIZE)
  set_tests_properties(functional PROPERTIES ENVIRONMENT KVX_SANITIZED=1)
endif()
