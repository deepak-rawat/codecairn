cmake_minimum_required(VERSION 3.24)

function(run)
    execute_process(COMMAND ${ARGV} COMMAND_ECHO STDOUT RESULT_VARIABLE result)
    if(NOT
       result
       EQUAL
       0
    )
        message(FATAL_ERROR "Release command failed (${result}): ${ARGV}")
    endif()
endfunction()

get_filename_component(SOURCE "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(STRINGS "${SOURCE}/CMakeLists.txt" version_line REGEX "^    VERSION [0-9]+\\.")
string(
    REGEX
    REPLACE "^    VERSION ([0-9.]+).*$"
            "\\1"
            VERSION
            "${version_line}"
)
if(NOT
   VERSION
   MATCHES
   "^[0-9]+\\.[0-9]+\\.[0-9]+$"
)
    message(FATAL_ERROR "Cannot determine CodeCairn version")
endif()
if(NOT DEFINED OUTPUT)
    set(OUTPUT "${SOURCE}/build/releases")
endif()
get_filename_component(OUTPUT "${OUTPUT}" ABSOLUTE)
if(EXISTS "${SOURCE}/SOURCE-MANIFEST.txt")
    file(STRINGS "${SOURCE}/SOURCE-MANIFEST.txt" revision_line REGEX "^revision=[a-f0-9]+$")
    file(STRINGS "${SOURCE}/SOURCE-MANIFEST.txt" dirty_line REGEX "^dirty=(true|false)$")
    if(NOT revision_line OR NOT dirty_line)
        message(FATAL_ERROR "Invalid source archive provenance in SOURCE-MANIFEST.txt")
    endif()
    string(
        REPLACE "revision="
                ""
                REVISION
                "${revision_line}"
    )
    string(
        REPLACE "dirty="
                ""
                DIRTY
                "${dirty_line}"
    )
    message(STATUS "Using source archive provenance: ${REVISION}, dirty=${DIRTY}")
else()
    find_program(GIT git REQUIRED)
    execute_process(
        COMMAND "${GIT}" -C "${SOURCE}" rev-parse HEAD
        OUTPUT_VARIABLE REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND "${GIT}" -C "${SOURCE}" status --porcelain -- .
        OUTPUT_VARIABLE changes COMMAND_ERROR_IS_FATAL ANY
    )
    set(DIRTY false)
    if(NOT
       changes
       STREQUAL
       ""
    )
        set(DIRTY true)
    endif()
endif()

function(checksum artifact)
    file(SHA256 "${artifact}" hash)
    get_filename_component(name "${artifact}" NAME)
    file(WRITE "${artifact}.sha256" "${hash}  ${name}\n")
endfunction()

function(copy_immutable source destination)
    if(EXISTS "${destination}")
        file(SHA256 "${source}" expected)
        file(SHA256 "${destination}" actual)
        if(NOT
           actual
           STREQUAL
           expected
        )
            message(FATAL_ERROR "Existing release artifact differs: ${destination}")
        endif()
    else()
        configure_file("${source}" "${destination}" COPYONLY)
    endif()
endfunction()
