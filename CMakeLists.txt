cmake_minimum_required(VERSION 3.16)

function(split_ver VARNAME VALUE)
  set(MAJOR 0)
  set(MINOR 0)
  set(MINORMINOR ${VALUE})
  while(${MINORMINOR} GREATER_EQUAL 65535)
    math(EXPR MINORMINOR "${MINORMINOR} - 65535")
    math(EXPR MINOR "${MINOR} + 1")
  endwhile()
  while(${MINOR} GREATER_EQUAL 255)
    math(EXPR MINOR "${MINOR} - 255")
    math(EXPR MAJOR "${MAJOR} + 1")
  endwhile()
  set("${VARNAME}"
      "${MAJOR},${MINOR},${MINORMINOR}"
      PARENT_SCOPE)
endfunction()
if($ENV{CI_PIPELINE_ID})
  message(NOTICE "> Using version: $ENV{CI_PIPELINE_ID}")
  set(VERSION "$ENV{CI_PIPELINE_ID}")
else()
  message(NOTICE "> Using default version: 1")
  set(VERSION "1")
endif()
split_ver(VERSION_SPLIT ${VERSION})
if($ENV{CI_JOB_ID})
  message(NOTICE "> Using job version: $ENV{CI_JOB_ID}")
  set(VERSION_JOB "$ENV{CI_JOB_ID}")
else()
  message(NOTICE "> Using default version: 1")
  set(VERSION_JOB "1")
endif()
split_ver(VERSION_JOB_SPLIT ${VERSION_JOB})

project(myrsm VERSION ${VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if((DEFINED ENV{CI_JOB_ID}) AND (DEFINED ENV{CI_COMMIT_REF_SLUG}))
  set(PROJECT_VERSION_NAME "$ENV{CI_JOB_ID}-$ENV{CI_COMMIT_REF_SLUG}")
else()
  set(PROJECT_VERSION_NAME "localbuild")
endif()
if (DEFINED ENV{CI_COMMIT_REF_SLUG})
  set (BRANCH "$ENV{CI_COMMIT_REF_SLUG}")
else()
  set (BRANCH "localbuild")
endif()
add_definitions(-DVERSION=\"${VERSION}\")
add_definitions(-DBRANCH=\"${BRANCH}\")
add_definitions(-DPROJECT_VERSION_NAME=\"${PROJECT_VERSION_NAME}\")
# CI sets MYRSM_RELEASE=1 for main and v* tags: logs go to %ProgramData%\MyRsm\logs.
if("$ENV{MYRSM_RELEASE}" STREQUAL "1")
  add_definitions(-DMYRSM_RELEASE_BUILD)
endif()
message("Building ${VERSION} ${PROJECT_VERSION_NAME}")
set(COMMENT "${PROJECT_VERSION_NAME}")

# Allow for a nice long message on all qDebug
add_definitions(-DQT_MESSAGELOGCONTEXT)

set(ORIGINALICONPNG ${CMAKE_CURRENT_SOURCE_DIR}/logo-rsm.png)
set(ICONICO ${CMAKE_CURRENT_BINARY_DIR}/icon.ico)
set(RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/myrsm.rc)
add_custom_command(
    OUTPUT ${ICONICO}
    COMMAND magick ${ORIGINALICONPNG} -define
            icon:auto-resize=16,24,32,48,64,72,96,128,256 ${ICONICO}
    DEPENDS ${ORIGINALICONPNG}
    COMMENT "Generating ${ICONICO}")

set_source_files_properties(${RC_FILE} PROPERTIES OBJECT_DEPENDS ${ICONICO})

configure_file(myrsm.rc.in myrsm.rc @ONLY)

option(MYRSM_BUILD_APP "Build the myrsm executable (needs Qt HttpServer + WebSockets)" ON)
option(MYRSM_BUILD_TESTS "Build the unit tests and the fixture renderer" ON)

if(MSVC)
  # Sources contain UTF-8 text; don't depend on the build machine's code page.
  add_compile_options(/utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network)

# Shared-memory reader, Windows-free Prometheus exporter and config loading,
# shared by the app, the unit tests and the fixture renderer.
add_library(myrsm_core STATIC
  RsmTypes.hpp
  MyRsm.hpp
  MyRsm.cpp
  PromExporter.hpp
  PromExporter.cpp
  Config.hpp
  Config.cpp)
target_include_directories(myrsm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(myrsm_core PUBLIC Qt6::Core Qt6::Network)

if(MYRSM_BUILD_TESTS)
  enable_testing()
  find_package(Qt6 REQUIRED COMPONENTS Test)

  qt_add_executable(tst_promexporter tests/tst_promexporter.cpp)
  target_link_libraries(tst_promexporter PRIVATE myrsm_core Qt6::Test)
  add_test(NAME tst_promexporter COMMAND tst_promexporter)
  # Find the Qt DLLs when run from the build tree.
  set_tests_properties(tst_promexporter PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

  qt_add_executable(tst_config tests/tst_config.cpp)
  target_link_libraries(tst_config PRIVATE myrsm_core Qt6::Test)
  add_test(NAME tst_config COMMAND tst_config)
  set_tests_properties(tst_config PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

  # Renders a MyRSM "/" JSON fixture through prom::render (driven by
  # tools/run_parity.py against the reference connector).
  qt_add_executable(myrsm_render_fixture
    tests/render_fixture.cpp
    tests/FixtureLoader.hpp
    tests/FixtureLoader.cpp)
  target_link_libraries(myrsm_render_fixture PRIVATE myrsm_core)
endif()

if(NOT MYRSM_BUILD_APP)
  return()
endif()

find_package(Qt6 REQUIRED COMPONENTS HttpServer Network WebSockets)
include(GNUInstallDirs)

qt_add_executable(myrsm
  main.cpp
  ServiceControl.hpp
  ServiceControl.cpp
  logger.hpp
  logger.cpp
  ${RC_FILE})

target_link_libraries(myrsm PRIVATE myrsm_core Qt6::Core Qt6::HttpServer Qt6::Network Qt6::WebSockets
  advapi32 shell32)

# Portable layout used by the installer (installer/myrsm.iss) and the CI
# artifact: bin/myrsm.exe + Qt DLLs, plugins/, translations/.
qt_generate_deploy_app_script(
  TARGET
  ${PROJECT_NAME}
  OUTPUT_SCRIPT
  deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR
)
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(SCRIPT ${deploy_script})

function(copy_to VAR DEST)
  if(EXISTS ${VAR})
    message(NOTICE "> will install ${VAR} to ${DEST}")
    install(
      FILES ${VAR}
      DESTINATION ${DEST})
  else()
    message(NOTICE "> ${VAR} not found")
  endif()
endfunction()

# MinGW runtime DLLs, only present for MinGW Qt builds.
copy_to(${Qt6_DIR}/../../../bin/libc++.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libunwind.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libstdc++-6.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libgcc_s_seh-1.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libwinpthread-1.dll ${CMAKE_INSTALL_BINDIR})
