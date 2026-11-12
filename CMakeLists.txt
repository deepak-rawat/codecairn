cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET
        "26.0"
        CACHE STRING "Minimum supported macOS"
    )
    set(CMAKE_OSX_ARCHITECTURES
        "arm64"
        CACHE STRING "Supported macOS architecture"
    )
endif()

project(
    CodeCairn
    VERSION 0.1.3
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

include(CTest)
option(CODECAIRN_BUILD_GUI "Build the Qt Widgets application and launcher" ON)
set(CODECAIRN_DATA_DIRECTORY share/codecairn)
if(APPLE AND CODECAIRN_BUILD_GUI)
    set(CODECAIRN_DATA_DIRECTORY codecairn.app/Contents/Resources/share/codecairn)
endif()
option(CODECAIRN_BUNDLE_RUNTIME "Deploy runtime dependencies alongside CodeCairn" ON)
option(CODECAIRN_RELEASE "Require complete release runtime dependencies" OFF)
if(APPLE AND CODECAIRN_RELEASE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET
        "26.0"
        CACHE STRING "" FORCE
    )
    set(CMAKE_OSX_ARCHITECTURES
        "arm64"
        CACHE STRING "" FORCE
    )
endif()
include(cmake/Dependencies.cmake)
if(CODECAIRN_RELEASE AND NOT CODECAIRN_CLANGD)
    message(FATAL_ERROR "Release builds require clangd.")
endif()

add_library(
    codecairn_core STATIC
    src/core/include/codecairn/CoreSession.h
    src/core/CoreSession.cpp
    src/core/internal/DefinitionResolver.h
    src/core/include/codecairn/DocumentAnalysis.h
    src/core/include/codecairn/WorkspaceIndex.h
    src/core/include/codecairn/WorkspaceServices.h
    src/core/include/codecairn/LanguageServerClient.h
    src/core/include/codecairn/NavigationService.h
    src/core/include/codecairn/ReferenceSession.h
    src/core/Diagnostics.cpp
    src/core/ApplicationStateStore.cpp
    src/core/SourceAnalysis.cpp
    src/core/LanguageRegistry.cpp
    src/core/CompilerSymbolRecovery.cpp
    src/core/DocumentAnalysis.cpp
    src/core/WorkspaceOptionsStore.cpp
    src/core/WorkspaceServices.cpp
    src/core/DefinitionResolver.cpp
    src/core/SourceCoordinates.cpp
    src/core/CompilationDatabase.cpp
    src/core/FileIO.cpp
    src/core/ProjectDiscovery.cpp
    src/core/ProjectSession.cpp
    src/core/NavigationTarget.cpp
    src/core/NavigationService.cpp
    src/core/ReferenceSession.cpp
    src/core/ReferenceSearch.cpp
    src/core/ReferenceQuery.cpp
    src/core/ReferenceHistory.cpp
    src/core/NavigationHistory.cpp
    src/core/WorkspaceIndex.cpp
    src/core/WorkspacePaths.cpp
    src/core/SymbolIndexStore.cpp
    src/core/ReferenceCandidateIndex.cpp
    src/core/WorkspaceScanner.cpp
    src/core/WorkspaceIndexBuilder.cpp
    src/core/WorkspaceSymbolQuery.cpp
    src/core/LanguageServerClient.cpp
)
target_include_directories(
    codecairn_core
    PUBLIC src/core/include
    PRIVATE src/core/internal "${TREE_SITTER_INCLUDE_DIR}" "${LIBCLANG_INCLUDE_DIR}"
)
target_link_libraries(
    codecairn_core
    PUBLIC Qt6::Core Qt6::Concurrent
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
if(WIN32)
    target_link_libraries(codecairn_core PRIVATE psapi)
endif()
target_compile_options(
    codecairn_core PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
                           "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
)
get_filename_component(codecairn_libclang_directory "${LIBCLANG_LIBRARY}" DIRECTORY)
set(codecairn_clang_resource_root "${codecairn_libclang_directory}/clang")
target_compile_definitions(
    codecairn_core
    PRIVATE CODECAIRN_CLANG_RESOURCE_ROOT="${codecairn_clang_resource_root}"
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
add_executable(codecairn-cli src/cli/Cli.cpp)
target_link_libraries(codecairn-cli PRIVATE codecairn_core)
target_compile_definitions(codecairn-cli PRIVATE CODECAIRN_VERSION="${PROJECT_VERSION}")
set_target_properties(codecairn-cli PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
if(WIN32)
    set_target_properties(
        codecairn-cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin"
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(NOT CODECAIRN_BUILD_GUI)
    if(CODECAIRN_BUNDLE_RUNTIME)
        message(STATUS "Headless install uses system runtime dependencies; bundling is GUI-only.")
    endif()
    install(TARGETS codecairn-cli RUNTIME DESTINATION bin)
    install(
        FILES LICENSE
        DESTINATION share/codecairn/licenses
        RENAME CodeCairn.txt
    )
    install(FILES packaging/THIRD-PARTY-NOTICES.txt DESTINATION share/codecairn/licenses)
    return()
endif()

add_library(
    codecairn_ui_qt STATIC
    src/ui/AppSettings.cpp
    src/ui/CodeEditor.cpp
    src/ui/SourceDocument.cpp
    src/ui/SymbolBrowser.cpp
    src/ui/SymbolCatalogModel.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindowUi.cpp
    src/ui/MainWindowWorkspace.cpp
    src/ui/MainWindowProjects.cpp
    src/ui/ProjectDialog.cpp
    src/ui/MainWindowSettings.cpp
    src/ui/MainWindowNavigation.cpp
    src/ui/MainWindowLinks.cpp
    src/ui/MainWindowPanels.cpp
    src/ui/MainWindowReferences.cpp
    src/ui/ReferenceResultsView.cpp
    src/ui/NavigationTree.cpp
    src/ui/UiAppearance.cpp
    resources.qrc
)
target_link_libraries(codecairn_ui_qt PUBLIC codecairn_core scintilla_qt)
target_include_directories(codecairn_ui_qt PUBLIC src/ui)
target_compile_options(
    codecairn_ui_qt PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
                            "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
)
qt_add_executable(codecairn src/ui/main.cpp)
target_link_libraries(codecairn PRIVATE codecairn_ui_qt)
target_compile_definitions(codecairn PRIVATE CODECAIRN_VERSION="${PROJECT_VERSION}")
add_executable(cairn src/ui/Launcher.cpp)
target_link_libraries(cairn PRIVATE Qt6::Core)
target_compile_definitions(cairn PRIVATE CODECAIRN_VERSION="${PROJECT_VERSION}")
set_target_properties(cairn PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
if(WIN32)
    enable_language(RC)
    target_sources(codecairn PRIVATE assets/icons/codecairn.rc)
    target_include_directories(codecairn PRIVATE "${PROJECT_SOURCE_DIR}/assets/icons")
    set_source_files_properties(
        assets/icons/codecairn.rc PROPERTIES OBJECT_DEPENDS
                                             "${PROJECT_SOURCE_DIR}/assets/icons/codecairn.ico"
    )
    set_target_properties(codecairn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set_target_properties(cairn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
elseif(APPLE)
    target_sources(codecairn PRIVATE assets/icons/codecairn.icns)
    set_source_files_properties(
        assets/icons/codecairn.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
    set_target_properties(
        codecairn
        PROPERTIES MACOSX_BUNDLE_ICON_FILE codecairn.icns
                   MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/Info.plist.in"
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
if(UNIX
   AND NOT APPLE
   AND CODECAIRN_BUNDLE_RUNTIME
)
    set_target_properties(codecairn cairn codecairn-cli PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

install(
    TARGETS codecairn
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
if(APPLE)
    install(PROGRAMS "$<TARGET_FILE:cairn>" DESTINATION codecairn.app/Contents/MacOS)
    install(PROGRAMS "$<TARGET_FILE:codecairn-cli>" DESTINATION codecairn.app/Contents/MacOS)
    if(NOT CODECAIRN_BUNDLE_RUNTIME)
        install(TARGETS cairn codecairn-cli RUNTIME DESTINATION bin)
    endif()
else()
    install(TARGETS cairn codecairn-cli RUNTIME DESTINATION bin)
endif()
install(FILES config/default-settings.json DESTINATION "${CODECAIRN_DATA_DIRECTORY}")
install(
    FILES LICENSE
    DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses"
    RENAME CodeCairn.txt
)
if(UNIX AND NOT APPLE)
    install(FILES assets/org.codecairn.CodeCairn.desktop DESTINATION share/applications)
    install(FILES assets/icons/codecairn.svg DESTINATION share/icons/hicolor/scalable/apps)
    foreach(
        size
        16
        24
        32
        48
        64
        128
        256
        512
    )
        install(
            FILES "assets/icons/codecairn-${size}.png"
            DESTINATION "share/icons/hicolor/${size}x${size}/apps"
            RENAME codecairn.png
        )
    endforeach()
endif()
install(
    FILES "${scintilla_SOURCE_DIR}/License.txt"
    DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses"
    RENAME Scintilla.txt
)
install(FILES packaging/THIRD-PARTY-NOTICES.txt DESTINATION "${CODECAIRN_DATA_DIRECTORY}/licenses")
include(packaging/Deployment.cmake)
include(packaging/CPack.cmake)
