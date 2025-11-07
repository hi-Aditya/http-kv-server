cmake_minimum_required(VERSION 3.18)
project(kvserver CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# ---- header-only deps ----
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(Threads REQUIRED)

# ---- MySQL client (Arch: mariadb-libs) ----
# Look for libmysqlclient and headers (usually in /usr/include/mysql)
find_library(MYSQLCLIENT_LIBRARY NAMES mysqlclient libmysqlclient REQUIRED)
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
  HINTS /usr/include/mysql /usr/local/include/mysql
  REQUIRED)

# ---- your targets ----
add_executable(kvserver
  src/kv_server.cpp       # <- make sure this matches your actual filename
  src/DB.cpp
  src/util.cpp            # remove if you don't have it
)

target_include_directories(kvserver
  PRIVATE
    ${PROJECT_SOURCE_DIR}/include   # where DB.hpp & Log.hpp are
    ${MYSQL_INCLUDE_DIR}            # mysql/mysql.h
)

target_link_libraries(kvserver
  PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    ${MYSQLCLIENT_LIBRARY}
    Threads::Threads
)

# Optional: small tool
add_executable(loadgen tools/loadgen.cpp)
target_link_libraries(loadgen PRIVATE httplib::httplib nlohmann_json::nlohmann_json Threads::Threads)
target_include_directories(loadgen PRIVATE ${PROJECT_SOURCE_DIR}/include)

