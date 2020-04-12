cmake_minimum_required(VERSION 3.10)
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "-g -O0")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/EHa>")

find_package(Boost 1.66.0 REQUIRED COMPONENTS thread iostreams filesystem exception)
find_package(leveldb CONFIG REQUIRED)
find_package(Threads REQUIRED)
if(WIN32)
find_package(lz4 CONFIG)
else()
link_directories(/usr/local/lib)
endif()

include_directories(include)
include_directories(json/include)
include_directories(cxxopts/include)
include_directories(cpptoml/include)
include_directories(${Boost_INCLUDE_DIRS})

if(WIN32)
set(libs
    wsock32
    ws2_32
    lz4::lz4)
else()
set(libs
    lz4)
endif()

project(syncclient)
set(cli_src syncclient/src/main.cpp)
add_executable(syncclient ${cli_src})
target_link_libraries(syncclient
    PRIVATE
        ${Boost_LIBRARIES}
        ${libs})

project(syncserver)
set(serv_src syncserver/src/main.cpp)
add_executable(syncserver ${serv_src})
target_link_libraries(syncserver
    PRIVATE
        ${Boost_LIBRARIES}
        ${libs})

project(synclocal)
set(loc_src synclocal/src/main.cpp)
add_executable(synclocal ${loc_src})
target_link_libraries(synclocal
    PRIVATE
        ${Boost_LIBRARIES}
        leveldb::leveldb
        ${libs})
