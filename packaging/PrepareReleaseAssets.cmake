include("${CMAKE_CURRENT_LIST_DIR}/ReleaseCommon.cmake")
if(NOT DEFINED ARTIFACTS OR NOT IS_DIRECTORY "${ARTIFACTS}")
    message(FATAL_ERROR "Set ARTIFACTS to the downloaded workflow artifact directory.")
endif()
if(NOT
   RELEASE_TAG
   STREQUAL
   "codecairn-v${VERSION}"
)
    message(FATAL_ERROR "Release tag must match CodeCairn version ${VERSION}.")
endif()
set(expected_artifacts
    codecairn-source-recipes
    codecairn-windows
    codecairn-macos
    codecairn-msys2
    codecairn-arch
)
file(GLOB entries "${ARTIFACTS}/*")
foreach(entry IN LISTS entries)
    get_filename_component(name "${entry}" NAME)
    if(NOT
       name
       IN_LIST
       expected_artifacts
       OR NOT IS_DIRECTORY "${entry}"
    )
        message(FATAL_ERROR "Unexpected workflow artifact: ${entry}")
    endif()
endforeach()
if(EXISTS "${OUTPUT}")
    file(
        GLOB
        existing
        "${OUTPUT}/*"
        "${OUTPUT}/.[!.]*"
    )
    if(existing)
        message(FATAL_ERROR "Release asset output must be empty: ${OUTPUT}")
    endif()
endif()

set(source "${ARTIFACTS}/codecairn-source-recipes")
set(archive "${source}/codecairn-${VERSION}-source.tar.gz")
set(assets "${archive}")
foreach(
    spec
    "codecairn-windows|codecairn-${VERSION}-windows11-x64.zip"
    "codecairn-macos|codecairn-${VERSION}-macos26-arm64-unsigned.dmg"
    "codecairn-msys2|mingw-w64-ucrt-x86_64-codecairn-${VERSION}-*.pkg.tar.zst"
    "codecairn-arch|codecairn-${VERSION}-*-x86_64.pkg.tar.zst"
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
        artifact
    )
    list(
        GET
        parts
        1
        pattern
    )
    file(
        GLOB matches
        LIST_DIRECTORIES FALSE
        "${ARTIFACTS}/${artifact}/${pattern}"
    )
    list(LENGTH matches count)
    if(NOT
       count
       EQUAL
       1
    )
        message(FATAL_ERROR "Expected one ${artifact}/${pattern} asset; found ${count}.")
    endif()
    list(APPEND assets ${matches})
    file(
        GLOB files
        LIST_DIRECTORIES FALSE
        "${ARTIFACTS}/${artifact}/*"
    )
    foreach(file IN LISTS files)
        if(artifact STREQUAL codecairn-arch AND file STREQUAL
                                                "${ARTIFACTS}/codecairn-arch/.SRCINFO"
        )
            continue()
        endif()
        if(NOT
           file
           STREQUAL
           matches
           AND NOT
               file
               STREQUAL
               "${matches}.sha256"
        )
            message(FATAL_ERROR "Unexpected release artifact: ${file}")
        endif()
    endforeach()
endforeach()

foreach(asset IN LISTS assets)
    if(NOT EXISTS "${asset}")
        message(FATAL_ERROR "Release asset is missing: ${asset}")
    endif()
    file(SIZE "${asset}" size)
    if(size EQUAL 0 OR NOT EXISTS "${asset}.sha256")
        message(FATAL_ERROR "Release asset is empty or its checksum is missing: ${asset}")
    endif()
    file(SHA256 "${asset}" hash)
    file(READ "${asset}.sha256" sidecar)
    string(
        REPLACE "\r\n"
                "\n"
                sidecar
                "${sidecar}"
    )
    get_filename_component(name "${asset}" NAME)
    if(NOT
       sidecar
       STREQUAL
       "${hash}  ${name}\n"
    )
        message(FATAL_ERROR "Release asset checksum differs: ${asset}")
    endif()
endforeach()
file(SHA256 "${archive}" source_hash)
set(recipes homebrew/Formula/codecairn.rb arch/PKGBUILD msys2/PKGBUILD)
foreach(recipe IN LISTS recipes)
    file(READ "${source}/${recipe}" content)
    foreach(
        expected
        "${source_hash}"
        "https://github.com/deepak-rawat/codecairn/releases/download/${RELEASE_TAG}/codecairn-${VERSION}-source.tar.gz"
    )
        string(FIND "${content}" "${expected}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Public recipe does not match the release source: ${recipe}")
        endif()
    endforeach()
    if(recipe STREQUAL "homebrew/Formula/codecairn.rb")
        set(expected "version \"${VERSION}\"")
    else()
        set(expected "pkgver=${VERSION}\n")
    endif()
    string(FIND "${content}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Public recipe version does not match: ${recipe}")
    endif()
endforeach()
set(srcinfo "${ARTIFACTS}/codecairn-arch/.SRCINFO")
file(READ "${srcinfo}" content)
foreach(
    expected
    "pkgbase = codecairn\n"
    "pkgname = codecairn\n"
    "pkgver = ${VERSION}\n"
    "sha256sums = ${source_hash}\n"
    "https://github.com/deepak-rawat/codecairn/releases/download/${RELEASE_TAG}/codecairn-${VERSION}-source.tar.gz"
)
    string(FIND "${content}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Arch metadata does not match the release source: ${srcinfo}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")
string(
    RANDOM
    LENGTH 12
    random
)
set(work "${OUTPUT}/recipes-work-${random}")
file(
    ARCHIVE_EXTRACT
    INPUT
    "${archive}"
    DESTINATION
    "${work}/source"
    PATTERNS
    "codecairn-${VERSION}/SOURCE-MANIFEST.txt"
)
file(READ "${work}/source/codecairn-${VERSION}/SOURCE-MANIFEST.txt" provenance)
foreach(expected "CodeCairn ${VERSION}\n" "revision=${REVISION}\n" "dirty=false\n")
    string(FIND "${provenance}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Source archive provenance does not match the release revision.")
    endif()
endforeach()
foreach(asset IN LISTS assets)
    get_filename_component(name "${asset}" NAME)
    copy_immutable("${asset}" "${OUTPUT}/${name}")
    checksum("${OUTPUT}/${name}")
endforeach()
foreach(recipe IN LISTS recipes)
    configure_file("${source}/${recipe}" "${work}/${recipe}" COPYONLY)
endforeach()
configure_file("${srcinfo}" "${work}/arch/.SRCINFO" COPYONLY)
file(REMOVE_RECURSE "${work}/source")
set(recipe_archive "${OUTPUT}/codecairn-${VERSION}-recipes.tar.gz")
run("${CMAKE_COMMAND}"
    -E
    chdir
    "${work}"
    "${CMAKE_COMMAND}"
    -E
    tar
    czf
    "${recipe_archive}"
    --format=gnutar
    homebrew
    arch
    msys2
)
checksum("${recipe_archive}")
file(REMOVE_RECURSE "${work}")
file(
    GLOB files
    LIST_DIRECTORIES FALSE
    "${OUTPUT}/*"
)
set(manifest "")
foreach(file IN LISTS files)
    file(SHA256 "${file}" hash)
    get_filename_component(name "${file}" NAME)
    string(APPEND manifest "${hash}  ${name}\n")
endforeach()
file(WRITE "${OUTPUT}/SHA256SUMS" "${manifest}")
message(STATUS "Verified complete release assets in ${OUTPUT}")
