if(NOT CODECAIRN_BUNDLE_RUNTIME)
    return()
endif()

if(WIN32)
    get_target_property(codecairn_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(codecairn_qt_bin "${codecairn_qmake}" DIRECTORY)
    find_program(
        CODECAIRN_WINDEPLOYQT
        NAMES windeployqt6 windeployqt
        HINTS "${codecairn_qt_bin}"
        NO_DEFAULT_PATH REQUIRED
    )
    if(NOT CMAKE_OBJDUMP)
        message(FATAL_ERROR "Windows deployment requires an objdump-compatible dependency scanner.")
    endif()

    set(codecairn_deploy_script "${CMAKE_CURRENT_LIST_DIR}/DeployWindows.cmake")
    add_custom_target(
        deploy ALL
        COMMAND
            "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:codecairn>"
            "-DDESTINATION=$<TARGET_FILE_DIR:codecairn>" "-DWINDEPLOYQT=${CODECAIRN_WINDEPLOYQT}"
            "-DQT_BIN=${codecairn_qt_bin}" "-DOBJDUMP=${CMAKE_OBJDUMP}"
            "-DCLANGD=${CODECAIRN_CLANGD}" "-DCLANG_RESOURCE_ROOT=${codecairn_clang_resource_root}"
            "-DCLANG_RESOURCE_DESTINATION=$<TARGET_FILE_DIR:codecairn>/../lib/clang" -P
            "${codecairn_deploy_script}"
        DEPENDS codecairn
        COMMENT "Deploying Qt plugins and non-system runtime DLLs for direct launch"
        VERBATIM
    )
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/InstallWindows.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/InstallWindows.cmake" @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallWindows.cmake")
    if(BUILD_TESTING)
        # The app-local qt.conf also applies to QtTest in the build directory.
        add_custom_command(
            TARGET deploy
            POST_BUILD
            COMMAND
                "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                "$<TARGET_FILE_DIR:codecairn>/plugins/platforms/"
            VERBATIM
        )
        add_test(
            NAME windows_runtime
            COMMAND
                "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:codecairn>"
                "-DCLANG_RESOURCE_DESTINATION=$<TARGET_FILE_DIR:codecairn>/../lib/clang"
                "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/windows-runtime-check"
                "-DEXPECTED_VERSION=${PROJECT_VERSION}" "-DOBJDUMP=${CMAKE_OBJDUMP}" -P
                "${CMAKE_CURRENT_LIST_DIR}/VerifyWindowsRuntime.cmake"
        )
        set_tests_properties(windows_runtime PROPERTIES TIMEOUT 90)
    endif()
elseif(APPLE)
    get_target_property(codecairn_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(codecairn_qt_bin "${codecairn_qmake}" DIRECTORY)
    find_program(CODECAIRN_MACDEPLOYQT macdeployqt HINTS "${codecairn_qt_bin}" REQUIRED)
    find_file(CODECAIRN_COCOA_PLUGIN libqcocoa.dylib
              HINTS "${codecairn_qt_bin}/../share/qt/plugins/platforms"
                    "${codecairn_qt_bin}/../plugins/platforms" REQUIRED
    )
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/InstallMacOS.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/InstallMacOS.cmake" @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallMacOS.cmake")
else()
    # Qt deploys a bin/lib/plugins tree on Linux.
    qt_generate_deploy_app_script(
        TARGET
        codecairn
        OUTPUT_SCRIPT
        codecairn_install_deploy
        NO_TRANSLATIONS
    )
    install(SCRIPT "${codecairn_install_deploy}")

    set(codecairn_install_clang_resources "lib/clang")
    file(
        GLOB
        codecairn_clang_includes
        CONFIGURE_DEPENDS
        "${codecairn_clang_resource_root}/*/include"
    )
    if(NOT codecairn_clang_includes)
        message(
            FATAL_ERROR "No Clang resource headers found under ${codecairn_clang_resource_root}."
        )
    endif()
    foreach(include_dir IN LISTS codecairn_clang_includes)
        get_filename_component(version_dir "${include_dir}" DIRECTORY)
        get_filename_component(version "${version_dir}" NAME)
        install(DIRECTORY "${include_dir}"
                DESTINATION "${codecairn_install_clang_resources}/${version}"
        )
    endforeach()
endif()
