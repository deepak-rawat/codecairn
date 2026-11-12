cmake_minimum_required(VERSION 3.24)
project(CounterExample LANGUAGES C)

include(CounterTargets.cmake)

option(BUILD_COUNTER "Build the counter example" ON)
if(BUILD_COUNTER)
    add_counter(counter)
    add_executable(counter_demo ../c/main.c)
    target_link_libraries(counter_demo PRIVATE counter)
endif()
