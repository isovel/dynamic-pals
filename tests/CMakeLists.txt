# Host tests for the parts of DynamicPals that need no game. Standalone, since
# the mod's own CMakeLists adds RE-UE4SS unconditionally.
#   cmake -S tests -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host
cmake_minimum_required(VERSION 3.22)
project(DynamicPalsTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1)
FetchContent_MakeAvailable(Catch2)

add_subdirectory(../deps/pal-config-lib pal-config-lib)

add_executable(DynamicPalsTests ../src/SettingsStore.cpp SettingsTest.cpp)
target_include_directories(DynamicPalsTests PRIVATE ../include)
target_link_libraries(DynamicPalsTests PRIVATE PalCfg::Core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(DynamicPalsTests)
