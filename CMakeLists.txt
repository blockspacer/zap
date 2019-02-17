﻿cmake_minimum_required (VERSION 3.8)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup()

set(CMAKE_MODULE_PATH ${CONAN_CMAKE_MODULE_PATH} ${CMAKE_MODULE_PATH})
conan_set_find_paths()

find_package(Flatbuffers REQUIRED)
find_package(FlatBuffers REQUIRED)

add_library(zap src/zap.cpp include/bb/zap.hpp include/bb/span.hpp include/bb/flatbuf.hpp include/bb/function_traits.hpp)
target_compile_features(zap PUBLIC cxx_std_17)
target_include_directories(zap PUBLIC "include")

find_package(Boost REQUIRED COMPONENTS system filesystem)

FLATBUFFERS_GENERATE_C_HEADERS(foo schemas/ip_addr.fbs)
message(STATUS ${foo_OUTPUTS})

FLATBUFFERS_GENERATE_C_HEADERS(req_headers schemas/req.fbs)

add_library (ip_handler SHARED src/ip_handler.cpp ${foo_OUTPUTS})
target_link_libraries(ip_handler PUBLIC zap)
target_link_libraries(ip_handler PUBLIC ${CONAN_LIBS})
set_target_properties(ip_handler PROPERTIES CXX_VISIBILITY_PRESET hidden)
set_target_properties(ip_handler PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_definitions(ip_handler PUBLIC _GLIBCXX_USE_CXX11_ABI=0)

add_executable (zaprt src/zap-rt.cpp ${req_headers_OUTPUTS})
target_link_libraries (zaprt PUBLIC zap ${Boost_LIBRARIES} dl pthread)
target_include_directories (zaprt PUBLIC ${Boost_INCLUDE_DIRS})
target_compile_definitions(zaprt PUBLIC _GLIBCXX_USE_CXX11_ABI=0)