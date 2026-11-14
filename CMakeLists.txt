cmake_minimum_required(VERSION 3.20)
project(TC2WalkmodBootstrap LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this project for 64-bit Windows (x64).")
endif()

add_library(dinput8 SHARED src/dinput8_proxy.cpp src/dinput8.def)
target_compile_features(dinput8 PRIVATE cxx_std_17)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(dinput8 PROPERTIES OUTPUT_NAME "dinput8")
