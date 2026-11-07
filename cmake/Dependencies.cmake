include_guard(GLOBAL)
include(FetchContent)
include(CheckCSourceCompiles)
include(CMakePushCheckState)

if(APPLE AND NOT CMAKE_CROSSCOMPILING)
    find_program(
        CODECAIRN_HOMEBREW
        NAMES brew
        HINTS /opt/homebrew/bin /usr/local/bin
    )
    if(CODECAIRN_HOMEBREW)
        execute_process(
            COMMAND "${CODECAIRN_HOMEBREW}" --prefix
            OUTPUT_VARIABLE codecairn_homebrew_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE codecairn_homebrew_error
            RESULT_VARIABLE codecairn_homebrew_result
        )
        if(NOT
           codecairn_homebrew_result
           EQUAL
           0
        )
            message(FATAL_ERROR "Cannot query Homebrew prefix: ${codecairn_homebrew_error}")
        endif()
        if(NOT IS_ABSOLUTE "${codecairn_homebrew_prefix}" OR NOT IS_DIRECTORY
                                                             "${codecairn_homebrew_prefix}"
        )
            message(FATAL_ERROR "Invalid Homebrew prefix: ${codecairn_homebrew_prefix}")
        endif()
        foreach(
            package
            qt
            qt5compat
            tree-sitter
            sqlite
            re2
            llvm
        )
            set(prefix "${codecairn_homebrew_prefix}/opt/${package}")
            if(IS_DIRECTORY "${prefix}")
                list(APPEND CMAKE_PREFIX_PATH "${prefix}")
            endif()
        endforeach()
    endif()
elseif(MINGW AND NOT CMAKE_CROSSCOMPILING)
    get_filename_component(codecairn_compiler_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(codecairn_toolchain_prefix "${codecairn_compiler_bin}" DIRECTORY)
    list(APPEND CMAKE_PREFIX_PATH "${codecairn_toolchain_prefix}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_program(CODECAIRN_LLVM_CONFIG NAMES llvm-config)
    if(CODECAIRN_LLVM_CONFIG)
        execute_process(
            COMMAND "${CODECAIRN_LLVM_CONFIG}" --prefix
            OUTPUT_VARIABLE codecairn_llvm_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE codecairn_llvm_error
            RESULT_VARIABLE codecairn_llvm_result
        )
        if(NOT
           codecairn_llvm_result
           EQUAL
           0
        )
            message(FATAL_ERROR "Cannot query LLVM prefix: ${codecairn_llvm_error}")
        endif()
        if(NOT IS_ABSOLUTE "${codecairn_llvm_prefix}" OR NOT IS_DIRECTORY
                                                         "${codecairn_llvm_prefix}"
        )
            message(FATAL_ERROR "Invalid LLVM prefix: ${codecairn_llvm_prefix}")
        endif()
        list(APPEND CMAKE_PREFIX_PATH "${codecairn_llvm_prefix}")
    endif()
endif()

find_package(
    Qt6 6.5 REQUIRED
    COMPONENTS Core
               Gui
               Widgets
               Core5Compat
               Concurrent
)
find_package(SQLite3 REQUIRED)
find_package(re2 CONFIG QUIET)
if(NOT TARGET re2::re2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(
        CODECAIRN_RE2
        REQUIRED
        IMPORTED_TARGET
        re2
    )
    add_library(re2::re2 ALIAS PkgConfig::CODECAIRN_RE2)
endif()
find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h REQUIRED)
find_library(TREE_SITTER_LIBRARY NAMES tree-sitter REQUIRED)
find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h REQUIRED)
find_library(LIBCLANG_LIBRARY NAMES clang libclang REQUIRED)
get_filename_component(codecairn_llvm_lib "${LIBCLANG_LIBRARY}" DIRECTORY)
find_program(
    CODECAIRN_CLANGD
    NAMES clangd
    HINTS "${codecairn_llvm_lib}/../bin"
)

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
