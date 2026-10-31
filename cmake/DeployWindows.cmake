cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

foreach(
    required
    EXECUTABLE
    DESTINATION
    WINDEPLOYQT
    QT_BIN
    OBJDUMP
    CLANG_RESOURCE_ROOT
    CLANG_RESOURCE_DESTINATION
)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing deployment argument: ${required}")
    endif()
endforeach()

set(ENV{PATH} "${QT_BIN};$ENV{PATH}")
execute_process(
    COMMAND "${WINDEPLOYQT}" --no-translations --compiler-runtime --dir "${DESTINATION}" --plugindir
            "${DESTINATION}/plugins" "${EXECUTABLE}" RESULT_VARIABLE deployment_result
)
if(NOT
   deployment_result
   EQUAL
   0
)
    message(FATAL_ERROR "windeployqt failed: ${deployment_result}")
endif()

# windeployqt does not cover arbitrary libraries such as Tree-sitter or all
# transitive dependencies in the MSYS2 Qt distribution.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")
set(executables "${EXECUTABLE}")
file(GLOB compiler_resource_includes "${CLANG_RESOURCE_ROOT}/*/include")
if(NOT compiler_resource_includes)
    message(FATAL_ERROR "No Clang resource headers found under ${CLANG_RESOURCE_ROOT}.")
endif()
foreach(include_dir IN LISTS compiler_resource_includes)
    get_filename_component(version_dir "${include_dir}" DIRECTORY)
    get_filename_component(version "${version_dir}" NAME)
    file(COPY "${include_dir}" DESTINATION "${CLANG_RESOURCE_DESTINATION}/${version}")
endforeach()
if(CLANGD AND EXISTS "${CLANGD}")
    file(
        COPY_FILE
        "${CLANGD}"
        "${DESTINATION}/clangd.exe"
        ONLY_IF_DIFFERENT
    )
    list(APPEND executables "${DESTINATION}/clangd.exe")
    get_filename_component(clang_bin "${CLANGD}" DIRECTORY)
    file(GLOB resource_includes "${clang_bin}/../lib/clang/*/include")
    foreach(include_dir IN LISTS resource_includes)
        get_filename_component(version_dir "${include_dir}" DIRECTORY)
        get_filename_component(version "${version_dir}" NAME)
        file(COPY "${include_dir}" DESTINATION "${CLANG_RESOURCE_DESTINATION}/${version}")
    endforeach()
endif()
file(GLOB_RECURSE plugin_dlls "${DESTINATION}/plugins/*.dll")
file(
    GET_RUNTIME_DEPENDENCIES
    EXECUTABLES
    ${executables}
    MODULES
    ${plugin_dlls}
    DIRECTORIES
    "${DESTINATION}"
    "${QT_BIN}"
    PRE_EXCLUDE_REGEXES
    "^[aA][pP][iI]-[mM][sS]-"
    "^[eE][xX][tT]-[mM][sS]-"
    POST_EXCLUDE_REGEXES
    ".*[\\/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][\\/].*"
    RESOLVED_DEPENDENCIES_VAR
    resolved
    UNRESOLVED_DEPENDENCIES_VAR
    unresolved
    CONFLICTING_DEPENDENCIES_PREFIX
    conflicts
)
if(unresolved)
    message(FATAL_ERROR "Unresolved application/plugin dependencies: ${unresolved}")
endif()
foreach(name IN LISTS conflicts_FILENAMES)
    # The active UCRT64 prefix is authoritative when a previous deployment
    # already contains a copy of a dependency also reached through a new DLL.
    if(NOT EXISTS "${QT_BIN}/${name}")
        message(FATAL_ERROR "Conflicting runtime dependency outside the toolchain prefix: ${name}")
    endif()
    file(
        COPY_FILE
        "${QT_BIN}/${name}"
        "${DESTINATION}/${name}"
        ONLY_IF_DIFFERENT
    )
endforeach()
foreach(library IN LISTS resolved)
    get_filename_component(name "${library}" NAME)
    if(NOT
       "${library}"
       STREQUAL
       "${DESTINATION}/${name}"
    )
        file(
            COPY_FILE
            "${library}"
            "${DESTINATION}/${name}"
            ONLY_IF_DIFFERENT
        )
    endif()
endforeach()
file(WRITE "${DESTINATION}/qt.conf" "[Paths]\nPrefix=.\nPlugins=plugins\n")
message(STATUS "Direct-launch runtime deployed beside ${EXECUTABLE}")
