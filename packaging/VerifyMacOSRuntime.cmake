cmake_minimum_required(VERSION 3.24)
if(NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR "macOS runtime validation requires macOS.")
endif()
foreach(required APP VERSION TEST_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing argument: ${required}")
    endif()
endforeach()
set(ENV{PATH} "/usr/bin:/bin:/usr/sbin:/sbin")
set(ENV{QT_QPA_PLATFORM} cocoa)
foreach(
    variable
    QT_PLUGIN_PATH
    QT_QPA_PLATFORM_PLUGIN_PATH
    QTDIR
    DYLD_LIBRARY_PATH
    DYLD_FRAMEWORK_PATH
)
    unset(ENV{${variable}})
endforeach()
foreach(name codecairn cairn clangd)
    set(executable "${APP}/Contents/MacOS/${name}")
    execute_process(
        COMMAND /usr/bin/lipo -archs "${executable}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE architecture
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE error
    )
    if(NOT
       result
       EQUAL
       0
       OR NOT
          architecture
          STREQUAL
          "arm64"
    )
        message(FATAL_ERROR "${name} is not an ARM64-only executable: ${architecture}\n${error}")
    endif()
    execute_process(
        COMMAND "${executable}" --version
        RESULT_VARIABLE result
        TIMEOUT 20
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT
       result
       EQUAL
       0
    )
        message(FATAL_ERROR "${name} cannot run without Homebrew PATH: ${output}\n${error}")
    endif()
    string(STRIP "${output}" output)
    if(NOT
       name
       STREQUAL
       "clangd"
       AND NOT
           output
           STREQUAL
           "CodeCairn ${VERSION}"
    )
        message(FATAL_ERROR "Unexpected ${name} version: ${output}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${TEST_DIRECTORY}")
set(source "${TEST_DIRECTORY}/resource-check.c")
file(WRITE "${source}" "#include <stddef.h>\nsize_t value;\n")
file(WRITE "${TEST_DIRECTORY}/compile_commands.json"
     "[{\"directory\":\"${TEST_DIRECTORY}\",\"file\":\"${source}\","
     "\"arguments\":[\"clang\",\"-fsyntax-only\",\"${source}\"]}]\n"
)
execute_process(
    COMMAND "${APP}/Contents/MacOS/clangd" "--check=${source}"
            "--compile-commands-dir=${TEST_DIRECTORY}" --enable-config=false --log=verbose
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 30
)
string(FIND "${output}${error}" "-resource-dir=${APP}/Contents/lib/clang/" resource_position)
if(NOT
   result
   EQUAL
   0
   OR resource_position EQUAL -1
)
    message(FATAL_ERROR "Bundled clangd did not use packaged resource headers: ${output}\n${error}")
endif()
message(STATUS "Native ARM64 macOS startup and clangd resources verified without Homebrew PATH.")
