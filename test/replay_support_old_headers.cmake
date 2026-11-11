file(MAKE_DIRECTORY "${WORK}/headers")
execute_process(COMMAND git show "fd380aafe1df5a5379216d70c4fe4bfcf7f4b5db:cli/fossilize_feature_filter.hpp"
    WORKING_DIRECTORY "${SOURCE}" OUTPUT_FILE "${WORK}/headers/fossilize_feature_filter.hpp"
    RESULT_VARIABLE status TIMEOUT 15)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot read actual input FeatureFilter header: ${status}")
endif()
execute_process(COMMAND "${COMPILER}" -std=c++14 "-I${WORK}/headers" "-I${SOURCE}/khronos"
    "-I${SOURCE}/cli/volk" "-I${SOURCE}/cli" "${SOURCE}/test/replay_support_old_caller.cpp"
    "${UTILS}" "${VOLK}" -pthread -ldl -o "${WORK}/old-bool-caller"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 90)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Old bool caller compilation/link failed: ${status}\n${output}\n${errors}")
endif()
execute_process(COMMAND "${WORK}/old-bool-caller" RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 15)
if(NOT status EQUAL 0 OR NOT output MATCHES "M19 actual input FeatureFilter bool caller supported/rejected/recovery passed")
    message(FATAL_ERROR "Old bool caller behavior failed: ${status}\n${output}\n${errors}")
endif()
message(STATUS "${output}")
