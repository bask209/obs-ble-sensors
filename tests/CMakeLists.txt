# Standalone unit tests for the platform-independent core (parser, state machine,
# registry, BLE manager with the mock backend). Needs no OBS, Qt or Bluetooth.
cmake_minimum_required(VERSION 3.20)
project(obs-ble-sensors-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/core")

add_executable(
  ble-sensors-tests
  test_main.cpp
  test_parser.cpp
  test_state.cpp
  test_manager.cpp
  ${CORE_DIR}/heart_rate_parser.cpp
  ${CORE_DIR}/sensor_state.cpp
  ${CORE_DIR}/display_format.cpp
  ${CORE_DIR}/device_identity.cpp
  ${CORE_DIR}/device_registry.cpp
  ${CORE_DIR}/ble_manager.cpp
  ${CORE_DIR}/mock_backend.cpp
)
target_include_directories(ble-sensors-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(ble-sensors-tests PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(ble-sensors-tests PRIVATE /W4 /WX /wd4127 /utf-8 /permissive-)
else()
  target_compile_options(ble-sensors-tests PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME ble-sensors-tests COMMAND ble-sensors-tests)

# Windows-only diagnostic probe for real hardware (uses the shipping WinRT backend).
if(WIN32)
  add_executable(
    ble-probe
    ble_probe.cpp
    ${CORE_DIR}/heart_rate_parser.cpp
    ${CORE_DIR}/sensor_state.cpp
    ${CORE_DIR}/device_identity.cpp
    ${CORE_DIR}/ble_manager.cpp
    ${CORE_DIR}/../windows/winrt_backend.cpp
  )
  target_include_directories(ble-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
  target_compile_definitions(ble-probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(ble-probe PRIVATE /W4 /WX /wd4127 /utf-8 /permissive-)
  target_link_libraries(ble-probe PRIVATE WindowsApp Threads::Threads)
endif()
