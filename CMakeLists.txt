cmake_minimum_required(VERSION 3.1.0 FATAL_ERROR)
project(pasane)

find_package(yaml-cpp REQUIRED)

add_executable(pasane pasane.cpp parse.cpp)
configure_file(sample.yml sample.yml COPYONLY)

target_compile_features(pasane PRIVATE cxx_range_for)

target_link_libraries(pasane
        pulse
        lib${YAML_CPP_LIBRARIES}.a
        )
