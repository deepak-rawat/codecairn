include("${CMAKE_CURRENT_LIST_DIR}/ReleaseCommon.cmake")
if(DEFINED BOTTLE_JSON)
    message(FATAL_ERROR "BOTTLE_JSON is no longer supported; CodeCairn uses a source-only Homebrew formula.")
endif()
if(NOT DEFINED ARCHIVE)
    set(ARCHIVE "${OUTPUT}/codecairn-${VERSION}-source.tar.gz")
endif()
if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Create the source distribution first: ${ARCHIVE}")
endif()
file(SHA256 "${ARCHIVE}" SHA256)
if(NOT DEFINED RELEASE_URL)
    set(RELEASE_URL "https://github.com/deepak-rawat/codecairn/releases/download/codecairn-v${VERSION}")
endif()
set(SOURCE_URL "${RELEASE_URL}/codecairn-${VERSION}-source.tar.gz")
foreach(platform msys2 arch)
    file(MAKE_DIRECTORY "${OUTPUT}/${platform}")
    configure_file("${SOURCE}/packaging/${platform}/PKGBUILD.in"
                   "${OUTPUT}/${platform}/PKGBUILD" @ONLY NEWLINE_STYLE UNIX)
    configure_file("${ARCHIVE}" "${OUTPUT}/${platform}/codecairn-${VERSION}-source.tar.gz" COPYONLY)
endforeach()
configure_file("${SOURCE}/packaging/homebrew/codecairn.rb.in" "${OUTPUT}/codecairn.rb"
               @ONLY NEWLINE_STYLE UNIX)
file(MAKE_DIRECTORY "${OUTPUT}/homebrew/Formula")
configure_file("${OUTPUT}/codecairn.rb" "${OUTPUT}/homebrew/Formula/codecairn.rb" COPYONLY)
checksum("${OUTPUT}/homebrew/Formula/codecairn.rb")
file(WRITE "${OUTPUT}/arch/.SRCINFO"
    "pkgbase = codecairn\n\tpkgdesc = Desktop source browser and editor\n"
    "\tpkgver = ${VERSION}\n\tpkgrel = 1\n\turl = https://github.com/deepak-rawat/codecairn\n"
    "\tarch = x86_64\n\tlicense = MIT\n\tmakedepends = cmake\n\tmakedepends = ninja\n"
    "\tdepends = qt6-base>=6.5\n\tdepends = qt6-5compat\n\tdepends = tree-sitter>=0.25\n"
    "\tdepends = sqlite\n\tdepends = re2\n\tdepends = clang\n\tdepends = llvm-libs\n"
    "\tsource = ${SOURCE_URL}\n\tsha256sums = ${SHA256}\n\npkgname = codecairn\n")
checksum("${OUTPUT}/codecairn.rb")
run("${CMAKE_COMMAND}" -E chdir "${OUTPUT}" "${CMAKE_COMMAND}" -E tar cf
    "${OUTPUT}/codecairn-${VERSION}-recipes.zip" --format=zip
    msys2/PKGBUILD arch/PKGBUILD arch/.SRCINFO)
checksum("${OUTPUT}/codecairn-${VERSION}-recipes.zip")
message(STATUS "Recipes generated in ${OUTPUT}; source SHA256=${SHA256}")
