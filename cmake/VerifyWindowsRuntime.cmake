cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
get_filename_component(directory "${EXECUTABLE}" DIRECTORY)
foreach(file "${EXECUTABLE}" "${directory}/qt.conf" "${directory}/plugins/platforms/qwindows.dll")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "Missing direct-launch artifact: ${file}. Build the deploy target.")
    endif()
endforeach()
if(NOT DEFINED CLANG_RESOURCE_DESTINATION)
    message(FATAL_ERROR "Missing deployment argument: CLANG_RESOURCE_DESTINATION")
endif()
file(GLOB resource_headers "${CLANG_RESOURCE_DESTINATION}/*/include/stddef.h")
if(NOT resource_headers)
    message(FATAL_ERROR "Missing packaged Clang resource headers in ${CLANG_RESOURCE_DESTINATION}")
endif()

# Deliberately remove the developer environment, including any Qt plugin paths.
set(ENV{PATH} "$ENV{SystemRoot}\\System32;$ENV{SystemRoot}")
foreach(
    variable
    QT_PLUGIN_PATH
    QT_QPA_PLATFORM_PLUGIN_PATH
    QTDIR
    QML_IMPORT_PATH
    QML2_IMPORT_PATH
)
    unset(ENV{${variable}})
endforeach()
set(ENV{QT_QPA_PLATFORM} windows)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")
file(GLOB_RECURSE plugins "${directory}/plugins/*.dll")
set(executables "${EXECUTABLE}")
if(EXISTS "${directory}/clangd.exe")
    list(APPEND executables "${directory}/clangd.exe")
endif()
file(
    GET_RUNTIME_DEPENDENCIES
    EXECUTABLES
    ${executables}
    MODULES
    ${plugins}
    DIRECTORIES
    "${directory}"
    PRE_EXCLUDE_REGEXES
    "^[aA][pP][iI]-[mM][sS]-"
    "^[eE][xX][tT]-[mM][sS]-"
    POST_EXCLUDE_REGEXES
    ".*[\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/].*"
    RESOLVED_DEPENDENCIES_VAR
    resolved
    UNRESOLVED_DEPENDENCIES_VAR
    unresolved
)
if(unresolved)
    message(FATAL_ERROR "Missing packaged dependencies: ${unresolved}")
endif()
file(REAL_PATH "${directory}" runtime_root)
foreach(library IN LISTS resolved)
    file(REAL_PATH "${library}" actual)
    cmake_path(
        IS_PREFIX
        runtime_root
        "${actual}"
        NORMALIZE
        is_packaged
    )
    if(NOT is_packaged)
        message(FATAL_ERROR "Dependency leaked from outside the package: ${actual}")
    endif()
endforeach()

# QApplication initializes the native Windows platform before handling --version.
execute_process(
    COMMAND "${EXECUTABLE}" --version
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 20
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT
   result
   EQUAL
   0
)
    message(FATAL_ERROR "Native launch without MSYS2 PATH failed (${result}): ${output}\n${error}")
endif()
if(NOT output MATCHES "^CodeCairn [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "Unexpected application identity: ${output}\n${error}")
endif()
string(STRIP "${output}" identity)
if(DEFINED EXPECTED_VERSION AND NOT identity STREQUAL "CodeCairn ${EXPECTED_VERSION}")
    message(FATAL_ERROR "Expected CodeCairn ${EXPECTED_VERSION}, got ${identity}")
endif()
message(STATUS "Native Windows startup and dependency closure verified without MSYS2 PATH.")
if(EXISTS "${directory}/clangd.exe")
    execute_process(
        COMMAND "${directory}/clangd.exe" --version
        TIMEOUT 20
        RESULT_VARIABLE clangd_result
        OUTPUT_QUIET
        ERROR_VARIABLE clangd_error
    )
    if(NOT
       clangd_result
       EQUAL
       0
    )
        message(FATAL_ERROR "Packaged clangd failed: ${clangd_error}")
    endif()
    if(NOT DEFINED TEST_DIRECTORY)
        message(FATAL_ERROR "Missing test argument: TEST_DIRECTORY")
    endif()
    file(MAKE_DIRECTORY "${TEST_DIRECTORY}")
    file(REAL_PATH "${TEST_DIRECTORY}" check_directory)
    set(check_source "${check_directory}/resource-check.c")
    file(WRITE "${check_source}" "#include <stddef.h>\nsize_t value;\n")
    file(
        WRITE "${check_directory}/compile_commands.json"
        "[{\"directory\":\"${check_directory}\",\"file\":\"${check_source}\","
        "\"arguments\":[\"clang\",\"-fsyntax-only\",\"${check_source}\"]}]"
    )
    execute_process(
        COMMAND
            "${directory}/clangd.exe" "--check=${check_source}"
            "--compile-commands-dir=${check_directory}" --enable-config=false --log=verbose
        TIMEOUT 30
        RESULT_VARIABLE check_result
        OUTPUT_VARIABLE check_output
        ERROR_VARIABLE check_error
    )
    file(REAL_PATH "${CLANG_RESOURCE_DESTINATION}" resource_root)
    string(FIND "${check_output}${check_error}" "-resource-dir=${resource_root}/" resource_position)
    if(NOT check_result EQUAL 0 OR resource_position EQUAL -1)
        message(FATAL_ERROR "Packaged clangd did not use deployed resource headers: ${check_output}\n${check_error}")
    endif()
endif()
