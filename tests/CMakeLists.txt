find_package(Qt6 REQUIRED COMPONENTS Test)

function(
    codecairn_core_test
    target
    source
    name
)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE codecairn_core Qt6::Test)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(
        ${target}
        PROPERTIES MACOSX_BUNDLE FALSE
                   WIN32_EXECUTABLE FALSE
                   RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
    )
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

codecairn_core_test(core_tests core/CoreTests.cpp core)
codecairn_core_test(document_analysis_tests core/DocumentAnalysisTests.cpp document_analysis)
codecairn_core_test(analysis_parity_tests core/AnalysisParityTests.cpp analysis_parity)
codecairn_core_test(index_parity_tests core/IndexParityTests.cpp index_parity)
codecairn_core_test(application_state_tests core/ApplicationStateTests.cpp application_state)
codecairn_core_test(
    reference_candidate_index_tests core/ReferenceCandidateIndexTests.cpp reference_candidate_index
)
codecairn_core_test(
    language_tests
    core/LanguageTests.cpp
    languages
    -o
    language-results.xml,junitxml
    -o
    language-results.txt,txt
)
set_tests_properties(languages PROPERTIES WORKING_DIRECTORY "${PROJECT_BINARY_DIR}")
target_compile_definitions(
    language_tests
    PRIVATE CODECAIRN_LANGUAGE_FIXTURES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/languages"
)
codecairn_core_test(cli_tests cli/CliTests.cpp cli)
add_dependencies(cli_tests codecairn-cli)
target_compile_definitions(cli_tests PRIVATE CODECAIRN_CLI_PATH="$<TARGET_FILE:codecairn-cli>")

foreach(target index_parity_tests reference_candidate_index_tests language_tests)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/core/internal")
endforeach()

option(CODECAIRN_BUILD_BENCHMARKS "Build and register benchmark fixtures" ON)
if(CODECAIRN_BUILD_BENCHMARKS)
    codecairn_core_test(workflow_benchmarks benchmarks/WorkflowBenchmarks.cpp workflows)
    set_tests_properties(workflows PROPERTIES LABELS benchmark)
endif()

if(NOT CODECAIRN_BUILD_GUI)
    return()
endif()

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
add_executable(launcher_fixture fixtures/LauncherFixture.cpp)
target_link_libraries(launcher_fixture PRIVATE Qt6::Core)
set_target_properties(launcher_fixture PROPERTIES MACOSX_BUNDLE FALSE WIN32_EXECUTABLE FALSE)
qt_add_executable(launcher_tests ui/LauncherTests.cpp)
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

function(
    codecairn_test
    target
    source
    name
)
    qt_add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE codecairn_ui_qt Qt6::Test)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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
    ui/PrototypeTests.cpp
    prototype
    -o
    test-results.xml,junitxml
    -o
    -,txt
)
codecairn_test(
    workspace_tests
    ui/WorkspaceTests.cpp
    workspace
    -o
    workspace-results.xml,junitxml
    -o
    workspace-results.txt,txt
)
codecairn_test(
    configuration_tests
    ui/ConfigurationTests.cpp
    configuration
    -o
    configuration-results.xml,junitxml
    -o
    configuration-results.txt,txt
)
codecairn_test(
    interaction_tests
    ui/InteractionTests.cpp
    interactions
    -o
    interaction-results.xml,junitxml
    -o
    interaction-results.txt,txt
)
codecairn_test(
    startup_tests
    ui/StartupTests.cpp
    startup
    -o
    startup-results.xml,junitxml
    -o
    startup-results.txt,txt
)
codecairn_test(refactoring_tests ui/RefactoringTests.cpp refactoring)
codecairn_test(project_tests ui/ProjectTests.cpp projects)
codecairn_test(
    diagnostics_tests
    ui/DiagnosticsTests.cpp
    diagnostics
    -o
    diagnostics-results.xml,junitxml
    -o
    -,txt
)
codecairn_test(
    reference_tests
    ui/ReferenceTests.cpp
    references
    -o
    reference-results.xml,junitxml
    -o
    reference-results.txt,txt
)
codecairn_test(symbol_scaling_tests ui/SymbolScalingTests.cpp symbol_scaling)
foreach(
    target
    workspace_tests
    refactoring_tests
    diagnostics_tests
    symbol_scaling_tests
    reference_tests
    codecairn_tests
    configuration_tests
    interaction_tests
    startup_tests
)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/core/internal")
endforeach()
if(CODECAIRN_BUILD_BENCHMARKS)
    codecairn_test(indexing_benchmarks benchmarks/IndexingBenchmarks.cpp large_indexing)
    target_include_directories(
        indexing_benchmarks PRIVATE "${PROJECT_SOURCE_DIR}/src/core/internal"
    )
    set_tests_properties(
        large_indexing
        PROPERTIES LABELS
                   benchmark
                   TIMEOUT
                   900
    )
    codecairn_test(
        codecairn_benchmarks
        benchmarks/RefactoringBenchmarks.cpp
        benchmarks
        -iterations
        1
        -o
        benchmark-results.txt,txt
    )
    set_tests_properties(benchmarks PROPERTIES LABELS benchmark)
endif()
foreach(target codecairn_tests configuration_tests interaction_tests)
    target_sources(${target} PRIVATE fixtures/fixtures.qrc)
endforeach()
