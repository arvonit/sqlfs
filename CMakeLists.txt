cmake_minimum_required(VERSION 3.25)

project(sqlfs
    VERSION 1.0
    DESCRIPTION "A user-mode filesystem stored in SQLite, mounted with macFUSE (FSKit)"
    LANGUAGES CXX)

set(CORE_SOURCES
    src/db/database.h src/db/database.cpp
    src/core/fs_result.h src/core/file_attributes.h
    src/core/filesystem.h src/core/filesystem.cpp)

set(FUSE_SOURCES
    src/main.cpp src/fuse/adapter.h src/fuse/adapter.cpp)

# CMake 3.31 doesn't know how to ask AppleClang for C++26, so pass the flag directly
add_compile_options(-std=c++26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # We need this for clangd to get a compile_commands.json

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Debug builds catch memory bugs (AddressSanitizer) and undefined behavior (UBSan)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(WARNINGS
    -Wall -Wextra -Wpedantic -Werror -Wno-error=unused-parameter -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
find_package(SQLite3 REQUIRED)

# Embed schema.sql into the binary as a string constant
file(READ src/db/schema.sql SCHEMA_SQL)
configure_file(src/db/schema.h.in generated/schema.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/db/schema.sql)

add_library(sqlfs_core STATIC ${CORE_SOURCES})
target_include_directories(sqlfs_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sqlfs_core PUBLIC SQLite::SQLite3)
target_compile_options(sqlfs_core PRIVATE ${WARNINGS})

add_executable(sqlfs ${FUSE_SOURCES})
target_link_libraries(sqlfs PRIVATE sqlfs_core PkgConfig::FUSE3)
target_compile_options(sqlfs PRIVATE ${WARNINGS})

include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.8.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(unit_tests tests/unit/filesystem_tests.cpp)
target_link_libraries(unit_tests PRIVATE sqlfs_core Catch2::Catch2WithMain)
target_compile_options(unit_tests PRIVATE ${WARNINGS})

enable_testing()
include(Catch)
catch_discover_tests(unit_tests)
