cmake_minimum_required(VERSION 3.16)
project(uvc_capture_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)

add_library(camera_core
    src/latest_frame_slot.cpp
    src/jpeg_decoder.cpp)
target_include_directories(camera_core PUBLIC include)
target_link_libraries(camera_core PUBLIC PkgConfig::TURBOJPEG Threads::Threads)

add_executable(camera_demo src/main.cpp src/v4l2_camera.cpp)
target_link_libraries(camera_demo PRIVATE camera_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_slot tests/test_slot.cpp)
    target_link_libraries(test_slot PRIVATE camera_core)
    add_test(NAME latest_frame_slot COMMAND test_slot)

    add_executable(test_decoder tests/test_decoder.cpp)
    target_link_libraries(test_decoder PRIVATE camera_core)
    add_test(NAME jpeg_decoder COMMAND test_decoder)

    add_test(NAME camera_cli COMMAND ${CMAKE_COMMAND}
        "-DCAMERA_DEMO=$<TARGET_FILE:camera_demo>"
        "-DFIXTURE_GENERATOR=$<TARGET_FILE:test_decoder>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake")
    set_tests_properties(latest_frame_slot jpeg_decoder camera_cli PROPERTIES TIMEOUT 30)
endif()
