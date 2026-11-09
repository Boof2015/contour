cmake_minimum_required(VERSION 3.24)
project(Contour VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Contour's APO and diagnostic tools require Windows.")
endif()

function(contour_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    endif()
endfunction()

add_executable(contour-probe tools/audio_probe/main.cpp)
contour_target(contour-probe)
target_link_libraries(contour-probe PRIVATE ole32 uuid)

add_library(contour-dsp STATIC src/dsp/channel_gain.cpp src/dsp/peq.cpp)
contour_target(contour-dsp)
target_include_directories(contour-dsp PUBLIC include)

add_library(contour-eq-import STATIC src/eq/peq_import.cpp src/eq/peq_export.cpp)
contour_target(contour-eq-import)
target_link_libraries(contour-eq-import PUBLIC contour-dsp)

add_library(contour-profile STATIC src/eq/profile.cpp)
contour_target(contour-profile)
target_link_libraries(contour-profile PUBLIC contour-dsp ole32)

add_library(contour-edit-session STATIC src/eq/edit_session.cpp)
contour_target(contour-edit-session)
target_link_libraries(contour-edit-session PUBLIC contour-profile)

add_library(contour-profile-library STATIC src/eq/profile_library.cpp src/eq/file_storage.cpp)
contour_target(contour-profile-library)
target_link_libraries(contour-profile-library PUBLIC contour-edit-session)

add_library(contour-live-eq STATIC src/dsp/live_eq.cpp)
contour_target(contour-live-eq)
target_link_libraries(contour-live-eq PUBLIC contour-dsp audioeng)

add_library(contour-live-control STATIC src/eq/live_control.cpp)
contour_target(contour-live-control)
target_link_libraries(contour-live-control PUBLIC contour-edit-session contour-live-eq propsys uuid bcrypt)

add_library(contour-endpoint-session STATIC src/eq/endpoint_session.cpp)
contour_target(contour-endpoint-session)
target_link_libraries(contour-endpoint-session PUBLIC contour-live-control)

add_library(contour-profile-device-session STATIC src/eq/profile_device_session.cpp src/eq/profile_exit.cpp src/eq/profile_reconciliation.cpp)
contour_target(contour-profile-device-session)
target_link_libraries(contour-profile-device-session PUBLIC contour-profile-library contour-endpoint-session)

add_executable(contour-control-probe tools/control_probe/main.cpp)
contour_target(contour-control-probe)
target_link_libraries(contour-control-probe PRIVATE contour-endpoint-session advapi32)

add_executable(contour-eq-inspect tools/eq_inspect/main.cpp)
contour_target(contour-eq-inspect)
target_link_libraries(contour-eq-inspect PRIVATE contour-eq-import contour-profile)

add_library(contour-apo SHARED src/apo/gain_apo.cpp src/apo/dll.cpp src/apo/contour-apo.def)
contour_target(contour-apo)
target_link_libraries(contour-apo PRIVATE contour-dsp AudioBaseProcessingObjectV140
    audiomediatypecrt audioeng ole32 uuid advapi32 contour-profile contour-live-eq contour-live-control)
if(MSVC)
    # The SDK media-type archive requests atls.lib through a default-library
    # directive, but its objects need no ATL symbols. Keep the build independent
    # of that optional VS component; real unresolved symbols still fail the link.
    target_link_options(contour-apo PRIVATE /NODEFAULTLIB:atls.lib)
    # Microsoft APO guidance forbids an embedded manifest in protected audiodg.
    target_link_options(contour-apo PRIVATE /MANIFEST:NO)
endif()

add_executable(contour-deploy-info tools/deployment/info.cpp tools/deployment/device_inventory.cpp)
contour_target(contour-deploy-info)
target_include_directories(contour-deploy-info PRIVATE include)
target_link_libraries(contour-deploy-info PRIVATE AudioBaseProcessingObjectV140
    audioeng ole32 uuid advapi32 cfgmgr32)

# Explicit offline staging target. This is not the desktop install action and
# does not install a driver, register a COM object, or restart Windows Audio.
add_custom_target(contour-component-package
    COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/deployment/stage-component.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -OutputDirectory "${CMAKE_BINARY_DIR}/component-package/$<CONFIG>"
    DEPENDS contour-apo contour-deploy-info
    VERBATIM)

add_executable(contour-audio-test tools/audio_test/main.cpp)
contour_target(contour-audio-test)
target_link_libraries(contour-audio-test PRIVATE ole32 uuid avrt contour-profile contour-live-control advapi32)

# The setup helper validates the staged APO before attaching it, including in
# builds where the rest of the test suite is disabled.
    add_executable(contour-apo-host tests/apo_host/main.cpp)
    contour_target(contour-apo-host)
    target_include_directories(contour-apo-host PRIVATE include tests)
    target_link_libraries(contour-apo-host PRIVATE audiomediatypecrt ole32 uuid contour-profile contour-live-control contour-endpoint-session)
    if(MSVC)
        target_link_options(contour-apo-host PRIVATE /NODEFAULTLIB:atls.lib)
    endif()
    add_dependencies(contour-apo-host contour-apo)
include(CTest)
if(BUILD_TESTING)
    set(CONTOUR_INFVERIF "" CACHE FILEPATH "Optional WDK InfVerif.exe for component package checks")
    set(CONTOUR_INF2CAT "" CACHE FILEPATH "Optional WDK Inf2Cat.exe for component package checks")
    set(component_validation_tools)
    if(CONTOUR_INFVERIF)
        list(APPEND component_validation_tools -InfVerif "${CONTOUR_INFVERIF}")
    endif()
    if(CONTOUR_INF2CAT)
        list(APPEND component_validation_tools -Inf2Cat "${CONTOUR_INF2CAT}")
    endif()
    add_test(NAME apo-contract COMMAND contour-apo-host $<TARGET_FILE:contour-apo>)
    add_test(NAME apo-component-contract COMMAND contour-apo-host $<TARGET_FILE:contour-apo> --component)
    add_test(NAME apo-packaged-contract COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/apo_package_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/apo-package-tests")
    set_tests_properties(apo-packaged-contract PROPERTIES TIMEOUT 60)
    add_test(NAME component-package COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/component_package_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/component-package-tests"
        ${component_validation_tools})
    set_tests_properties(component-package PROPERTIES TIMEOUT 90)
    add_test(NAME extension-package COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/extension_package_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/extension-package-tests"
        ${component_validation_tools})
    set_tests_properties(extension-package PROPERTIES TIMEOUT 90)
    add_test(NAME endpoint-deployment COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/endpoint_deployment_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/endpoint-deployment-tests")
    set_tests_properties(endpoint-deployment PROPERTIES TIMEOUT 180)
    add_test(NAME shared-device-setup COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_setup_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/shared-setup-tests")
    set_tests_properties(shared-device-setup PROPERTIES TIMEOUT 180)
    add_test(NAME product-removal COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/product_removal_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/product-removal-tests")
    set_tests_properties(product-removal PROPERTIES TIMEOUT 180)

    add_executable(contour-dsp-tests tests/dsp_tests.cpp)
    contour_target(contour-dsp-tests)
    target_include_directories(contour-dsp-tests PRIVATE tests)
    target_link_libraries(contour-dsp-tests PRIVATE contour-dsp)
    add_test(NAME channel-gain COMMAND contour-dsp-tests)
    add_executable(contour-peq-tests tests/peq_tests.cpp)
    contour_target(contour-peq-tests)
    target_include_directories(contour-peq-tests PRIVATE tests)
    target_link_libraries(contour-peq-tests PRIVATE contour-eq-import)
    add_test(NAME parametric-eq COMMAND contour-peq-tests)
    add_executable(contour-live-eq-tests tests/live_eq_tests.cpp)
    contour_target(contour-live-eq-tests)
    target_include_directories(contour-live-eq-tests PRIVATE tests)
    target_link_libraries(contour-live-eq-tests PRIVATE contour-profile contour-live-eq)
    add_test(NAME live-eq COMMAND contour-live-eq-tests "${CMAKE_BINARY_DIR}/live-eq-tests")
    add_executable(contour-edit-session-tests tests/edit_session_tests.cpp)
    contour_target(contour-edit-session-tests)
    target_include_directories(contour-edit-session-tests PRIVATE tests)
    target_link_libraries(contour-edit-session-tests PRIVATE contour-edit-session contour-live-eq contour-eq-import)
    add_test(NAME edit-session COMMAND contour-edit-session-tests "${CMAKE_BINARY_DIR}/edit-session-tests")
    add_executable(contour-profile-library-tests tests/profile_library_tests.cpp)
    contour_target(contour-profile-library-tests)
    target_include_directories(contour-profile-library-tests PRIVATE tests)
    target_link_libraries(contour-profile-library-tests PRIVATE contour-profile-library contour-live-control)
    add_test(NAME profile-library COMMAND contour-profile-library-tests "${CMAKE_BINARY_DIR}/profile-library-tests")
    add_executable(contour-live-control-tests tests/live_control_tests.cpp)
    contour_target(contour-live-control-tests)
    target_include_directories(contour-live-control-tests PRIVATE tests)
    target_link_libraries(contour-live-control-tests PRIVATE contour-live-control)
    add_test(NAME live-control COMMAND contour-live-control-tests)
    add_executable(contour-endpoint-session-tests tests/endpoint_session_tests.cpp)
    contour_target(contour-endpoint-session-tests)
    target_include_directories(contour-endpoint-session-tests PRIVATE tests)
    target_link_libraries(contour-endpoint-session-tests PRIVATE contour-endpoint-session)
    add_test(NAME endpoint-session COMMAND contour-endpoint-session-tests)
    add_executable(contour-profile-device-session-tests tests/profile_device_session_tests.cpp)
    contour_target(contour-profile-device-session-tests)
    target_include_directories(contour-profile-device-session-tests PRIVATE tests)
    target_link_libraries(contour-profile-device-session-tests PRIVATE contour-profile-device-session)
    add_test(NAME profile-device-session COMMAND contour-profile-device-session-tests "${CMAKE_BINARY_DIR}/profile-device-session-tests")
    add_executable(contour-profile-exit-tests tests/profile_exit_tests.cpp)
    contour_target(contour-profile-exit-tests)
    target_include_directories(contour-profile-exit-tests PRIVATE tests)
    target_link_libraries(contour-profile-exit-tests PRIVATE contour-profile-device-session)
    add_test(NAME profile-exit COMMAND contour-profile-exit-tests "${CMAKE_BINARY_DIR}/profile-exit-tests")
    add_executable(contour-profile-reconciliation-tests tests/profile_reconciliation_tests.cpp)
    contour_target(contour-profile-reconciliation-tests)
    target_include_directories(contour-profile-reconciliation-tests PRIVATE tests)
    target_link_libraries(contour-profile-reconciliation-tests PRIVATE contour-profile-device-session)
    add_test(NAME profile-reconciliation COMMAND contour-profile-reconciliation-tests "${CMAKE_BINARY_DIR}/profile-reconciliation-tests")
    add_test(NAME tone-analysis COMMAND contour-audio-test --self-test)

    add_test(NAME deployment-recovery COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/deployment_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/deployment-tests")
    set_tests_properties(deployment-recovery PROPERTIES TIMEOUT 180)
    add_test(NAME control-context-recovery COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/control_context_tests.ps1"
        -WorkDirectory "${CMAKE_BINARY_DIR}/control-context-tests")
    set_tests_properties(control-context-recovery PROPERTIES TIMEOUT 90)
endif()

option(CONTOUR_BUILD_APP "Build the Contour desktop editor (WebView2)" ON)
if(CONTOUR_BUILD_APP)
    add_subdirectory(src/app)
endif()
