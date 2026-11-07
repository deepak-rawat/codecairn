cmake_minimum_required(VERSION 3.24)
if(NOT
   CMAKE_HOST_SYSTEM_NAME
   STREQUAL
   "Linux"
)
    message(FATAL_ERROR "Linux dependency preparation requires Linux.")
endif()
if(NOT DEFINED OUTPUT OR NOT IS_ABSOLUTE "${OUTPUT}")
    message(FATAL_ERROR "Set OUTPUT to an absolute, private dependency directory.")
endif()
find_program(MAKE make REQUIRED)
file(MAKE_DIRECTORY "${OUTPUT}/sources" "${OUTPUT}/share/codecairn/linux-notices/qt")
set(archive "${OUTPUT}/sources/tree-sitter-0.25.10.tar.gz")
file(
    DOWNLOAD "https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.25.10.tar.gz"
    "${archive}"
    EXPECTED_HASH SHA256=ad5040537537012b16ef6e1210a572b927c7cdc2b99d1ee88d44a7dcdc3ff44c
    STATUS status
    TLS_VERIFY ON
)
list(
    GET
    status
    0
    result
)
if(NOT
   result
   EQUAL
   0
)
    message(FATAL_ERROR "Cannot download pinned Tree-sitter source: ${status}")
endif()
file(
    ARCHIVE_EXTRACT
    INPUT
    "${archive}"
    DESTINATION
    "${OUTPUT}/sources"
)
cmake_host_system_information(RESULT cpus QUERY NUMBER_OF_LOGICAL_CORES)
execute_process(
    COMMAND "${MAKE}" -C "${OUTPUT}/sources/tree-sitter-0.25.10" "-j${cpus}" "PREFIX=${OUTPUT}"
            install COMMAND_ERROR_IS_FATAL ANY
)
file(MAKE_DIRECTORY "${OUTPUT}/share/codecairn/linux-notices/tree-sitter")
configure_file(
    "${OUTPUT}/sources/tree-sitter-0.25.10/LICENSE"
    "${OUTPUT}/share/codecairn/linux-notices/tree-sitter/LICENSE" COPYONLY
)
foreach(
    spec
    "LGPL-3.0-only.txt|da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768|https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/LGPL-3.0-only.txt"
    "GPL-3.0-only.txt|8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903|https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/GPL-3.0-only.txt"
    "ICU-LICENSE.txt|f3005e195ff74d8812cc1f182a1c446fab678d70a10e3dada497585befee5416|https://raw.githubusercontent.com/unicode-org/icu/release-73-2/icu4c/LICENSE"
)
    string(
        REPLACE "|"
                ";"
                parts
                "${spec}"
    )
    list(
        GET
        parts
        0
        name
    )
    list(
        GET
        parts
        1
        hash
    )
    list(
        GET
        parts
        2
        url
    )
    file(
        DOWNLOAD "${url}" "${OUTPUT}/share/codecairn/linux-notices/qt/${name}"
        EXPECTED_HASH "SHA256=${hash}"
        STATUS status
        TLS_VERIFY ON
    )
    list(
        GET
        status
        0
        result
    )
    if(NOT
       result
       EQUAL
       0
    )
        message(FATAL_ERROR "Cannot download runtime license ${name}: ${status}")
    endif()
endforeach()
message(STATUS "Pinned Linux dependencies and notices prepared in ${OUTPUT}")
