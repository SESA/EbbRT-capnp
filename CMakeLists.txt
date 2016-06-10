cmake_minimum_required(VERSION 2.6)

# This must be before the project() command so cmake finds our platform
# definition
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")

# This skips compiler verification which will fail because we haven't finished
# installing the necessary libraries
set(CMAKE_CXX_COMPILER_WORKS 1)

project(Acpica CXX)

# Initialize CXXFLAGS.
set(CMAKE_CXX_FLAGS                "-Wall -Werror -std=gnu++14")
set(CMAKE_CXX_FLAGS_DEBUG          "-O0 -g")
set(CMAKE_CXX_FLAGS_MINSIZEREL     "-Os -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE        "-O4 -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

include_directories(include)

add_library(capnp STATIC
  ${CMAKE_SOURCE_DIR}/src/capnp/arena.c++
  ${CMAKE_SOURCE_DIR}/src/capnp/blob.c++
  ${CMAKE_SOURCE_DIR}/src/capnp/layout.c++
  ${CMAKE_SOURCE_DIR}/src/capnp/serialize.c++
  ${CMAKE_SOURCE_DIR}/src/capnp/message.c++
  )

add_library(kj STATIC
  ${CMAKE_SOURCE_DIR}/src/kj/io.c++
  ${CMAKE_SOURCE_DIR}/src/kj/string.c++
  ${CMAKE_SOURCE_DIR}/src/kj/debug.c++
  ${CMAKE_SOURCE_DIR}/src/kj/mutex.c++
  ${CMAKE_SOURCE_DIR}/src/kj/array.c++
  ${CMAKE_SOURCE_DIR}/src/kj/common.c++
  ${CMAKE_SOURCE_DIR}/src/kj/exception.c++
  )

install(TARGETS capnp kj
  ARCHIVE DESTINATION lib)

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/capnp DESTINATION include
  FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/kj DESTINATION include
  FILES_MATCHING PATTERN "*.h")
