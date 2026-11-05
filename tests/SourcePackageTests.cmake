cmake_minimum_required(VERSION 3.24)
string(RANDOM LENGTH 8 run_id)
set(TEST_DIR "${TEST_DIR}/run-${run_id}")
set(OUTPUT "${TEST_DIR}/unused")
include("${SOURCE_DIR}/packaging/ReleaseCommon.cmake")
set(fixture "${TEST_DIR}/checkout")
file(MAKE_DIRECTORY "${fixture}/packaging/source" "${fixture}/cmake"
                    "${TEST_DIR}/dependency/upstream")
foreach(script PrepareSource ReleaseCommon)
    configure_file("${SOURCE_DIR}/packaging/${script}.cmake"
                   "${fixture}/packaging/${script}.cmake" COPYONLY)
endforeach()
configure_file("${SOURCE_DIR}/packaging/source/CMakeLists.txt.in"
               "${fixture}/packaging/source/CMakeLists.txt.in" COPYONLY)
file(WRITE "${TEST_DIR}/dependency/upstream/parser.c" "int parser_fixture;\n")
file(WRITE "${TEST_DIR}/dependency/upstream/LICENSE" "Fixture license\n")
run("${CMAKE_COMMAND}" -E chdir "${TEST_DIR}/dependency" "${CMAKE_COMMAND}" -E tar czf
    "${TEST_DIR}/dependency.tar.gz" upstream)
file(SHA256 "${TEST_DIR}/dependency.tar.gz" hash)
file(WRITE "${fixture}/cmake/SourceArchives.cmake"
    "codecairn_source(fixture FIXTURE_SOURCE_DIR \"file://${TEST_DIR}/dependency.tar.gz\" ${hash} parser.c LICENSE)\n")
file(WRITE "${fixture}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\nproject(\n    CodeCairn\n    VERSION ${VERSION}\n    LANGUAGES NONE\n)\n"
    "if(NOT EXISTS \"\${CMAKE_CURRENT_SOURCE_DIR}/vendor/fixture/parser.c\")\n"
    "    message(FATAL_ERROR \"Missing offline source dependency\")\nendif()\n"
    "if(CODECAIRN_ALLOW_DOWNLOADS)\n    message(FATAL_ERROR \"Downloads must be disabled\")\nendif()\n")
file(WRITE "${fixture}/removed.txt" "local snapshot deletion fixture\n")
find_program(GIT git REQUIRED)
run("${GIT}" init "${fixture}")
run("${GIT}" -C "${fixture}" add .)
run("${GIT}" -C "${fixture}" -c user.name=Packaging-Test -c user.email=test@example.invalid
    commit -m "Source package fixture")
set(command "${CMAKE_COMMAND}" "-DOUTPUT=${TEST_DIR}/packages"
    -P "${fixture}/packaging/PrepareSource.cmake")
run(${command})
set(archive "${TEST_DIR}/packages/codecairn-${VERSION}-source.tar.gz")
file(SHA256 "${archive}" actual)
file(READ "${archive}.sha256" sidecar)
if(NOT sidecar STREQUAL "${actual}  codecairn-${VERSION}-source.tar.gz\n")
    message(FATAL_ERROR "Source package checksum differs")
endif()
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${TEST_DIR}/extracted")
set(root "${TEST_DIR}/extracted/codecairn-${VERSION}")
file(READ "${root}/SOURCE-MANIFEST.txt" manifest)
if(NOT manifest MATCHES "dirty=false" OR NOT manifest MATCHES "${hash}")
    message(FATAL_ERROR "Source/dependency provenance is missing")
endif()
file(READ "${root}/vendor/fixture/LICENSE" notice)
if(NOT notice STREQUAL "Fixture license\n")
    message(FATAL_ERROR "Pinned dependency license is missing")
endif()
run("${CMAKE_COMMAND}" -S "${root}" -B "${TEST_DIR}/offline-build" -G Ninja
    -DCODECAIRN_ALLOW_DOWNLOADS=OFF)
run("${CMAKE_COMMAND}" --build "${TEST_DIR}/offline-build" --parallel)
file(APPEND "${fixture}/CMakeLists.txt" "\n# local change\n")
file(REMOVE "${fixture}/removed.txt")
execute_process(COMMAND ${command} RESULT_VARIABLE result
                OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR NOT error MATCHES "Commit release inputs first")
    message(FATAL_ERROR "Dirty release sources were accepted: ${output}\n${error}")
endif()
run("${CMAKE_COMMAND}" -DALLOW_DIRTY=ON "-DOUTPUT=${TEST_DIR}/local-packages"
    -P "${fixture}/packaging/PrepareSource.cmake")
file(ARCHIVE_EXTRACT INPUT "${TEST_DIR}/local-packages/codecairn-${VERSION}-source.tar.gz"
     DESTINATION "${TEST_DIR}/local-extracted")
set(local_root "${TEST_DIR}/local-extracted/codecairn-${VERSION}")
file(READ "${local_root}/SOURCE-MANIFEST.txt" local_manifest)
if(NOT local_manifest MATCHES "dirty=true" OR EXISTS "${local_root}/removed.txt")
    message(FATAL_ERROR "Local snapshot provenance or tracked deletion handling is incorrect")
endif()
message(STATUS "CPack source layout, checksums, pinned licenses, and offline configure/build verified")
