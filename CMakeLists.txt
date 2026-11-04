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

add_library(contour-eq-import STATIC src/eq/peq_import.cpp)
contour_target(contour-eq-import)
target_link_libraries(contour-eq-import PUBLIC contour-dsp)

add_library(contour-profile STATIC src/eq/profile.cpp)
contour_target(contour-profile)
target_link_libraries(contour-profile PUBLIC contour-dsp ole32)

add_library(contour-live-eq STATIC src/dsp/live_eq.cpp)
contour_target(contour-live-eq)
target_link_libraries(contour-live-eq PUBLIC contour-dsp audioeng)

add_executable(contour-eq-inspect tools/eq_inspect/main.cpp)
contour_target(contour-eq-inspect)
target_link_libraries(contour-eq-inspect PRIVATE contour-eq-import contour-profile)

add_library(contour-apo SHARED src/apo/gain_apo.cpp src/apo/dll.cpp src/apo/contour-apo.def)
contour_target(contour-apo)
target_link_libraries(contour-apo PRIVATE contour-dsp AudioBaseProcessingObjectV140
    audiomediatypecrt audioeng ole32 uuid advapi32 contour-profile contour-live-eq)
if(MSVC)
    # The SDK media-type archive requests atls.lib through a default-library
    # directive, but its objects need no ATL symbols. Keep the build independent
    # of that optional VS component; real unresolved symbols still fail the link.
    target_link_options(contour-apo PRIVATE /NODEFAULTLIB:atls.lib)
    # Microsoft APO guidance forbids an embedded manifest in protected audiodg.
    target_link_options(contour-apo PRIVATE /MANIFEST:NO)
endif()

add_executable(contour-deploy-info tools/deployment/info.cpp)
contour_target(contour-deploy-info)
target_include_directories(contour-deploy-info PRIVATE include)
target_link_libraries(contour-deploy-info PRIVATE AudioBaseProcessingObjectV140
    audioeng ole32 uuid advapi32)

add_executable(contour-audio-test tools/audio_test/main.cpp)
contour_target(contour-audio-test)
target_link_libraries(contour-audio-test PRIVATE ole32 uuid avrt contour-profile)

include(CTest)
if(BUILD_TESTING)
    add_executable(contour-apo-host tests/apo_host/main.cpp)
    contour_target(contour-apo-host)
    target_include_directories(contour-apo-host PRIVATE include tests)
    target_link_libraries(contour-apo-host PRIVATE audiomediatypecrt ole32 uuid contour-profile)
    if(MSVC)
        target_link_options(contour-apo-host PRIVATE /NODEFAULTLIB:atls.lib)
    endif()
    add_dependencies(contour-apo-host contour-apo)
    add_test(NAME apo-contract COMMAND contour-apo-host $<TARGET_FILE:contour-apo>)

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
    add_test(NAME tone-analysis COMMAND contour-audio-test --self-test)

    add_test(NAME deployment-recovery COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/deployment_tests.ps1"
        -BinaryDirectory $<TARGET_FILE_DIR:contour-apo>
        -WorkDirectory "${CMAKE_BINARY_DIR}/deployment-tests")
    set_tests_properties(deployment-recovery PROPERTIES TIMEOUT 180)
endif()
