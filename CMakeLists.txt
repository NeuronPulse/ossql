cmake_minimum_required(VERSION 3.14)
project(ossql CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(ossql
  main.cpp
  fs_table.cpp
  proc_table.cpp
  system_table.cpp
  cgroup_table.cpp
  snap.cpp
  functions.cpp
  passwd_table.cpp
  group_table.cpp
  mounts_table.cpp
  net_table.cpp
  fd_table.cpp
  env_table.cpp
)

# Smoke tests: run ossql with a handful of queries and assert on the output.
# Keeps the read-only, thin-view contract honest without a heavyweight harness.
enable_testing()
add_test(NAME smoke COMMAND ${CMAKE_SOURCE_DIR}/tests/check.sh $<TARGET_FILE:ossql>)

# Prefer find_package (uses LIBRARIES/INCLUDE_DIRS, avoids imported-target alias issues);
# fall back to find_library/find_path when the package config is missing.
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  target_include_directories(ossql PRIVATE ${SQLite3_INCLUDE_DIRS})
  target_link_libraries(ossql PRIVATE ${SQLite3_LIBRARIES})
else()
  find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h)
  find_library(SQLITE3_LIBRARY NAMES sqlite3)
  if(NOT SQLITE3_LIBRARY OR NOT SQLITE3_INCLUDE_DIR)
    message(FATAL_ERROR "libsqlite3 未找到，请安装 libsqlite3-dev")
  endif()
  target_include_directories(ossql PRIVATE ${SQLITE3_INCLUDE_DIR})
  target_link_libraries(ossql PRIVATE ${SQLITE3_LIBRARY})
endif()
# std::filesystem needs explicit linking on older toolchains.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(ossql PRIVATE stdc++fs)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  target_compile_options(ossql PRIVATE -O2 -Wall)
else()
  target_compile_options(ossql PRIVATE -g -O0 -Wall -Wextra)
endif()
