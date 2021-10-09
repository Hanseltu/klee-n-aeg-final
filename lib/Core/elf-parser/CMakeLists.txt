CMAKE_MINIMUM_REQUIRED(VERSION 3.0)

set(SRC symbols.cc elf_parser.cpp)
set(CMAKE_CXX_COMPILER g++)
set(CMAKEe_CXX_FLAGS "-std=gnu++11")
add_library(elf-parser SHARED ${SRC})
