include("${CMAKE_CURRENT_LIST_DIR}/ReleaseCommon.cmake")
if(EXISTS "${SOURCE}/SOURCE-MANIFEST.txt")
    message(
        FATAL_ERROR "PrepareSource must run from a Git checkout, not an unpacked source archive."
    )
endif()
string(
    RANDOM
    LENGTH 12
    random
)
set(work "${OUTPUT}/source-work-${random}")
set(root "${work}/codecairn-${VERSION}")
file(MAKE_DIRECTORY "${root}")
execute_process(
    COMMAND "${GIT}" -C "${SOURCE}" ls-files --cached --others --exclude-standard -- .
    OUTPUT_VARIABLE files
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
if(files MATCHES ";")
    message(FATAL_ERROR "Source filenames containing semicolons are not supported.")
endif()
string(
    REPLACE "\n"
            ";"
            files
            "${files}"
)
if(DIRTY)
    execute_process(
        COMMAND "${GIT}" -C "${SOURCE}" ls-files --deleted -- .
        OUTPUT_VARIABLE deleted
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
    )
    if(deleted)
        string(
            REPLACE "\n"
                    ";"
                    deleted
                    "${deleted}"
        )
        list(REMOVE_ITEM files ${deleted})
    endif()
endif()
foreach(file IN LISTS files)
    if(file MATCHES "^(build|out|\\.git|\\.vscode)(/|$)" OR file MATCHES "(^|/)\\.DS_Store$")
        continue()
    endif()
    if(NOT EXISTS "${SOURCE}/${file}")
        message(FATAL_ERROR "Tracked source is missing: ${file}")
    endif()
    get_filename_component(directory "${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${root}/${directory}")
    file(COPY "${SOURCE}/${file}" DESTINATION "${root}/${directory}")
endforeach()

function(
    codecairn_source
    name
    override
    url
    hash
)
    set(archive "${work}/${name}.tar.gz")
    file(
        DOWNLOAD "${url}" "${archive}"
        EXPECTED_HASH "SHA256=${hash}"
        TLS_VERIFY ON
    )
    file(
        ARCHIVE_EXTRACT
        INPUT
        "${archive}"
        DESTINATION
        "${work}/unpacked-${name}"
    )
    file(
        GLOB children
        LIST_DIRECTORIES TRUE
        "${work}/unpacked-${name}/*"
    )
    list(LENGTH children count)
    if(NOT
       count
       EQUAL
       1
    )
        message(FATAL_ERROR "Expected one source root in ${name}")
    endif()
    list(
        GET
        children
        0
        unpacked
    )
    foreach(required IN LISTS ARGN)
        if(NOT EXISTS "${unpacked}/${required}")
            message(FATAL_ERROR "${name}: archive missing ${required}")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${root}/vendor/${name}")
    file(COPY "${unpacked}/" DESTINATION "${root}/vendor/${name}")
    file(APPEND "${root}/SOURCE-MANIFEST.txt" "${name} ${hash} ${url}\n")
endfunction()

file(WRITE "${root}/SOURCE-MANIFEST.txt"
     "CodeCairn ${VERSION}\nrevision=${REVISION}\ndirty=${DIRTY}\n"
)
include("${SOURCE}/cmake/SourceArchives.cmake")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/source/CMakeLists.txt.in" "${work}/project/CMakeLists.txt" @ONLY
)
run("${CMAKE_COMMAND}"
    -S
    "${work}/project"
    -B
    "${work}/config"
)
find_program(CPACK cpack REQUIRED)
run("${CPACK}"
    --config
    "${work}/config/CPackSourceConfig.cmake"
    -B
    "${work}/packages"
)
set(archive "${OUTPUT}/codecairn-${VERSION}-source.tar.gz")
copy_immutable("${work}/packages/codecairn-${VERSION}-source.tar.gz" "${archive}")
checksum("${archive}")
file(REMOVE_RECURSE "${work}")
message(STATUS "Source distribution: ${archive}")
