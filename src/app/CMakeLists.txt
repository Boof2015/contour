include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.3650.58/microsoft.web.webview2.1.0.3650.58.nupkg
    URL_HASH SHA256=911a472128c82ac8baa0c486c23342cc9dd6e7dc50d754e676726642ca065c60
    DOWNLOAD_NAME webview2.zip DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json webview2)

add_library(contour-app-model STATIC model.cpp audio_connection.cpp)
contour_target(contour-app-model)
target_include_directories(contour-app-model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(contour-app-model PUBLIC contour-profile-device-session contour-eq-import nlohmann_json::nlohmann_json)

add_library(contour-app-setup STATIC setup.cpp)
contour_target(contour-app-setup)
target_link_libraries(contour-app-setup PUBLIC contour-app-model shell32 user32)
add_executable(contour-endpoint-check "${PROJECT_SOURCE_DIR}/tools/deployment/endpoint_check.cpp")
contour_target(contour-endpoint-check)
target_link_libraries(contour-endpoint-check PRIVATE contour-app-setup)
add_executable(contour WIN32 main.cpp)
contour_target(contour)
target_include_directories(contour SYSTEM PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(contour PRIVATE contour-app-model contour-app-setup
    "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    shlwapi version user32 shell32 dwmapi)
add_custom_target(contour-ui-assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:contour>/setup"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/tools/deployment/contour-setup.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/DeviceSetup.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/AudioPolicy.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/SharedSetup.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/EndpointDeployment.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/LegacyMigration.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/ControlContext.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/Deployment.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/RegistryStore.cs"
        "$<TARGET_FILE_DIR:contour>/setup"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:contour>/ui"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:contour>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" "$<TARGET_FILE_DIR:contour>/licenses/json.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${webview2_SOURCE_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:contour>/licenses/WebView2.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${webview2_SOURCE_DIR}/NOTICE.txt" "$<TARGET_FILE_DIR:contour>/licenses/WebView2-NOTICE.txt")
add_dependencies(contour contour-ui-assets contour-apo contour-deploy-info contour-apo-host contour-endpoint-check)
if(BUILD_TESTING)
    add_executable(contour-app-tests "${PROJECT_SOURCE_DIR}/tests/app_tests.cpp")
    contour_target(contour-app-tests)
    target_include_directories(contour-app-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(contour-app-tests PRIVATE contour-app-model)
    add_executable(contour-setup-tests "${PROJECT_SOURCE_DIR}/tests/setup_tests.cpp")
    contour_target(contour-setup-tests)
    target_link_libraries(contour-setup-tests PRIVATE contour-app-setup)
    add_test(NAME desktop-setup-boundary COMMAND contour-setup-tests)
    add_test(NAME device-setup-recovery COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${PROJECT_SOURCE_DIR}/tests/device_setup_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/device-setup-tests")
    set_tests_properties(device-setup-recovery PROPERTIES TIMEOUT 180)
    add_test(NAME desktop-model COMMAND contour-app-tests "${CMAKE_BINARY_DIR}/desktop-model-tests")
    find_program(CONTOUR_NODE_EXECUTABLE node)
    if(CONTOUR_NODE_EXECUTABLE)
        add_test(NAME desktop-ui-bridge COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_bridge_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui")
        set_tests_properties(desktop-ui-bridge PROPERTIES TIMEOUT 30)
        add_test(NAME desktop-setup-scan COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_setup_scan_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui")
        set_tests_properties(desktop-setup-scan PROPERTIES TIMEOUT 30)
        add_test(NAME desktop-ui-interactions COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_interaction_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_BINARY_DIR}/ui-interaction-checks")
        set_tests_properties(desktop-ui-interactions PROPERTIES TIMEOUT 60)
        add_test(NAME desktop-curve-preview COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_curve_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE:contour-app-tests>")
    endif()
endif()
