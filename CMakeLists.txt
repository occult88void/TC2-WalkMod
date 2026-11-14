cmake_minimum_required(VERSION 3.20)
project(TC2WalkModBootstrap LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project must be built on Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Crew 2 requires the x64 build of this proxy.")
endif()

add_library(dinput8 SHARED
    src/dinput8_proxy.cpp
    src/dinput8.def
)

target_compile_features(dinput8 PRIVATE cxx_std_17)
target_compile_definitions(dinput8 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    DIRECTINPUT_VERSION=0x0800
    UNICODE
    _UNICODE
)

# bcrypt.lib is required for the BCrypt* SHA-256 functions used below.
target_link_libraries(dinput8 PRIVATE bcrypt)

set_target_properties(dinput8 PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(dinput8 PRIVATE /W4 /permissive- /EHsc)
endif()
