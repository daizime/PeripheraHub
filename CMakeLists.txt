cmake_minimum_required(VERSION 3.21)
project(PeripheralHub VERSION 0.0.0 LANGUAGES CXX)
# Public initial preview. Windows numeric resources require three version fields.
set(PERIPHERAL_APP_VERSION "0.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
option(PERIPHERAL_BUILD_APP "Build the Qt desktop application" ON)

set(PERIPHERAL_APP_SOURCES
    src/app/DeviceService.h src/app/DeviceService.cpp
    src/app/DeviceProfiles.h src/app/DeviceProfiles.cpp
    src/app/BindingSelection.h src/app/BindingSelection.cpp
    src/app/AppSettings.h src/app/AppSettings.cpp
    src/app/ProfileTracker.h src/app/ProfileTracker.cpp
    src/app/SettingsRestore.h src/app/SettingsRestore.cpp
    src/app/MainWindow.h src/app/MainWindow.cpp)
list(TRANSFORM PERIPHERAL_APP_SOURCES PREPEND "${PROJECT_SOURCE_DIR}/")

add_library(peripheral_core
    src/core/device_registry.cpp
    src/core/ddc_capabilities.cpp
    src/core/hid_discovery.cpp
    src/core/vgn_protocol.cpp src/core/vgn_config.cpp)
target_sources(peripheral_core PRIVATE src/core/n9_protocol.cpp)
target_include_directories(peripheral_core PUBLIC src)
if(MSVC)
    target_compile_options(peripheral_core PRIVATE /W4 /utf-8)
endif()

if(BUILD_TESTING)
    add_executable(vgn_config_tests tests/vgn_config_tests.cpp)
    target_link_libraries(vgn_config_tests PRIVATE peripheral_core)
    if(MSVC)
        target_compile_options(vgn_config_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME vgn_config_tests COMMAND vgn_config_tests)
    add_executable(n9_protocol_tests tests/n9_protocol_tests.cpp)
    target_link_libraries(n9_protocol_tests PRIVATE peripheral_core)
    add_test(NAME n9_protocol_tests COMMAND n9_protocol_tests)
    add_executable(core_tests tests/core_tests.cpp)
    target_link_libraries(core_tests PRIVATE peripheral_core)
    if(MSVC)
        target_compile_options(core_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME core_tests COMMAND core_tests)
    add_executable(ddc_tests tests/ddc_tests.cpp)
    target_link_libraries(ddc_tests PRIVATE peripheral_core)
    if(MSVC)
        target_compile_options(ddc_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME ddc_tests COMMAND ddc_tests)
    add_executable(hid_discovery_tests tests/hid_discovery_tests.cpp)
    target_link_libraries(hid_discovery_tests PRIVATE peripheral_core)
    add_executable(settings_restore_tests tests/settings_restore_tests.cpp src/app/SettingsRestore.cpp)
    target_include_directories(settings_restore_tests PRIVATE src)
    if(MSVC)
        target_compile_options(hid_discovery_tests PRIVATE /W4 /utf-8)
        target_compile_options(settings_restore_tests PRIVATE /W4 /utf-8)
    endif()
    add_test(NAME hid_discovery_tests COMMAND hid_discovery_tests)
    add_test(NAME settings_restore_tests COMMAND settings_restore_tests)
endif()

if(WIN32)
    add_library(peripheral_windows
        src/platform/WindowsHid.cpp
        src/platform/WindowsAudio.cpp
        src/platform/WindowsN9.cpp
        src/platform/WindowsMonitor.cpp)
    target_include_directories(peripheral_windows PUBLIC src)
    target_compile_definitions(peripheral_windows PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(peripheral_windows PRIVATE peripheral_core setupapi cfgmgr32 hid ole32 uuid propsys dxva2 user32)
    if(MSVC)
        target_compile_options(peripheral_windows PRIVATE /W4 /utf-8)
    endif()
    # Hardware acceptance probe. It is not packaged and performs at most one
    # whitelisted write, only when --apply is passed on the command line.
    add_executable(vgn_dpi_write_probe tools/vgn-write-probe/main.cpp)
    target_link_libraries(vgn_dpi_write_probe PRIVATE peripheral_core peripheral_windows)
    add_executable(n9_peq_write_probe tools/n9-peq-write-probe/main.cpp)
    target_link_libraries(n9_peq_write_probe PRIVATE peripheral_core peripheral_windows)
    add_executable(hid_inventory tools/hid-inventory/main.cpp)
    target_link_libraries(hid_inventory PRIVATE peripheral_core peripheral_windows)
    if(MSVC)
        target_compile_options(vgn_dpi_write_probe PRIVATE /W4 /utf-8)
        target_compile_options(n9_peq_write_probe PRIVATE /W4 /utf-8)
        target_compile_options(hid_inventory PRIVATE /W4 /utf-8)
    endif()
    if(PERIPHERAL_BUILD_APP)
        find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)
        # Embed resources through a normalized generated source, independent of Windows code page.
        file(GLOB_RECURSE theme_assets CONFIGURE_DEPENDS src/app/widgets/icons/*.png)
        set(theme_source "${CMAKE_CURRENT_BINARY_DIR}/UiThemeResources.cpp")
        add_custom_command(OUTPUT "${theme_source}"
            COMMAND "${CMAKE_COMMAND}" "-DRCC=$<TARGET_FILE:Qt6::rcc>"
                "-DINPUT=${PROJECT_SOURCE_DIR}/src/app/widgets/UiTheme.qrc"
                "-DOUTPUT=${theme_source}"
                -P "${PROJECT_SOURCE_DIR}/cmake/GenerateResourceSource.cmake"
            DEPENDS src/app/widgets/UiTheme.qrc src/app/widgets/Glass.qss ${theme_assets}
                third_party/fluent-system-icons/LICENSE third_party/fluent-system-icons/manifest.json
                cmake/GenerateResourceSource.cmake Qt6::rcc VERBATIM)
        add_library(peripheral_widgets OBJECT
            src/app/widgets/MouseDetails.h src/app/widgets/MouseDetails.cpp
            src/app/widgets/HeadsetDetails.h src/app/widgets/HeadsetDetails.cpp
            src/app/widgets/DeviceCard.h src/app/widgets/DeviceCard.cpp
            src/app/widgets/DeviceGrid.h src/app/widgets/DeviceGrid.cpp
            src/app/widgets/UiTheme.h src/app/widgets/UiTheme.cpp "${theme_source}"
            src/app/widgets/MonitorControls.h src/app/widgets/MonitorControls.cpp
            src/app/widgets/PercentDetails.h src/app/widgets/PercentDetails.cpp
            src/app/widgets/SettingsPage.h src/app/widgets/SettingsPage.cpp)
        set_target_properties(peripheral_widgets PROPERTIES AUTOMOC ON)
        target_include_directories(peripheral_widgets PUBLIC src)
        target_link_libraries(peripheral_widgets PUBLIC Qt6::Widgets)
        if(MSVC)
            target_compile_options(peripheral_widgets PRIVATE /W4 /utf-8)
        endif()
        if(BUILD_TESTING)
            add_executable(device_profiles_tests tests/device_profiles_tests.cpp src/app/DeviceProfiles.cpp)
            target_include_directories(device_profiles_tests PRIVATE src)
            target_link_libraries(device_profiles_tests PRIVATE Qt6::Core peripheral_core)
            if(MSVC)
                target_compile_options(device_profiles_tests PRIVATE /W4 /utf-8)
            endif()
            add_test(NAME device_profiles_tests COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}" $<TARGET_FILE:device_profiles_tests>)
            set_tests_properties(device_profiles_tests PROPERTIES TIMEOUT 20)
            add_executable(app_settings_tests tests/app_settings_tests.cpp src/app/AppSettings.cpp)
            target_include_directories(app_settings_tests PRIVATE src)
            target_link_libraries(app_settings_tests PRIVATE Qt6::Core)
            if(MSVC)
                target_compile_options(app_settings_tests PRIVATE /W4 /utf-8)
            endif()
            add_test(NAME app_settings_tests COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}" $<TARGET_FILE:app_settings_tests>)
            set_tests_properties(app_settings_tests PROPERTIES TIMEOUT 20)
        endif()
        enable_language(RC)
        # 同时标记 Windows 文件属性和程序界面，数值版本保留给资源编译器。
        configure_file(src/app/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/PeripheralHubVersion.rc" @ONLY)
        configure_file(src/app/widgets/icons/app/PeripheralHub.ico
            "${CMAKE_CURRENT_BINARY_DIR}/PeripheralHub.ico" COPYONLY)
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/PeripheralHubVersion.rc"
            PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/PeripheralHub.ico")
        add_executable(PeripheralHub WIN32
            "${CMAKE_CURRENT_BINARY_DIR}/PeripheralHubVersion.rc"
            src/app/main.cpp
            src/app/RuntimeSession.cpp
            ${PERIPHERAL_APP_SOURCES})
        set_target_properties(PeripheralHub PROPERTIES AUTOMOC ON)
        target_include_directories(PeripheralHub PRIVATE src)
        target_compile_definitions(PeripheralHub PRIVATE PERIPHERAL_APP_VERSION="${PERIPHERAL_APP_VERSION}")
        target_link_libraries(PeripheralHub PRIVATE peripheral_core peripheral_windows peripheral_widgets)
        if(MSVC)
            target_compile_options(PeripheralHub PRIVATE /W4 /utf-8)
        endif()
        install(TARGETS PeripheralHub RUNTIME DESTINATION .)
        if(BUILD_TESTING)
            add_subdirectory(tests/qt)
        endif()
    endif()
endif()
