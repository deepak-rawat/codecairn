include("${CMAKE_CURRENT_LIST_DIR}/ReleaseCommon.cmake")
if(NOT DEFINED ARCHIVE)
    set(ARCHIVE "${OUTPUT}/codecairn-${VERSION}-source.tar.gz")
endif()
if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Create the source distribution first: ${ARCHIVE}")
endif()
file(SHA256 "${ARCHIVE}" SHA256)
if(NOT DEFINED RELEASE_URL)
    set(RELEASE_URL
        "https://github.com/deepak-rawat/codecairn/releases/download/codecairn-v${VERSION}"
    )
endif()
set(SOURCE_URL "${RELEASE_URL}/codecairn-${VERSION}-source.tar.gz")
foreach(platform msys2 arch)
    file(MAKE_DIRECTORY "${OUTPUT}/${platform}")
    configure_file(
        "${SOURCE}/packaging/${platform}/PKGBUILD.in" "${OUTPUT}/${platform}/PKGBUILD" @ONLY
        NEWLINE_STYLE UNIX
    )
    configure_file("${ARCHIVE}" "${OUTPUT}/${platform}/codecairn-${VERSION}-source.tar.gz" COPYONLY)
endforeach()
file(MAKE_DIRECTORY "${OUTPUT}/homebrew/Formula")
configure_file(
    "${SOURCE}/packaging/homebrew/codecairn.rb.in" "${OUTPUT}/homebrew/Formula/codecairn.rb" @ONLY
    NEWLINE_STYLE UNIX
)
message(STATUS "Recipes generated in ${OUTPUT}; source SHA256=${SHA256}")
