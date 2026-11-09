cmake_minimum_required(VERSION 3.18)
project(image_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "image_pipeline targets Linux / Jetson")
endif()
option(ENABLE_CUDA "Build the NVIDIA NPP processing backend" ON)

find_package(Threads REQUIRED)
find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)

add_library(image_core
    src/common/image_io.cpp
    src/input/jpeg_decoder.cpp
    src/processing/cpu.cpp)
target_include_directories(image_core PUBLIC src ${OpenCV_INCLUDE_DIRS})
target_link_libraries(image_core PUBLIC PkgConfig::TURBOJPEG Threads::Threads ${OpenCV_LIBS})
if(ENABLE_CUDA)
    find_package(CUDAToolkit 12.6 REQUIRED)
    target_sources(image_core PRIVATE src/processing/cuda_npp.cpp)
    target_link_libraries(image_core PRIVATE CUDA::cudart CUDA::nppc CUDA::nppif CUDA::nppial CUDA::nppidei)
else()
    target_sources(image_core PRIVATE src/processing/cuda_stub.cpp)
endif()

add_library(pipeline_runtime
    src/app/options.cpp
    src/app/pipeline.cpp
    src/input/v4l2.cpp
    src/preview/preview.cpp)
target_link_libraries(pipeline_runtime PUBLIC image_core)

add_executable(image_pipeline src/app/main.cpp)
target_link_libraries(image_pipeline PRIVATE pipeline_runtime)
add_executable(image_benchmark src/benchmark/main.cpp src/app/options.cpp)
target_link_libraries(image_benchmark PRIVATE image_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_slot tests/test_slot.cpp)
    target_link_libraries(test_slot PRIVATE image_core)
    add_test(NAME latest_slot COMMAND test_slot)
    add_executable(test_decoder tests/test_decoder.cpp)
    target_link_libraries(test_decoder PRIVATE image_core)
    add_test(NAME jpeg_decoder COMMAND test_decoder)
    add_executable(test_processing tests/test_processing.cpp)
    target_link_libraries(test_processing PRIVATE image_core)
    add_test(NAME processing_cpu COMMAND test_processing)
    if(ENABLE_CUDA)
        add_test(NAME processing_cuda COMMAND test_processing --cuda)
        set_tests_properties(processing_cuda PROPERTIES LABELS "cuda" TIMEOUT 120)
    endif()
    add_test(NAME pipeline_cli COMMAND ${CMAKE_COMMAND}
        "-DPIPELINE=$<TARGET_FILE:image_pipeline>"
        "-DFIXTURE_GENERATOR=$<TARGET_FILE:test_decoder>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake")
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME preview_http COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_preview.py"
            "$<TARGET_FILE:image_pipeline>" "$<TARGET_FILE:test_decoder>")
        set_tests_properties(preview_http PROPERTIES TIMEOUT 60)
    endif()
    set_tests_properties(latest_slot jpeg_decoder processing_cpu pipeline_cli PROPERTIES TIMEOUT 30)
endif()
