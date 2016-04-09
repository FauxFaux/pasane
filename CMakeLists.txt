cmake_minimum_required(VERSION 2.8)
project(pasane)

add_executable(pasane pasane.cpp)
target_link_libraries(pasane pulse)
