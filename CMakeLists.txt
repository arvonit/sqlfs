cmake_minimum_required(VERSION 3.16)
project(sqlfs C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -O2)

find_package(PkgConfig REQUIRED)

# SQLite: prefer Homebrew version on macOS for latest sqlite3
if(APPLE)
    set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/sqlite/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

# FUSE: macFUSE framework on macOS, fuse3 via pkg-config on Linux
if(APPLE)
    set(FUSE_INCLUDE_DIR "/Library/Frameworks/macFUSE.framework/Headers")
    set(FUSE_LIBRARIES   "-framework macFUSE")
    if(NOT EXISTS "${FUSE_INCLUDE_DIR}/fuse.h")
        message(FATAL_ERROR
            "macFUSE headers not found at ${FUSE_INCLUDE_DIR}.\n"
            "Install macFUSE 5.1.x from https://macfuse.github.io/")
    endif()
else()
    pkg_check_modules(FUSE3 REQUIRED fuse3)
    set(FUSE_INCLUDE_DIR ${FUSE3_INCLUDE_DIRS})
    set(FUSE_LIBRARIES   ${FUSE3_LIBRARIES})
endif()

add_executable(sqlfs
    src/main.c
    src/db.c
    src/fsops.c
)

target_include_directories(sqlfs PRIVATE
    src/
    ${SQLITE3_INCLUDE_DIRS}
    ${FUSE_INCLUDE_DIR}
)

target_link_libraries(sqlfs PRIVATE
    ${SQLITE3_LIBRARIES}
    ${FUSE_LIBRARIES}
    pthread
)

target_link_directories(sqlfs PRIVATE
    ${SQLITE3_LIBRARY_DIRS}
)

if(NOT APPLE)
    target_link_directories(sqlfs PRIVATE ${FUSE3_LIBRARY_DIRS})
endif()

install(TARGETS sqlfs DESTINATION bin)
