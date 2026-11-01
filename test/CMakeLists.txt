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

# Actual utility processes, portable CMake process control, no GPU or shell.
if (FOSSILIZE_CLI)
    add_executable(utility-parse-test utility_parse_test.cpp)
    target_link_libraries(utility-parse-test PRIVATE fossilize)
    target_compile_options(utility-parse-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(utility-parse-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    foreach(tool list rehash prune)
        set(cases version invalid-json empty populated mixed)
        if(tool STREQUAL "list")
            list(APPEND cases dependency-failure empty-dependencies legacy-invocations)
        elseif(tool STREQUAL "prune")
            list(APPEND cases rejected-application)
        endif()
        foreach(case IN LISTS cases)
            set(name utility-parse-${tool}-${case})
            set(work "${CMAKE_CURRENT_BINARY_DIR}/${name}")
            file(MAKE_DIRECTORY "${work}")
            add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
                -DGENERATOR=$<TARGET_FILE:utility-parse-test>
                -DTOOL=$<TARGET_FILE:fossilize-${tool}>
                -DTOOL_NAME=${tool} -DCASE=${case} -DWORK=${work}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/utility_parse.cmake)
            set_tests_properties(${name} PROPERTIES TIMEOUT 90 WORKING_DIRECTORY "${work}")
        endforeach()
    endforeach()
endif()

# Shared file completion has no Vulkan or CLI-library dependency.
foreach(helper_target file-helper-test file-helper-compatibility-test)
    if(helper_target STREQUAL "file-helper-test")
        set(helper_source file_helper_test.cpp)
    else()
        set(helper_source file_helper_compatibility_test.cpp)
    endif()
    add_executable(${helper_target} ${helper_source} ${CMAKE_SOURCE_DIR}/cli/file.cpp)
    target_include_directories(${helper_target} PRIVATE ${CMAKE_SOURCE_DIR}/cli)
    target_compile_options(${helper_target} PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(${helper_target} PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
endforeach()
foreach(kind string buffer)
    set(helper_cases open short-write flush close first-error success filesystem)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND helper_cases dev-full)
    endif()
    foreach(case IN LISTS helper_cases)
        set(name file-helper-${kind}-${case})
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        add_test(NAME ${name} COMMAND file-helper-test ${kind} ${case})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endforeach()
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/file-helper-legacy")
add_test(NAME file-helper-legacy-compatibility COMMAND file-helper-compatibility-test)
set_tests_properties(file-helper-legacy-compatibility PROPERTIES TIMEOUT 30
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/file-helper-legacy")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME file-helper-legacy-completion COMMAND ${CMAKE_COMMAND}
        -DCALLER=$<TARGET_FILE:file-helper-compatibility-test>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/file_helper_completion.cmake)
    set_tests_properties(file-helper-legacy-completion PROPERTIES TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/file-helper-legacy")
endif()

# Include the production robust Linux controller in a private syscall fixture.
# No injection symbols or options are added to the shipped replayer.
if(FOSSILIZE_CLI AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(fork-child-setup-test fork_child_setup_test.cpp)
    target_link_libraries(fork-child-setup-test PRIVATE fossilize cli-utils)
    target_compile_options(fork-child-setup-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(fork-child-setup-test PROPERTIES LINK_FLAGS
        "${FOSSILIZE_LINK_FLAGS} -Wl,--wrap=fork,--wrap=pipe,--wrap=pthread_sigmask,--wrap=dup2,--wrap=getppid,--wrap=waitpid,--wrap=read,--wrap=epoll_wait,--wrap=exit")
    set(child_setup_cases healthy healthy-recovery parent-death prctl-failure parent-race)
    foreach(site sigmask stdout-dup2 stdin-dup2)
        foreach(position initial-first initial-last recovery)
            list(APPEND child_setup_cases ${position}-${site})
        endforeach()
    endforeach()
    foreach(case IN LISTS child_setup_cases)
        set(name fork-child-${case})
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        add_test(NAME ${name} COMMAND fork-child-setup-test ${case})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endforeach()
endif()

# Test-only adapters retain real master loops, fork/CreateProcess, pipes and OS
# waits. They are compiled into this executable alone, never the shipping CLI.
if(FOSSILIZE_CLI AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    add_executable(replay-controller-test replay_controller_test.cpp)
    target_link_libraries(replay-controller-test PRIVATE fossilize cli-utils)
    target_compile_options(replay-controller-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(replay-controller-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    foreach(case clean zero empty-archive lost lost-progress mixed-success-first mixed-loss-first recover
                 skip-graphics skip-compute skip-raytracing skip-graphics-final skip-compute-final
                 skip-raytracing-final skip-graphics-progress skip-graphics-final-progress
                 partial loss-after-retry held-sibling signal status-error outcome-state)
        set(name replay-controller-${case})
        set(work "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        file(MAKE_DIRECTORY "${work}")
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DCONTROLLER=$<TARGET_FILE:replay-controller-test> -DCASE=${case}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_controller.cmake)
        set_tests_properties(${name} PROPERTIES TIMEOUT 45 WORKING_DIRECTORY "${work}")
    endforeach()
endif()
if(TARGET replay-controller-test)
    add_executable(replay-controller-compatibility-test replay_controller_compatibility_test.cpp)
    target_link_libraries(replay-controller-compatibility-test PRIVATE fossilize)
    target_compile_options(replay-controller-compatibility-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(replay-controller-compatibility-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    foreach(case normal master progress e5 e6 compatibility)
        set(name replay-controller-cli-${case})
        set(work "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        file(MAKE_DIRECTORY "${work}")
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DGENERATOR=$<TARGET_FILE:replay-controller-test>
            -DREPLAY=$<TARGET_FILE:fossilize-replay>
            -DCALLER=$<TARGET_FILE:replay-controller-compatibility-test>
            -DCASE=${case} -DWORK=${work}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_controller_cli.cmake)
        set_tests_properties(${name} PROPERTIES TIMEOUT 90 WORKING_DIRECTORY "${work}")
    endforeach()
endif()

if(FOSSILIZE_CLI)
    add_executable(replay-outcome-test replay_outcome_test.cpp)
    target_link_libraries(replay-outcome-test PRIVATE fossilize cli-utils)
    target_compile_options(replay-outcome-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(replay-outcome-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    if(FOSSILIZE_REPLAYER_SPIRV_VAL)
        target_compile_definitions(replay-outcome-test PRIVATE FOSSILIZE_REPLAYER_SPIRV_VAL)
        target_link_libraries(replay-outcome-test PRIVATE SPIRV-Tools-static)
    endif()
    set(outcome_cases state parse-read creation compile-required-loop support setup-dependency worker-queue matrix)
    if(FOSSILIZE_REPLAYER_SPIRV_VAL)
        list(APPEND outcome_cases validation)
    endif()
    foreach(case creation-rerun success-policy loop compile-required write-failure legacy-provenance null-provenance whitelist legacy-invocation)
        set(name replay-cache-${case})
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        add_test(NAME ${name} COMMAND replay-outcome-test cache-${case})
        set_tests_properties(${name} PROPERTIES TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endforeach()
    foreach(case IN LISTS outcome_cases)
        set(name replay-outcome-${case})
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        add_test(NAME ${name} COMMAND replay-outcome-test ${case})
        set_tests_properties(${name} PROPERTIES TIMEOUT 90 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endforeach()
    foreach(case good create layout device)
        set(name replay-outcome-entry-${case})
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        add_test(NAME ${name} COMMAND replay-outcome-test --entry ${case} fixture.foz)
        set_tests_properties(${name} PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endforeach()
endif()
if(TARGET replay-outcome-test AND TARGET replay-controller-test)
    add_executable(replay-outcome-compatibility-test replay_outcome_compatibility_test.cpp)
    target_link_libraries(replay-outcome-compatibility-test PRIVATE fossilize)
    target_compile_options(replay-outcome-compatibility-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(replay-outcome-compatibility-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
    set(outcome_cli_cases e7 missing unsupported unsupported-shader library mixed good empty missinghash emptyhash selection hashselection whitelist compatibility)
    if(FOSSILIZE_REPLAYER_SPIRV_VAL)
        list(APPEND outcome_cli_cases validation)
    endif()
    foreach(case IN LISTS outcome_cli_cases)
        set(name replay-outcome-cli-${case})
        set(work "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        file(MAKE_DIRECTORY "${work}")
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            -DGENERATOR=$<TARGET_FILE:replay-outcome-test> -DREPLAY=$<TARGET_FILE:fossilize-replay>
            -DCALLER=$<TARGET_FILE:replay-outcome-compatibility-test> -DCASE=${case} -DWORK=${work}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_outcome_cli.cmake)
        set_tests_properties(${name} PROPERTIES TIMEOUT 180 WORKING_DIRECTORY "${work}")
    endforeach()
    foreach(order success-first failure-first)
        set(progress_cases absent)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND progress_cases progress)
        endif()
        foreach(progress IN LISTS progress_cases)
            set(case orderly-${order}-${progress})
            set(name replay-outcome-controller-${order}-${progress})
            set(work "${CMAKE_CURRENT_BINARY_DIR}/${name}")
            file(MAKE_DIRECTORY "${work}")
            add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
                -DGENERATOR=$<TARGET_FILE:replay-outcome-test> -DCONTROLLER=$<TARGET_FILE:replay-controller-test>
                -DSCENARIO=${case} -DCASE=mixed -DWORK=${work}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/replay_outcome_cli.cmake)
            set_tests_properties(${name} PROPERTIES TIMEOUT 60 WORKING_DIRECTORY "${work}")
        endforeach()
    endforeach()
endif()

# Portable deadline/evidence logic is registered on every supported platform.
add_executable(replay-timeout-test replay_timeout_test.cpp)
target_link_libraries(replay-timeout-test PRIVATE fossilize)
target_compile_options(replay-timeout-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
set_target_properties(replay-timeout-test PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
foreach(case deadlines reset delivery unknown protocol)
    add_test(NAME replay-timeout-unit-${case} COMMAND replay-timeout-test ${case})
    set_tests_properties(replay-timeout-unit-${case} PROPERTIES TIMEOUT 30)
endforeach()
if(FOSSILIZE_CLI AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(replay-timeout-controller-test replay_timeout_controller_test.cpp)
    target_link_libraries(replay-timeout-controller-test PRIVATE fossilize cli-utils)
    target_compile_options(replay-timeout-controller-test PRIVATE ${FOSSILIZE_CXX_FLAGS})
    set_target_properties(replay-timeout-controller-test PROPERTIES LINK_FLAGS
        "${FOSSILIZE_LINK_FLAGS} -Wl,--wrap=_ZNSt6chrono3_V212steady_clock3nowEv,--wrap=pthread_cond_clockwait,--wrap=pthread_kill,--wrap=kill,--wrap=epoll_wait,--wrap=epoll_ctl,--wrap=timerfd_settime,--wrap=timerfd_create,--wrap=read")
    foreach(case clean no-progress heartbeat heartbeat-reset cleanup spontaneous escalation delivery-failure kill-failure external-kill multithread recovery protocol dispatcher-reset dispatcher-disabled ordinary-no-progress ordinary-delivery-failure timer-create-failure timer-arm-failure timer-register-failure)
        set(name replay-timeout-controller-${case})
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
        add_test(NAME ${name} COMMAND replay-timeout-controller-test ${case})
        set_tests_properties(${name} PROPERTIES TIMEOUT 45 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    endforeach()
endif()
