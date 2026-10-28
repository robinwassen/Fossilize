add_executable(fossilize-test fossilize_test.cpp)
target_link_libraries(fossilize-test fossilize)
target_compile_options(fossilize-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
set_target_properties(fossilize-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
add_test(NAME fossilize-system-test COMMAND fossilize-test)

add_executable(varint-test varint_test.cpp)
target_link_libraries(varint-test fossilize)
target_compile_options(varint-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
set_target_properties(varint-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
add_test(NAME varint-system-test COMMAND varint-test)

add_executable(application-info-filter-test application_info_filter_test.cpp)
target_link_libraries(application-info-filter-test fossilize)
target_compile_options(application-info-filter-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
set_target_properties(application-info-filter-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
add_test(NAME application-info-filter-test COMMAND application-info-filter-test)

add_executable(object-cache-test object_cache_test.cpp)
target_link_libraries(object-cache-test fossilize)
set_target_properties(object-cache-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
add_test(NAME object-cache-test COMMAND object-cache-test)

if (FOSSILIZE_CLI)
    add_executable(multi-instance-and-device-test multi_instance_and_device_test.cpp)
    target_link_libraries(multi-instance-and-device-test cli-utils fossilize)
    set_target_properties(multi-instance-and-device-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")

    add_executable(feature-filter-test feature_filter_test.cpp)
    target_link_libraries(feature-filter-test cli-utils)
    set_target_properties(feature-filter-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    add_test(NAME feature-filter-test COMMAND feature-filter-test)
endif()

option(FOSSILIZE_TEST_RUNTIME_LAYER "Tests runtime layer" OFF)

if (FOSSILIZE_TEST_RUNTIME_LAYER)
    add_executable(layer-test layer_test.cpp)
    if (NOT WIN32)
        target_link_libraries(layer-test PRIVATE -pthread)
    endif()
    target_link_libraries(layer-test PRIVATE vulkan)
    add_test(NAME layer-test COMMAND layer-test)
    set_property(TEST layer-test APPEND PROPERTY
            ENVIRONMENT VK_LAYER_PATH=${CMAKE_BINARY_DIR}/layer)
    set_property(TEST layer-test APPEND PROPERTY
            ENVIRONMENT VK_INSTANCE_LAYERS=VK_LAYER_fossilize)
    set_property(TEST layer-test APPEND PROPERTY
            ENVIRONMENT FOSSILIZE_DUMP_PATH=${CMAKE_BINARY_DIR}/test-dump)
    set_property(TEST layer-test APPEND PROPERTY
            ENVIRONMENT FOSSILIZE_APPLICATION_INFO_FILTER_PATH=${CMAKE_CURRENT_SOURCE_DIR}/filter.json)
    set_property(TEST layer-test APPEND PROPERTY
            ENVIRONMENT BLOCK_ENV=ZblockZ)
endif()

if (NOT WIN32)
    add_executable(futex-test futex_test.cpp)
    target_link_libraries(futex-test fossilize -pthread)
    if (APPLE)
        add_test(NAME spinlock-futex-test COMMAND futex-test)
    else()
        add_test(NAME linux-futex-test COMMAND futex-test)
    endif()
endif()


# GPU-independent diagnostics exercise production replay/device branches and the
# layer's exported loader entrypoints. Give every process a private output path.
if (FOSSILIZE_CLI)
    foreach(diagnostic replay device)
        add_executable(${diagnostic}-diagnostic-test ${diagnostic}_diagnostic_test.cpp)
        target_link_libraries(${diagnostic}-diagnostic-test PRIVATE fossilize cli-utils)
        target_compile_options(${diagnostic}-diagnostic-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
        set_target_properties(${diagnostic}-diagnostic-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${diagnostic}-diagnostic")
        add_test(NAME ${diagnostic}-diagnostic-test COMMAND ${diagnostic}-diagnostic-test)
        set_tests_properties(${diagnostic}-diagnostic-test PROPERTIES TIMEOUT 60
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${diagnostic}-diagnostic")
    endforeach()
endif()
if (FOSSILIZE_VULKAN_LAYER)
    add_executable(capture-diagnostic-test capture_diagnostic_test.cpp)
    target_include_directories(capture-diagnostic-test PRIVATE ${CMAKE_SOURCE_DIR}/khronos)
    target_link_libraries(capture-diagnostic-test PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(capture-diagnostic-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(capture-diagnostic-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    foreach(mode normal qa paranoid eager binary)
        foreach(kind graphics compute raytracing)
            foreach(outcome reject success vulkan-error)
                set(case_name capture-diagnostic-${mode}-${kind}-${outcome})
                file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${case_name}")
                add_test(NAME ${case_name} COMMAND capture-diagnostic-test
                    $<TARGET_FILE:VkLayer_fossilize> ${mode} ${kind} ${outcome})
                set_tests_properties(${case_name} PROPERTIES TIMEOUT 60
                    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${case_name}")
            endforeach()
        endforeach()
    endforeach()
endif()
if (FOSSILIZE_CLI)
    add_executable(diagnostic-compatibility-test diagnostic_compatibility_test.cpp)
    target_link_libraries(diagnostic-compatibility-test PRIVATE fossilize cli-utils)
    target_compile_options(diagnostic-compatibility-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(diagnostic-compatibility-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/diagnostic-compatibility")
    add_test(NAME diagnostic-compatibility-test COMMAND ${CMAKE_COMMAND}
        -DCALLER=$<TARGET_FILE:diagnostic-compatibility-test>
        -DREPLAY=$<TARGET_FILE:fossilize-replay>
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/diagnostic-compatibility
        -P ${CMAKE_CURRENT_SOURCE_DIR}/diagnostic_compatibility.cmake)
    set_tests_properties(diagnostic-compatibility-test PROPERTIES TIMEOUT 90
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/diagnostic-compatibility")
endif()
