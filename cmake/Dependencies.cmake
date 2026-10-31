include_guard(GLOBAL)
include(FetchContent)
include(CheckCSourceCompiles)
include(CMakePushCheckState)

find_package(
    Qt6 6.5 REQUIRED
    COMPONENTS Core
               Gui
               Widgets
               Core5Compat
               Concurrent
)
find_package(SQLite3 REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h REQUIRED)
find_library(TREE_SITTER_LIBRARY NAMES tree-sitter REQUIRED)
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h REQUIRED)
find_library(LIBCLANG_LIBRARY NAMES clang libclang REQUIRED)
get_filename_component(codecairn_llvm_lib "${LIBCLANG_LIBRARY}" DIRECTORY)
find_program(CODECAIRN_CLANGD NAMES clangd HINTS "${codecairn_llvm_lib}/../bin")

if(TARGET SQLite3::SQLite3)
    set(codecairn_sqlite SQLite3::SQLite3)
else()
    set(codecairn_sqlite SQLite::SQLite3)
endif()

cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_INCLUDES "${TREE_SITTER_INCLUDE_DIR}")
check_c_source_compiles(
    "
    #include <tree_sitter/api.h>
    #if TREE_SITTER_LANGUAGE_VERSION < 15 || TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION > 14
    #error Tree-sitter must support grammar ABI versions 14 and 15
    #endif
    int main(void) { return 0; }
"
    CODECAIRN_TREE_SITTER_COMPATIBLE
)
cmake_pop_check_state()
if(NOT CODECAIRN_TREE_SITTER_COMPATIBLE)
    message(
        FATAL_ERROR
            "Install a Tree-sitter runtime development package supporting grammar ABI 14 and 15."
    )
endif()

option(CODECAIRN_ALLOW_DOWNLOADS
       "Allow fetching pinned sources when no local source override is supplied" ON
)

function(
    codecairn_source
    name
    override
    url
    hash
)
    set(default_source "")
    if(EXISTS "${PROJECT_SOURCE_DIR}/vendor/${name}")
        set(default_source "${PROJECT_SOURCE_DIR}/vendor/${name}")
    endif()
    set(${override}
        "${default_source}"
        CACHE PATH "Existing upstream ${name} source directory (no download)"
    )
    if(NOT
       "${${override}}"
       STREQUAL
       ""
    )
        get_filename_component(
            source
            "${${override}}"
            ABSOLUTE
            BASE_DIR
            "${PROJECT_SOURCE_DIR}"
        )
        message(STATUS "${name}: using local sources at ${source}")
    elseif(CODECAIRN_ALLOW_DOWNLOADS)
        FetchContent_Declare(
            ${name}
            URL "${url}"
            URL_HASH "SHA256=${hash}"
            DOWNLOAD_EXTRACT_TIMESTAMP
            FALSE
            # Populate sources only; targets are defined below.
            SOURCE_SUBDIR
            codecairn-unused
        )
        FetchContent_MakeAvailable(${name})
        set(source "${${name}_SOURCE_DIR}")
    else()
        message(
            FATAL_ERROR
                "Downloads are disabled. Set ${override} to an extracted/vendored ${name} source tree."
        )
    endif()
    foreach(required IN LISTS ARGN)
        if(NOT EXISTS "${source}/${required}")
            message(
                FATAL_ERROR
                    "${name}: missing ${required} in ${source}. Use the documented upstream source layout."
            )
        endif()
    endforeach()
    set(${name}_SOURCE_DIR
        "${source}"
        PARENT_SCOPE
    )
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/SourceArchives.cmake")

# Build the upstream Qt wrapper directly; QScintilla is a different integration.
file(
    GLOB
    scintilla_core
    CONFIGURE_DEPENDS
    "${scintilla_SOURCE_DIR}/src/*.cxx"
)
set(scintilla_qt_sources "${scintilla_SOURCE_DIR}/qt/ScintillaEditBase")
add_library(
    scintilla_qt STATIC
    ${scintilla_core}
    "${scintilla_qt_sources}/PlatQt.cpp"
    "${scintilla_qt_sources}/ScintillaQt.cpp"
    "${scintilla_qt_sources}/ScintillaEditBase.cpp"
    "${scintilla_qt_sources}/ScintillaQt.h"
    "${scintilla_qt_sources}/ScintillaEditBase.h"
)
target_include_directories(
    scintilla_qt SYSTEM PUBLIC "${scintilla_SOURCE_DIR}/include" "${scintilla_SOURCE_DIR}/src"
                               "${scintilla_qt_sources}"
)
target_compile_definitions(scintilla_qt PUBLIC SCINTILLA_QT=1 EXPORT_IMPORT_API=)
target_link_libraries(
    scintilla_qt
    PUBLIC Qt6::Widgets
    PRIVATE Qt6::Core5Compat
)
set_target_properties(scintilla_qt PROPERTIES CXX_STANDARD 17)

include("${CMAKE_CURRENT_LIST_DIR}/LanguageGrammars.cmake")
