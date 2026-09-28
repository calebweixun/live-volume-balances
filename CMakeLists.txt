cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for the live monitor dock" ON)
option(ENABLE_QT "Use Qt functionality for the live monitor dock" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
if(NOT WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE m)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "Live Volume Balancer requires ENABLE_FRONTEND_API and ENABLE_QT for its live monitor dock.")
endif()

find_package(Threads REQUIRED)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/plugin-main.c src/leveler-filter.c src/leveler-dsp.c src/monitor-dock.cpp
)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE c_std_11)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
if(NOT WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Threads::Threads)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

include(CTest)
if(BUILD_TESTING)
  add_executable(leveler-dsp-test tests/leveler-dsp-test.c src/leveler-dsp.c)
  target_include_directories(leveler-dsp-test PRIVATE src)
  target_compile_features(leveler-dsp-test PRIVATE c_std_11)
  if(NOT WIN32)
    target_link_libraries(leveler-dsp-test PRIVATE m)
  endif()
  add_test(NAME leveler-dsp COMMAND leveler-dsp-test)
endif()
