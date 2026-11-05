cmake_minimum_required(VERSION 3.24)
string(RANDOM LENGTH 8 run_id)
set(TEST_DIR "${TEST_DIR}/run-${run_id}")
set(OUTPUT "${TEST_DIR}/unused")
include("${SOURCE_DIR}/packaging/ReleaseCommon.cmake")
set(artifacts "${TEST_DIR}/artifacts")
set(source "${artifacts}/codecairn-source-recipes")
file(MAKE_DIRECTORY "${source}")
set(archive "${source}/codecairn-${VERSION}-source.tar.gz")
file(MAKE_DIRECTORY "${TEST_DIR}/source/codecairn-${VERSION}")
file(WRITE "${TEST_DIR}/source/codecairn-${VERSION}/SOURCE-MANIFEST.txt"
    "CodeCairn ${VERSION}\nrevision=${REVISION}\ndirty=false\n")
run("${CMAKE_COMMAND}" -E chdir "${TEST_DIR}/source" "${CMAKE_COMMAND}" -E tar czf
    "${archive}" --format=gnutar "codecairn-${VERSION}")
checksum("${archive}")
file(SHA256 "${archive}" source_hash)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DARCHIVE=${archive}" "-DOUTPUT=${source}"
                -P "${SOURCE_DIR}/packaging/PrepareRecipes.cmake"
                COMMAND_ERROR_IS_FATAL ANY)
set(assets
    "codecairn-windows/codecairn-${VERSION}-windows11-x64.zip"
    "codecairn-macos/codecairn-${VERSION}-macos26-arm64-unsigned.dmg"
    "codecairn-msys2/mingw-w64-ucrt-x86_64-codecairn-${VERSION}-1-any.pkg.tar.zst"
    "codecairn-arch/codecairn-${VERSION}-1-x86_64.pkg.tar.zst")
foreach(asset IN LISTS assets)
    get_filename_component(directory "${artifacts}/${asset}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${artifacts}/${asset}" "package fixture\n")
    checksum("${artifacts}/${asset}")
endforeach()
file(WRITE "${artifacts}/codecairn-arch/.SRCINFO"
    "pkgbase = codecairn\n\tpkgver = ${VERSION}\n"
    "\tsource = https://github.com/deepak-rawat/codecairn/releases/download/codecairn-v${VERSION}/codecairn-${VERSION}-source.tar.gz\n"
    "\tsha256sums = ${source_hash}\n\npkgname = codecairn\n")
set(command "${CMAKE_COMMAND}" "-DARTIFACTS=${artifacts}"
    "-DRELEASE_TAG=codecairn-v${VERSION}")

function(expect_failure name expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${TEST_DIR}/${name}" ${ARGN}
        "-DARTIFACTS=${artifacts}" "-DRELEASE_TAG=codecairn-v${VERSION}"
        -P "${SOURCE_DIR}/packaging/PrepareReleaseAssets.cmake"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT error MATCHES "${expected}")
        message(FATAL_ERROR "${name}: unexpected result: ${output}\n${error}")
    endif()
    if(NOT name STREQUAL valid AND EXISTS "${TEST_DIR}/${name}/SHA256SUMS")
        message(FATAL_ERROR "${name}: failed validation produced a release manifest")
    endif()
endfunction()

execute_process(COMMAND ${command} "-DOUTPUT=${TEST_DIR}/valid"
                -P "${SOURCE_DIR}/packaging/PrepareReleaseAssets.cmake"
                COMMAND_ERROR_IS_FATAL ANY)
file(GLOB files LIST_DIRECTORIES FALSE "${TEST_DIR}/valid/*")
list(LENGTH files count)
if(NOT count EQUAL 13)
    message(FATAL_ERROR "Expected six assets, six sidecars, and SHA256SUMS; found ${count}")
endif()
file(STRINGS "${TEST_DIR}/valid/SHA256SUMS" lines)
list(LENGTH lines count)
if(NOT count EQUAL 12)
    message(FATAL_ERROR "Release manifest has the wrong number of entries")
