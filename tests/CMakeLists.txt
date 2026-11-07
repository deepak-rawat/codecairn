find_package(Qt6 REQUIRED COMPONENTS Test)

add_test(NAME application_version COMMAND codecairn --version)
set_tests_properties(
    application_version
    PROPERTIES ENVIRONMENT
               "QT_QPA_PLATFORM=offscreen"
               PASS_REGULAR_EXPRESSION
               "CodeCairn ${PROJECT_VERSION}"
               TIMEOUT
               20
)
add_test(NAME launcher_version COMMAND cairn --version)
set_tests_properties(
    launcher_version PROPERTIES PASS_REGULAR_EXPRESSION "CodeCairn ${PROJECT_VERSION}"
)
add_executable(launcher_fixture LauncherFixture.cpp)
target_link_libraries(launcher_fixture PRIVATE Qt6::Core)
set_target_properties(launcher_fixture PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE)
qt_add_executable(launcher_tests LauncherTests.cpp)
target_link_libraries(launcher_tests PRIVATE Qt6::Test)
set_target_properties(launcher_tests PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE)
add_dependencies(launcher_tests cairn launcher_fixture)
add_test(NAME launcher COMMAND launcher_tests "$<TARGET_FILE:cairn>"
                               "$<TARGET_FILE:launcher_fixture>"
)
set_tests_properties(launcher PROPERTIES TIMEOUT 40)
add_test(NAME application_help COMMAND codecairn --help)
set_tests_properties(
    application_help
    PROPERTIES ENVIRONMENT
               "QT_QPA_PLATFORM=offscreen"
               PASS_REGULAR_EXPRESSION
               "CodeCairn - Find your way through code\\."
               TIMEOUT
               20
)

add_test(
    NAME deployment_paths
    COMMAND
        "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/deployment-paths" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/DeploymentPathsTests.cmake"
)
add_test(
    NAME packaging
    COMMAND
        "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/packaging" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/PackagingTests.cmake"
)
foreach(spec "CPack|cpack" "SourcePackage|source_package" "ReleaseAssets|release_assets")
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
        suite
    )
    list(
        GET
        parts
        1
        name
    )
    add_test(
        NAME "${name}"
        COMMAND
            "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/${name}" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/${suite}Tests.cmake"
    )
endforeach()
if(NOT CODECAIRN_BUNDLE_RUNTIME)
    add_test(
        NAME managed_runtime
        COMMAND
            "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:codecairn>"
            "-DRUNTIME_ROOT=${PROJECT_BINARY_DIR}" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/VerifyManagedRuntime.cmake"
    )
endif()

function(
    codecairn_test
    target
    source
    name
)
    qt_add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE codecairn_core Qt6::Test)
    set_target_properties(
        ${target}
        PROPERTIES WIN32_EXECUTABLE FALSE
                   MACOSX_BUNDLE FALSE
                   RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    )
    if(WIN32)
        set_target_properties(
            ${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
        )
    endif()
    if(CODECAIRN_CLANGD)
        target_compile_definitions(${target} PRIVATE CODECAIRN_TEST_CLANGD="${CODECAIRN_CLANGD}")
    endif()
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(
        ${name}
        PROPERTIES WORKING_DIRECTORY
                   "${PROJECT_BINARY_DIR}"
                   ENVIRONMENT
                   "QT_QPA_PLATFORM=offscreen"
                   TIMEOUT
                   180
    )
endfunction()

codecairn_test(
    codecairn_tests
    PrototypeTests.cpp
    prototype
    -o
    test-results.xml,junitxml
    -o
    -,txt
)
codecairn_test(
    workspace_tests
    WorkspaceTests.cpp
    workspace
    -o
    workspace-results.xml,junitxml
    -o
    workspace-results.txt,txt
)
codecairn_test(
    configuration_tests
    ConfigurationTests.cpp
    configuration
    -o
    configuration-results.xml,junitxml
    -o
    configuration-results.txt,txt
)
codecairn_test(
    language_tests
    LanguageTests.cpp
    languages
    -o
    language-results.xml,junitxml
    -o
    language-results.txt,txt
)
target_compile_definitions(
    language_tests PRIVATE CODECAIRN_EXAMPLES_PATH="${PROJECT_SOURCE_DIR}/examples/languages"
)
codecairn_test(
    interaction_tests
    InteractionTests.cpp
    interactions
    -o
    interaction-results.xml,junitxml
    -o
    interaction-results.txt,txt
)
codecairn_test(
    startup_tests
    StartupTests.cpp
    startup
    -o
    startup-results.xml,junitxml
    -o
    startup-results.txt,txt
)
codecairn_test(refactoring_tests RefactoringTests.cpp refactoring)
codecairn_test(application_state_tests ApplicationStateTests.cpp application_state)
codecairn_test(
    diagnostics_tests
    DiagnosticsTests.cpp
    diagnostics
    -o
    diagnostics-results.xml,junitxml
    -o
    -,txt
)
codecairn_test(
    reference_tests
    ReferenceTests.cpp
    references
    -o
    reference-results.xml,junitxml
    -o
    reference-results.txt,txt
)
codecairn_test(index_parity_tests IndexParityTests.cpp index_parity)
codecairn_test(
    reference_candidate_index_tests ReferenceCandidateIndexTests.cpp reference_candidate_index
)
codecairn_test(analysis_parity_tests AnalysisParityTests.cpp analysis_parity)
option(CODECAIRN_BUILD_BENCHMARKS "Build and register benchmark fixtures" ON)
if(CODECAIRN_BUILD_BENCHMARKS)
    codecairn_test(
        codecairn_benchmarks
        RefactoringBenchmarks.cpp
        benchmarks
        -iterations
        1
        -o
        benchmark-results.txt,txt
    )
    set_tests_properties(benchmarks PROPERTIES LABELS benchmark)
endif()
foreach(target codecairn_tests configuration_tests interaction_tests)
    target_sources(${target} PRIVATE fixtures.qrc)
endforeach()
