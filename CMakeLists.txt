cmake_minimum_required(VERSION 3.25)
project(InputTrace VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Core)

add_library(sqlite3 STATIC dependencies/sqlite-amalgamation/sqlite-amalgamation-3540000/sqlite3.c)
target_include_directories(sqlite3 PUBLIC dependencies/sqlite-amalgamation/sqlite-amalgamation-3540000)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_WAL_SYNCHRONOUS=1)

add_executable(inputtrace
    src/app/main.cpp
    src/capture/collector.cpp
    src/storage/storage.cpp)
target_include_directories(inputtrace PRIVATE include)
target_link_libraries(inputtrace PRIVATE Qt6::Core sqlite3 user32 kernel32 wtsapi32 psapi)
target_compile_definitions(inputtrace PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(inputtrace PRIVATE -Wall -Wextra -Wpedantic)
if(MINGW)
    target_link_options(inputtrace PRIVATE -static-libgcc -static-libstdc++)
endif()
enable_testing()
add_test(NAME backend_self_test COMMAND inputtrace --db ${CMAKE_BINARY_DIR}/ctest-selftest.db --self-test)
