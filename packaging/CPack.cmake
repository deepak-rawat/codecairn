if(NOT CODECAIRN_BUNDLE_RUNTIME)
    return()
endif()

if(CODECAIRN_RELEASE
   AND WIN32
   AND NOT
       CMAKE_SIZEOF_VOID_P
       EQUAL
       8
)
    message(FATAL_ERROR "Windows releases require an x64 toolchain.")
endif()
if(CODECAIRN_RELEASE AND APPLE)
    find_program(CODECAIRN_BREW brew REQUIRED)
endif()
if(CODECAIRN_RELEASE OR APPLE)
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/PackageMetadata.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PackageMetadata.cmake" @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/PackageMetadata.cmake")
endif()

set(CPACK_PACKAGE_NAME codecairn)
set(CPACK_PACKAGE_VENDOR "CodeCairn contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_SOURCE_GENERATOR "")
if(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_PACKAGE_FILE_NAME "codecairn-${PROJECT_VERSION}-macos26-arm64-unsigned")
    set(CPACK_DMG_VOLUME_NAME "CodeCairn ${PROJECT_VERSION}")
    set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK OFF)
elseif(WIN32)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME "codecairn-${PROJECT_VERSION}-windows11-x64")
else()
    set(CPACK_GENERATOR TGZ)
    set(CPACK_PACKAGE_FILE_NAME "codecairn-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
include(CPack)
