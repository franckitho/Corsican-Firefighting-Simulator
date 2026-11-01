# Outil ponctuel : régénère tests/reference/behave_surface.csv à partir des sources de behave.
#   git clone https://github.com/firelab/behave
#   cmake -S . -B build -DBEHAVE_SRC=<chemin>/behave/src/behave && cmake --build build
cmake_minimum_required(VERSION 3.24)
project(behave_reference LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT BEHAVE_SRC)
  message(FATAL_ERROR "Indiquer -DBEHAVE_SRC=<chemin vers behave/src/behave>")
endif()

file(GLOB BEHAVE_SOURCES "${BEHAVE_SRC}/*.cpp")
list(FILTER BEHAVE_SOURCES EXCLUDE REGEX "client\\.cpp$")

add_executable(behave_reference main.cpp ${BEHAVE_SOURCES})
target_include_directories(behave_reference PRIVATE "${BEHAVE_SRC}")
if(MSVC)
  target_compile_definitions(behave_reference PRIVATE _USE_MATH_DEFINES)
  target_compile_options(behave_reference PRIVATE /w)
endif()
