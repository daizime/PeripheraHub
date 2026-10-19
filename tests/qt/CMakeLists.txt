# These executables use fake backends or pure UI fixtures, never native device I/O.
function(add_hub_qt_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${name} PRIVATE Qt6::Widgets)
    set_target_properties(${name} PROPERTIES AUTOMOC ON)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /utf-8)
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${name}")
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"
        "QT_QPA_PLATFORM=offscreen"
        "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
        $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/artifacts/${name}")
endfunction()

add_hub_qt_test(runtime_session_tests runtime_session_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/RuntimeSession.cpp")
add_hub_qt_test(headset_details_tests headset_details_tests.cpp)
target_link_libraries(headset_details_tests PRIVATE peripheral_widgets peripheral_core)
add_hub_qt_test(mouse_details_tests mouse_details_tests.cpp)
target_link_libraries(mouse_details_tests PRIVATE peripheral_widgets peripheral_core)

add_hub_qt_test(device_profiles_format_tests device_profiles_format_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/DeviceProfiles.cpp")
target_link_libraries(device_profiles_format_tests PRIVATE peripheral_core)
add_hub_qt_test(profile_tracker_tests profile_tracker_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/DeviceProfiles.cpp"
    "${PROJECT_SOURCE_DIR}/src/app/ProfileTracker.cpp" "${PROJECT_SOURCE_DIR}/src/app/DeviceService.h")
target_link_libraries(profile_tracker_tests PRIVATE peripheral_core)
add_hub_qt_test(device_service_tests device_service_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/app/DeviceService.cpp" "${PROJECT_SOURCE_DIR}/src/app/DeviceService.h")
add_hub_qt_test(device_grid_tests device_grid_tests.cpp)
target_link_libraries(device_grid_tests PRIVATE peripheral_widgets)
add_hub_qt_test(wheel_boundary_tests wheel_boundary_tests.cpp)
target_link_libraries(wheel_boundary_tests PRIVATE peripheral_widgets)
add_hub_qt_test(dynamic_device_tests dynamic_device_tests.cpp ${PERIPHERAL_APP_SOURCES})
target_link_libraries(dynamic_device_tests PRIVATE peripheral_widgets peripheral_core)
