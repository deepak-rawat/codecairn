include("${CMAKE_CURRENT_LIST_DIR}/ReleaseCommon.cmake")
if(NOT DEFINED ARTIFACTS OR NOT IS_DIRECTORY "${ARTIFACTS}")
    message(FATAL_ERROR "Set ARTIFACTS to the downloaded workflow artifact directory.")
endif()
if(EXISTS "${OUTPUT}")
    file(GLOB existing "${OUTPUT}/*" "${OUTPUT}/.[!.]*")
    if(existing)
        message(FATAL_ERROR "Release asset output must be empty: ${OUTPUT}")
    endif()
endif()

set(source "${ARTIFACTS}/codecairn-source-recipes")
set(assets
    "${source}/codecairn-${VERSION}-source.tar.gz"
    "${ARTIFACTS}/codecairn-windows/codecairn-${VERSION}-windows11-x64.zip"
    "${ARTIFACTS}/codecairn-macos/codecairn-${VERSION}-macos26-arm64-unsigned.dmg"
    "${ARTIFACTS}/codecairn-linux/codecairn-${VERSION}-linux-ubuntu24.04-x64.tar.gz"
)
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(asset IN LISTS assets)
    get_filename_component(name "${asset}" NAME)
    configure_file("${asset}" "${OUTPUT}/${name}" COPYONLY)
    checksum("${OUTPUT}/${name}")
endforeach()

string(RANDOM LENGTH 12 random)
set(work "${OUTPUT}/recipes-work-${random}")
foreach(recipe homebrew/Formula/codecairn.rb arch/PKGBUILD msys2/PKGBUILD)
    configure_file("${source}/${recipe}" "${work}/${recipe}" COPYONLY)
endforeach()
set(recipe_archive "${OUTPUT}/codecairn-${VERSION}-recipes.tar.gz")
run("${CMAKE_COMMAND}" -E chdir "${work}" "${CMAKE_COMMAND}" -E tar
    czf "${recipe_archive}" --format=gnutar homebrew arch msys2)
checksum("${recipe_archive}")
file(REMOVE_RECURSE "${work}")
file(GLOB files LIST_DIRECTORIES FALSE "${OUTPUT}/*")
set(manifest "")
foreach(file IN LISTS files)
    file(SHA256 "${file}" hash)
    get_filename_component(name "${file}" NAME)
    string(APPEND manifest "${hash}  ${name}\n")
endforeach()
file(WRITE "${OUTPUT}/SHA256SUMS" "${manifest}")
message(STATUS "Release assets assembled in ${OUTPUT}; verify packages manually before publication.")
