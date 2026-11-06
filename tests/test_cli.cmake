# Run only through CTest after building camera_demo and test_decoder.
file(MAKE_DIRECTORY "${TEST_DIR}")
set(missing "${TEST_DIR}/missing.jpg")
file(REMOVE "${missing}")

function(check_cli expected_status expected_text)
    execute_process(COMMAND "${CAMERA_DEMO}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 5)
    if(NOT "${status}" STREQUAL "${expected_status}")
        message(FATAL_ERROR "camera_demo ${ARGN}: expected ${expected_status}, got ${status}\n${stdout}${stderr}")
    endif()
    string(FIND "${stdout}${stderr}" "${expected_text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "camera_demo ${ARGN}: missing '${expected_text}'\n${stdout}${stderr}")
    endif()
endfunction()

check_cli(0 "Usage:" --help)
foreach(value IN ITEMS -1 12junk 18446744073709551616)
    check_cli(1 "invalid value for --frames" --jpeg-file "${missing}" --frames "${value}")
endforeach()
# Explicit empty arguments must be preserved rather than dropped by list expansion.
execute_process(COMMAND "${CAMERA_DEMO}" --jpeg-file "${missing}" --frames ""
    RESULT_VARIABLE status ERROR_VARIABLE stderr TIMEOUT 5)
if(NOT "${status}" STREQUAL "1" OR NOT stderr MATCHES "invalid value for --frames")
    message(FATAL_ERROR "empty --frames was not rejected: ${status}: ${stderr}")
endif()
foreach(option IN ITEMS --width --height --fps)
    foreach(value IN ITEMS 0 -1 12junk 2147483648)
        check_cli(1 "${option}" --jpeg-file "${missing}" "${option}" "${value}")
    endforeach()
endforeach()
check_cli(1 "invalid value for --fps" --jpeg-file "${missing}" --fps 1000000001)
check_cli(1 "missing value for --width" --jpeg-file "${missing}" --width)
check_cli(1 "unknown option --bogus" --jpeg-file "${missing}" --bogus 1)
check_cli(1 "provide exactly one" --device /dev/videoX --jpeg-file "${missing}")
check_cli(1 "provide exactly one")
check_cli(1 "cannot open JPEG file:" --jpeg-file "${missing}" --frames 0)

set(fixture "${TEST_DIR}/sample.jpg")
execute_process(COMMAND "${FIXTURE_GENERATOR}" "${fixture}"
    RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr TIMEOUT 5)
if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "JPEG fixture generation failed: ${status}\n${stdout}${stderr}")
endif()
set(ppm "${TEST_DIR}/first.ppm")
file(REMOVE "${ppm}")
check_cli(0 "decode_errors=0" --jpeg-file "${fixture}" --width 4 --height 4
    --frames 30 --fps 100 --save-ppm "${ppm}")
if(NOT EXISTS "${ppm}")
    message(FATAL_ERROR "successful replay did not save a PPM")
endif()
# Use a file as a parent component: this path cannot be created on any platform.
check_cli(1 "cannot create PPM file:" --jpeg-file "${fixture}" --width 4 --height 4
    --frames 0 --fps 100 --save-ppm "${fixture}/first.ppm")
