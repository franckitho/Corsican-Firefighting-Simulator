cmake_minimum_required(VERSION 3.24)
project(firesim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Bibliothèque sans dépendance : utilisée par l'outil en ligne de commande et, plus tard, par Unreal.
add_library(firesim_core
  src/fuel_models.cpp
  src/io.cpp
  src/lambert93.cpp
  src/landscape.cpp
  src/rothermel.cpp
  src/spread.cpp
  src/weather.cpp
  src/wind_atlas.cpp)
target_include_directories(firesim_core PUBLIC include)

add_executable(firesim cli/main.cpp)
target_link_libraries(firesim PRIVATE firesim_core)

include(CTest)
if(BUILD_TESTING)
  add_executable(firesim_tests
    tests/test_main.cpp
    tests/test_lambert93.cpp
    tests/test_rothermel.cpp
    tests/test_spread.cpp
    tests/test_weather.cpp
    tests/test_wind_atlas.cpp)
  target_link_libraries(firesim_tests PRIVATE firesim_core)
  target_compile_definitions(firesim_tests PRIVATE FIRESIM_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/reference")
  add_test(NAME firesim_tests COMMAND firesim_tests)
endif()
