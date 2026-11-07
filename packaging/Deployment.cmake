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
    if(Qt6_VERSION VERSION_LESS 6.7)
        message(FATAL_ERROR "Bundled Linux packages require Qt 6.7 or newer.")
    endif()
    find_program(CODECAIRN_PATCHELF patchelf REQUIRED)
    set(codecairn_linux_executables "\"bin/cairn\"")
    if(CODECAIRN_CLANGD)
        install(PROGRAMS "${CODECAIRN_CLANGD}" DESTINATION bin)
        string(APPEND codecairn_linux_executables " \"bin/clangd\"")
    endif()
    # The loader, glibc, and graphics-driver libraries must come from the host.
    set(codecairn_linux_system_libraries
        "/(ld-linux[^/]*|lib(c|m|dl|pthread|rt|resolv|util|nss_[^/]*)\\.so[^/]*|lib(GL|GLX|GLdispatch|EGL|OpenGL|vulkan)\\.so[^/]*)$"
    )
    qt_generate_deploy_script(
        TARGET
        codecairn
        OUTPUT_SCRIPT
        codecairn_install_deploy
        CONTENT
        "qt_deploy_runtime_dependencies(
            EXECUTABLE \"$<TARGET_FILE:codecairn>\"
            ADDITIONAL_EXECUTABLES ${codecairn_linux_executables}
            GENERATE_QT_CONF NO_TRANSLATIONS
            BIN_DIR bin LIB_DIR lib PLUGINS_DIR plugins
            POST_EXCLUDE_REGEXES [==[${codecairn_linux_system_libraries}]==]
        )"
    )
    install(
        CODE "
        if(POLICY CMP0207)
            set(CMAKE_POLICY_DEFAULT_CMP0207 NEW)
            cmake_policy(SET CMP0207 NEW)
        endif()
    "
    )
    install(SCRIPT "${codecairn_install_deploy}")
    set(codecairn_linux_application "")
    set(codecairn_linux_launcher "")
    set(codecairn_qt_plugins "")
    if(CODECAIRN_RELEASE)
        set(codecairn_linux_application "$<TARGET_FILE:codecairn>")
        set(codecairn_linux_launcher "$<TARGET_FILE:cairn>")
        get_target_property(codecairn_qmake Qt6::qmake IMPORTED_LOCATION)
        execute_process(
            COMMAND "${codecairn_qmake}" -query QT_INSTALL_PLUGINS
            OUTPUT_VARIABLE codecairn_qt_plugins
            OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/InstallLinux.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/InstallLinux.cmake.in" @ONLY
    )
    file(
        GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/InstallLinux-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/InstallLinux.cmake.in"
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallLinux-$<CONFIG>.cmake")

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
