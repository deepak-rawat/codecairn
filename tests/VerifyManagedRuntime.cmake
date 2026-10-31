cmake_minimum_required(VERSION 3.24)

foreach(required EXECUTABLE RUNTIME_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing test argument: ${required}")
    endif()
endforeach()
get_filename_component(directory "${EXECUTABLE}" DIRECTORY)
file(
    GLOB unexpected
    "${directory}/*.dll" "${directory}/qt.conf" "${directory}/plugins"
    "${RUNTIME_ROOT}/lib/clang" "${directory}/../Resources/lib/clang" "${directory}/../lib/clang"
    "${directory}/../Frameworks" "${directory}/../PlugIns"
)
if(unexpected)
    message(FATAL_ERROR "Package-managed output contains bundled runtime files: ${unexpected}")
endif()
execute_process(
    COMMAND "${EXECUTABLE}" --version
    TIMEOUT 20
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Package-managed startup failed (${result}): ${output}\n${error}")
endif()
