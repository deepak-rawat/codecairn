cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum supported macOS")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Supported macOS architecture")
endif()

project(
    CodeCairn
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(CTest)
set(CODECAIRN_DATA_DIRECTORY share/codecairn)
if(APPLE)
    set(CODECAIRN_DATA_DIRECTORY codecairn.app/Contents/Resources/share/codecairn)
endif()
option(CODECAIRN_BUNDLE_RUNTIME "Deploy runtime dependencies alongside CodeCairn" ON)
option(CODECAIRN_RELEASE "Require complete release runtime dependencies" OFF)
if(APPLE AND CODECAIRN_RELEASE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "" FORCE)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()
include(cmake/Dependencies.cmake)
if(CODECAIRN_RELEASE AND NOT CODECAIRN_CLANGD)
    message(FATAL_ERROR "Release builds require clangd.")
endif()

add_library(
    codecairn_core STATIC
    src/AppSettings.cpp
    src/Diagnostics.cpp
    src/ApplicationStateStore.cpp
    src/SourceAnalysis.cpp
    src/LanguageRegistry.cpp
    src/CompilerSymbolRecovery.cpp
    src/CodeEditor.cpp
    src/SourceDocument.cpp
    src/WorkspaceOptionsStore.cpp
    src/WorkspaceServices.cpp
    src/DefinitionResolver.cpp
    src/SymbolBrowser.cpp
    src/SourceCoordinates.cpp
    src/CompilationDatabase.cpp
    src/FileIO.cpp
    src/MainWindow.cpp
    src/MainWindowUi.cpp
    src/MainWindowWorkspace.cpp
    src/MainWindowSettings.cpp
    src/MainWindowNavigation.cpp
    src/MainWindowLinks.cpp
    src/MainWindowPanels.cpp
    src/MainWindowReferences.cpp
    src/ReferenceSearch.cpp
    src/ReferenceQuery.cpp
    src/ReferenceHistory.cpp
    src/ReferenceResultsView.cpp
    src/NavigationHistory.cpp
    src/NavigationTree.cpp
    src/UiAppearance.cpp
    src/WorkspaceIndex.cpp
    src/WorkspacePaths.cpp
    src/SymbolIndexStore.cpp
    src/ReferenceCandidateIndex.cpp
    src/WorkspaceScanner.cpp
    src/WorkspaceIndexBuilder.cpp
    src/WorkspaceSymbolQuery.cpp
    src/LanguageServerClient.cpp
    resources.qrc
)
target_include_directories(
    codecairn_core
    PUBLIC src
    PRIVATE "${TREE_SITTER_INCLUDE_DIR}" "${LIBCLANG_INCLUDE_DIR}"
)
target_link_libraries(
    codecairn_core
    PUBLIC scintilla_qt Qt6::Concurrent
    PRIVATE c_grammar
            cpp_grammar
            python_grammar
            bash_grammar
            javascript_grammar
            typescript_grammar
            tsx_grammar
            go_grammar
            rust_grammar
            json_grammar
            yaml_grammar
            cmake_grammar
            "${TREE_SITTER_LIBRARY}"
            "${LIBCLANG_LIBRARY}"
            ${codecairn_sqlite}
            re2::re2
)
target_compile_options(
    codecairn_core PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
                            "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
)
get_filename_component(codecairn_libclang_directory "${LIBCLANG_LIBRARY}" DIRECTORY)
set(codecairn_clang_resource_root "${codecairn_libclang_directory}/clang")
target_compile_definitions(
    codecairn_core PRIVATE CODECAIRN_CLANG_RESOURCE_ROOT="${codecairn_clang_resource_root}"
                            CODECAIRN_BUNDLE_RUNTIME=$<BOOL:${CODECAIRN_BUNDLE_RUNTIME}>
                            CODECAIRN_BUILD_CONFIGURATION="$<CONFIG>"
                            CODECAIRN_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
                            CODECAIRN_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
)
if(CODECAIRN_CLANGD)
    target_compile_definitions(
        codecairn_core PRIVATE CODECAIRN_CONFIGURED_CLANGD="${CODECAIRN_CLANGD}"
    )
endif()
qt_add_executable(codecairn src/main.cpp)
target_link_libraries(codecairn PRIVATE codecairn_core)
target_compile_definitions(codecairn PRIVATE CODECAIRN_VERSION="${PROJECT_VERSION}")
add_executable(cairn src/Launcher.cpp)
target_link_libraries(cairn PRIVATE Qt6::Core)
target_compile_definitions(cairn PRIVATE CODECAIRN_VERSION="${PROJECT_VERSION}")
set_target_properties(cairn PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
if(WIN32)
    enable_language(RC)
    target_sources(codecairn PRIVATE assets/icons/codecairn.rc)
    target_include_directories(codecairn PRIVATE "${PROJECT_SOURCE_DIR}/assets/icons")
    set_source_files_properties(
        assets/icons/codecairn.rc
        PROPERTIES OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/assets/icons/codecairn.ico"
    )
    set_target_properties(codecairn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set_target_properties(cairn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
elseif(APPLE)
    target_sources(codecairn PRIVATE assets/icons/codecairn.icns)
    set_source_files_properties(
        assets/icons/codecairn.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
    set_target_properties(
        codecairn PROPERTIES MACOSX_BUNDLE_ICON_FILE codecairn.icns
                            MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/Info.plist.in"
    )
endif()
set_target_properties(
    codecairn
    PROPERTIES WIN32_EXECUTABLE TRUE
               MACOSX_BUNDLE TRUE
               MACOSX_BUNDLE_GUI_IDENTIFIER "org.codecairn.CodeCairn"
               MACOSX_BUNDLE_BUNDLE_NAME "CodeCairn"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
)
if(UNIX AND NOT APPLE AND CODECAIRN_BUNDLE_RUNTIME)
    set_target_properties(codecairn PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(
    TARGETS codecairn
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
install(TARGETS cairn RUNTIME DESTINATION bin)
if(APPLE)
    install(PROGRAMS "$<TARGET_FILE:cairn>" DESTINATION codecairn.app/Contents/MacOS)
endif()
install(FILES config/default-settings.json DESTINATION "${CODECAIRN_DATA_DIRECTORY}")
install(FILES LICENSE DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses" RENAME CodeCairn.txt)
if(UNIX AND NOT APPLE)
    install(FILES assets/org.codecairn.CodeCairn.desktop DESTINATION share/applications)
    install(FILES assets/icons/codecairn.svg DESTINATION share/icons/hicolor/scalable/apps)
    foreach(size 16 24 32 48 64 128 256 512)
        install(
            FILES "assets/icons/codecairn-${size}.png"
            DESTINATION "share/icons/hicolor/${size}x${size}/apps"
            RENAME codecairn.png
        )
    endforeach()
endif()
include(cmake/Deployment.cmake)
install(
    FILES "${scintilla_SOURCE_DIR}/License.txt"
    DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses"
    RENAME Scintilla.txt
)
