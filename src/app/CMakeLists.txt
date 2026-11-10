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

add_library(contour-app-model STATIC model.cpp audio_connection.cpp startup.cpp app_info.cpp)
contour_target(contour-app-model)
target_include_directories(contour-app-model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(contour-app-model PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(contour-app-model PUBLIC contour-profile-device-session contour-eq-import nlohmann_json::nlohmann_json advapi32 shell32 user32)

add_library(contour-app-setup STATIC setup.cpp)
contour_target(contour-app-setup)
target_link_libraries(contour-app-setup PUBLIC contour-app-model shell32 user32)
add_executable(contour-endpoint-check "${PROJECT_SOURCE_DIR}/tools/deployment/endpoint_check.cpp")
contour_target(contour-endpoint-check)
target_link_libraries(contour-endpoint-check PRIVATE contour-app-setup)
enable_language(RC)
configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)
add_executable(contour WIN32 main.cpp tray.cpp contour.rc)
contour_target(contour)
target_include_directories(contour PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(contour SYSTEM PRIVATE "${webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(contour PRIVATE contour-app-model contour-app-setup
    "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    shlwapi version user32 shell32 dwmapi)
add_custom_target(contour-ui-assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:contour>/setup"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/tools/deployment/contour-setup.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/contour-uninstall.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/SetupHost.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/ProductRemoval.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/DeviceSetup.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/AudioPolicy.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/SharedSetup.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/EndpointDeployment.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/RuntimeUpdate.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/LegacyMigration.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/ControlContext.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/Deployment.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/EffectPlacement.ps1"
        "${PROJECT_SOURCE_DIR}/tools/deployment/RegistryStore.cs"
        "$<TARGET_FILE_DIR:contour>/setup"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:contour>/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:contour>/LICENSE"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:contour>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" "$<TARGET_FILE_DIR:contour>/licenses/json.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${webview2_SOURCE_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:contour>/licenses/WebView2.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${webview2_SOURCE_DIR}/NOTICE.txt" "$<TARGET_FILE_DIR:contour>/licenses/WebView2-NOTICE.txt")
add_dependencies(contour contour-ui-assets contour-apo contour-deploy-info contour-apo-host contour-endpoint-check)
add_custom_target(contour-release-package
    COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${PROJECT_SOURCE_DIR}/tools/distribution/stage-release.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:contour>"
        -OutputDirectory "${CMAKE_BINARY_DIR}/distribution/$<CONFIG>/Contour-${PROJECT_VERSION}-windows-x64"
        -Version "${PROJECT_VERSION}" -Configuration "$<CONFIG>"
    DEPENDS contour
    VERBATIM)
set(CONTOUR_ISCC "" CACHE FILEPATH "Path to Inno Setup 7.1+ ISCC.exe")
set(CONTOUR_PREREQUISITE_DIRECTORY "" CACHE PATH "Folder containing Microsoft-signed vc_redist.x64.exe and MicrosoftEdgeWebview2Setup.exe")
if(CONTOUR_ISCC AND CONTOUR_PREREQUISITE_DIRECTORY)
    add_custom_target(contour-installer
        COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
            "${PROJECT_SOURCE_DIR}/tools/distribution/build-installer.ps1"
            -PayloadDirectory "${CMAKE_BINARY_DIR}/distribution/$<CONFIG>/Contour-${PROJECT_VERSION}-windows-x64"
            -OutputDirectory "${CMAKE_BINARY_DIR}/distribution/installer"
            -Iscc "${CONTOUR_ISCC}" -PrerequisiteDirectory "${CONTOUR_PREREQUISITE_DIRECTORY}"
        DEPENDS contour-release-package
        VERBATIM)
endif()
if(BUILD_TESTING)
    if(CONTOUR_ISCC)
        add_test(NAME distribution-installer COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
            "${PROJECT_SOURCE_DIR}/tests/installer_tests.ps1"
            -Iscc "${CONTOUR_ISCC}" -WorkDirectory "${CMAKE_BINARY_DIR}/installer-tests")
        set_tests_properties(distribution-installer PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
    add_test(NAME distribution-payload COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${PROJECT_SOURCE_DIR}/tests/release_payload_tests.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:contour>"
        -WorkDirectory "${CMAKE_BINARY_DIR}/release-payload-tests")
    set_tests_properties(distribution-payload PROPERTIES TIMEOUT 60)
    add_executable(contour-lifecycle-tests "${PROJECT_SOURCE_DIR}/tests/lifecycle_tests.cpp")
    contour_target(contour-lifecycle-tests)
    target_link_libraries(contour-lifecycle-tests PRIVATE user32)
    add_dependencies(contour-lifecycle-tests contour)
    add_test(NAME desktop-lifecycle COMMAND contour-lifecycle-tests "$<TARGET_FILE:contour>" "${CMAKE_BINARY_DIR}/lifecycle-tests")
    set_tests_properties(desktop-lifecycle PROPERTIES RUN_SERIAL TRUE TIMEOUT 45)
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
        add_test(NAME ci-draft-release COMMAND "${CONTOUR_NODE_EXECUTABLE}" --test
            "${PROJECT_SOURCE_DIR}/tests/ci_draft_tests.cjs")
        add_test(NAME desktop-ui-bridge COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_bridge_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui")
        set_tests_properties(desktop-ui-bridge PROPERTIES TIMEOUT 30)
        add_test(NAME desktop-setup-scan COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_setup_scan_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui")
        set_tests_properties(desktop-setup-scan PROPERTIES TIMEOUT 30)
        add_test(NAME desktop-browser-runner COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/headless_browser_tests.cjs" "${CMAKE_BINARY_DIR}/browser-runner-checks")
        set_tests_properties(desktop-browser-runner PROPERTIES TIMEOUT 90)
        add_test(NAME desktop-ui-interactions COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_interaction_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_BINARY_DIR}/ui-interaction-checks")
        set_tests_properties(desktop-ui-interactions PROPERTIES TIMEOUT 60)
        add_test(NAME desktop-tray COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/tray_ui_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_BINARY_DIR}/tray-ui-checks")
        set_tests_properties(desktop-tray PROPERTIES TIMEOUT 30)
        add_test(NAME desktop-curve-preview COMMAND "${CONTOUR_NODE_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/ui_curve_tests.cjs" "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE:contour-app-tests>")
    endif()
endif()
