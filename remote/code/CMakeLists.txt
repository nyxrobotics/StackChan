# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(StackChan-RemoteControl-ESPNow)

# The component registry archive is immutable, so apply the audited patch made
# specifically for the pinned 2.5.3 registry source after dependency resolution.
# The reverse check makes repeated CMake configuration idempotent.
find_package(Git REQUIRED)
set(ESPNOW_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp-now")
set(ESPNOW_RUNTIME_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/esp-now-2.5.3-runtime-safety.patch")

if(NOT EXISTS "${ESPNOW_COMPONENT_DIR}/src/espnow/src/espnow.c")
    message(FATAL_ERROR "ESP-NOW managed component was not resolved at ${ESPNOW_COMPONENT_DIR}")
endif()
if(NOT EXISTS "${ESPNOW_RUNTIME_PATCH}")
    message(FATAL_ERROR "ESP-NOW runtime-safety patch was not found at ${ESPNOW_RUNTIME_PATCH}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel
    WORKING_DIRECTORY "${ESPNOW_COMPONENT_DIR}"
    RESULT_VARIABLE ESPNOW_GIT_ROOT_RESULT
    OUTPUT_VARIABLE ESPNOW_GIT_ROOT
    ERROR_VARIABLE ESPNOW_GIT_ROOT_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT ESPNOW_GIT_ROOT_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to locate the Git worktree for ESP-NOW patching: ${ESPNOW_GIT_ROOT_ERROR}")
endif()
file(RELATIVE_PATH ESPNOW_COMPONENT_PREFIX "${ESPNOW_GIT_ROOT}" "${ESPNOW_COMPONENT_DIR}")
file(TO_CMAKE_PATH "${ESPNOW_COMPONENT_PREFIX}" ESPNOW_COMPONENT_PREFIX)
if(ESPNOW_COMPONENT_PREFIX MATCHES "^\\.\\.")
    message(FATAL_ERROR "ESP-NOW component is outside the Git worktree: ${ESPNOW_COMPONENT_DIR}")
endif()
set(ESPNOW_PATCH_DIRECTORY_ARG "--directory=${ESPNOW_COMPONENT_PREFIX}")

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check "${ESPNOW_PATCH_DIRECTORY_ARG}" "${ESPNOW_RUNTIME_PATCH}"
    WORKING_DIRECTORY "${ESPNOW_GIT_ROOT}"
    RESULT_VARIABLE ESPNOW_PATCH_FORWARD_CHECK
    OUTPUT_VARIABLE ESPNOW_PATCH_FORWARD_OUTPUT
    ERROR_VARIABLE ESPNOW_PATCH_FORWARD_ERROR
)
string(CONCAT ESPNOW_PATCH_FORWARD_LOG "${ESPNOW_PATCH_FORWARD_OUTPUT}" "${ESPNOW_PATCH_FORWARD_ERROR}")
if(ESPNOW_PATCH_FORWARD_LOG MATCHES "[Ss]kipped patch")
    message(FATAL_ERROR "Git skipped the ESP-NOW runtime-safety patch instead of checking it")
endif()
if(ESPNOW_PATCH_FORWARD_CHECK EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${ESPNOW_PATCH_DIRECTORY_ARG}" "${ESPNOW_RUNTIME_PATCH}"
        WORKING_DIRECTORY "${ESPNOW_GIT_ROOT}"
        RESULT_VARIABLE ESPNOW_PATCH_APPLY_RESULT
        OUTPUT_VARIABLE ESPNOW_PATCH_APPLY_OUTPUT
        ERROR_VARIABLE ESPNOW_PATCH_APPLY_ERROR
    )
    if(NOT ESPNOW_PATCH_APPLY_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply ESP-NOW runtime-safety patch: ${ESPNOW_PATCH_APPLY_ERROR}")
    endif()
    message(STATUS "Applied ESP-NOW runtime-safety patch")
else()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${ESPNOW_PATCH_DIRECTORY_ARG}" "${ESPNOW_RUNTIME_PATCH}"
        WORKING_DIRECTORY "${ESPNOW_GIT_ROOT}"
        RESULT_VARIABLE ESPNOW_PATCH_REVERSE_CHECK
        OUTPUT_VARIABLE ESPNOW_PATCH_REVERSE_OUTPUT
        ERROR_VARIABLE ESPNOW_PATCH_REVERSE_ERROR
    )
    string(CONCAT ESPNOW_PATCH_REVERSE_LOG "${ESPNOW_PATCH_REVERSE_OUTPUT}" "${ESPNOW_PATCH_REVERSE_ERROR}")
    if(ESPNOW_PATCH_REVERSE_LOG MATCHES "[Ss]kipped patch")
        message(FATAL_ERROR "Git skipped the reverse ESP-NOW runtime-safety patch check")
    endif()
    if(NOT ESPNOW_PATCH_REVERSE_CHECK EQUAL 0)
        message(FATAL_ERROR
            "ESP-NOW sources match neither the clean nor patched state.\n"
            "Forward check: ${ESPNOW_PATCH_FORWARD_ERROR}\n"
            "Reverse check: ${ESPNOW_PATCH_REVERSE_ERROR}")
    endif()
    message(STATUS "ESP-NOW runtime-safety patch already applied")
endif()
