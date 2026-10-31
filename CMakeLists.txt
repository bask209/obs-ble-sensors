cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

if(NOT WIN32)
  message(FATAL_ERROR "OBS BLE Sensors uses the Windows Bluetooth LE APIs and builds only on Windows.")
endif()

# Development only: compiles the simulated BLE backend, activated at runtime with the
# environment variable OBS_BLE_SENSORS_MOCK=1. Never enable for release packages.
option(OBS_BLE_SENSORS_DEV_MOCK "Build the simulated BLE backend (development only)" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

# C++/WinRT ships with the Windows SDK; WindowsApp.lib provides the WinRT entry points.
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE WindowsApp)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/core/heart_rate_parser.cpp
    src/core/heart_rate_parser.hpp
    src/core/sensor_state.cpp
    src/core/sensor_state.hpp
    src/core/display_format.cpp
    src/core/display_format.hpp
    src/core/device_identity.cpp
    src/core/device_identity.hpp
    src/core/device_registry.cpp
    src/core/device_registry.hpp
    src/core/backoff.hpp
    src/core/ble_backend.hpp
    src/core/ble_manager.cpp
    src/core/ble_manager.hpp
    src/windows/winrt_backend.cpp
    src/windows/winrt_backend.hpp
    src/obs/plugin_context.cpp
    src/obs/plugin_context.hpp
    src/obs/registry_store.cpp
    src/obs/registry_store.hpp
    src/obs/heart_rate_source.cpp
    src/obs/heart_rate_source.hpp
    src/ui/sensor_dialog.cpp
    src/ui/sensor_dialog.hpp
    src/ui/sensor_dialog_p.hpp
)

if(OBS_BLE_SENSORS_DEV_MOCK)
  message(WARNING "OBS_BLE_SENSORS_DEV_MOCK is ON: this build contains the simulated BLE backend. Do not ship it.")
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OBS_BLE_SENSORS_DEV_MOCK)
  target_sources(
    ${CMAKE_PROJECT_NAME}
    PRIVATE src/core/mock_backend.cpp src/core/mock_backend.hpp src/dev/dev_simulator.cpp src/dev/dev_simulator.hpp
  )
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
