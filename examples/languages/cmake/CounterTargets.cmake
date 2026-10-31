function(add_counter target)
    add_library(${target} STATIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../c/counter.c")
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../c")
endfunction()