endif()
foreach(line IN LISTS lines)
    string(SUBSTRING "${line}" 0 64 expected_hash)
    string(SUBSTRING "${line}" 66 -1 name)
    file(SHA256 "${TEST_DIR}/valid/${name}" actual)
    if(NOT actual STREQUAL expected_hash)
        message(FATAL_ERROR "Release manifest checksum differs: ${name}")
    endif()
endforeach()
file(ARCHIVE_EXTRACT INPUT "${TEST_DIR}/valid/codecairn-${VERSION}-recipes.tar.gz"
     DESTINATION "${TEST_DIR}/extracted")
foreach(recipe homebrew/Formula/codecairn.rb arch/PKGBUILD msys2/PKGBUILD arch/.SRCINFO)
    if(NOT EXISTS "${TEST_DIR}/extracted/${recipe}")
        message(FATAL_ERROR "Recipe archive is incomplete: ${recipe}")
    endif()
endforeach()
expect_failure(valid "output must be empty")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DARTIFACTS=${artifacts}"
    "-DOUTPUT=${TEST_DIR}/wrong-tag" -DRELEASE_TAG=codecairn-v99.0.0
    -P "${SOURCE_DIR}/packaging/PrepareReleaseAssets.cmake" RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "Wrong release tag was accepted")
endif()
set(windows "${artifacts}/codecairn-windows/codecairn-${VERSION}-windows11-x64.zip")
file(APPEND "${windows}" "tampered\n")
expect_failure(tampered "checksum differs")
file(WRITE "${windows}" "")
expect_failure(empty "empty or its checksum is missing")
file(WRITE "${windows}" "package fixture\n")
checksum("${windows}")
file(REMOVE "${windows}.sha256")
expect_failure(missing-checksum "empty or its checksum is missing")
checksum("${windows}")
file(REMOVE "${windows}")
expect_failure(missing "Expected one")
file(WRITE "${windows}" "package fixture\n")
file(WRITE "${artifacts}/codecairn-windows/unexpected.zip" "unexpected\n")
expect_failure(unexpected "Unexpected release artifact")
file(REMOVE "${artifacts}/codecairn-windows/unexpected.zip")
file(WRITE "${artifacts}/codecairn-arch/codecairn-${VERSION}-2-x86_64.pkg.tar.zst" "duplicate\n")
expect_failure(duplicate "Expected one")
file(REMOVE "${artifacts}/codecairn-arch/codecairn-${VERSION}-2-x86_64.pkg.tar.zst")
file(WRITE "${source}/homebrew/Formula/codecairn.rb" "wrong source recipe\n")
expect_failure(wrong-recipe "Public recipe does not match")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DARCHIVE=${archive}" "-DOUTPUT=${source}"
                -P "${SOURCE_DIR}/packaging/PrepareRecipes.cmake"
                COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${artifacts}/codecairn-arch/.SRCINFO" "wrong source metadata\n")
expect_failure(wrong-metadata "Arch metadata does not match")
file(WRITE "${TEST_DIR}/source/codecairn-${VERSION}/SOURCE-MANIFEST.txt"
    "CodeCairn ${VERSION}\nrevision=${REVISION}\ndirty=true\n")
run("${CMAKE_COMMAND}" -E chdir "${TEST_DIR}/source" "${CMAKE_COMMAND}" -E tar czf
    "${archive}" --format=gnutar "codecairn-${VERSION}")
checksum("${archive}")
file(SHA256 "${archive}" source_hash)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DARCHIVE=${archive}" "-DOUTPUT=${source}"
                -P "${SOURCE_DIR}/packaging/PrepareRecipes.cmake"
                COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${artifacts}/codecairn-arch/.SRCINFO"
    "pkgbase = codecairn\n\tpkgver = ${VERSION}\n"
    "\tsource = https://github.com/deepak-rawat/codecairn/releases/download/codecairn-v${VERSION}/codecairn-${VERSION}-source.tar.gz\n"
    "\tsha256sums = ${source_hash}\n\npkgname = codecairn\n")
expect_failure(dirty-source "Source archive provenance does not match")
message(STATUS "Complete release assembly and invalid-asset rejection verified")
