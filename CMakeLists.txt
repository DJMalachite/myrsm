cmake_minimum_required(VERSION 3.16)

include(CPackComponent)

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
add_definitions(-DVERSION=\"${VERSION}\")
add_definitions(-DPROJECT_VERSION_NAME=\"${PROJECT_VERSION_NAME}\")
message("Building ${VERSION} ${PROJECT_VERSION_NAME}")
set(COMMENT "${PROJECT_VERSION_NAME}")

# Allow for a nice long message on all qDebug
add_definitions(-DQT_MESSAGELOGCONTEXT)

set(ORIGINALICONPNG ${CMAKE_CURRENT_SOURCE_DIR}/logo-sm.png)
set(ICONICO ${CMAKE_CURRENT_BINARY_DIR}/icon.ico)
set(RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/myrsm.rc)
add_custom_command(
    OUTPUT ${ICONICO}
    COMMAND magick ${ORIGINALICONPNG} -define
            icon:auto-resize=16,24,32,48,64,72,96,128,256 ${ICONICO}
    DEPENDS ${ORIGINALICONPNG}
    COMMENT "Generating ${ICONICO}")

set_source_files_properties(${RC_FILE} PROPERTIES OBJECT_DEPENDS ${ICONICO})

string(REPLACE "-" "_" CPACK_TAG "${PROJECT_NAME}_$ENV{CI_COMMIT_REF_SLUG}")

configure_file(myrsm.rc.in myrsm.rc @ONLY)

find_package(Qt6 REQUIRED COMPONENTS Core HttpServer Network WebSockets)

qt_add_executable(myrsm main.cpp MyRsm.cpp ${RC_FILE}) # Replace with your source files

target_link_libraries(myrsm PRIVATE Qt6::Core Qt6::HttpServer Qt6::Network Qt6::WebSockets)

set(PROJECT_INSTALLER_MAIN_COMP ${CPACK_TAG})
set(PROJECT_INSTALLER_LIB_COMP ${CPACK_TAG}_libs)

qt_generate_deploy_app_script(
  TARGET
  ${PROJECT_NAME}
  OUTPUT_SCRIPT
  deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR
)
install(
  TARGETS ${PROJECT_NAME}
  COMPONENT ${PROJECT_INSTALLER_MAIN_COMP}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          COMPONENT ${PROJECT_INSTALLER_MAIN_COMP}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT ${PROJECT_INSTALLER_MAIN_COMP})
install(SCRIPT ${deploy_script} COMPONENT ${PROJECT_INSTALLER_LIB_COMP})

function(copy_to VAR DEST)
  if(EXISTS ${VAR})
    message(NOTICE "> will install ${VAR} to ${DEST}")
    install(
      FILES ${VAR}
      DESTINATION ${DEST}
      COMPONENT ${PROJECT_INSTALLER_MAIN_COMP})
  else()
    message(NOTICE "> ${VAR} not found")
  endif()
endfunction()

copy_to(${Qt6_DIR}/../../../bin/libc++.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libunwind.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libstdc++-6.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libgcc_s_seh-1.dll ${CMAKE_INSTALL_BINDIR})
copy_to(${Qt6_DIR}/../../../bin/libwinpthread-1.dll ${CMAKE_INSTALL_BINDIR})

set(CPACK_GENERATOR "IFW")
set(CPACK_VERBATIM_VARIABLES YES)

set(CPACK_IFW_PACKAGE_NAME ${CPACK_TAG})
set(CPACK_IFW_PACKAGE_TITLE ${CPACK_TAG})
set(CPACK_IFW_PACKAGE_PUBLISHER "qdel")
set(CPACK_IFW_PACKAGE_ICON "${CMAKE_CURRENT_BINARY_DIR}/icon.ico")

set(CPACK_IFW_PACKAGE_CONTROL_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/control.qs")

cpack_add_component(
  ${PROJECT_INSTALLER_LIB_COMP}
  DISPLAY_NAME "MyRsm app libraries"
  REQUIRED
  DESCRIPTION "MyRsm app libraries")
cpack_add_component(
  ${PROJECT_INSTALLER_MAIN_COMP}
  DISPLAY_NAME "MyRsm App"
  REQUIRED
  DESCRIPTION "MyRsm application!"
  DEPENDS ${PROJECT_INSTALLER_LIB_COMP})

include(CPack)
include(CPackIFW)

cpack_ifw_configure_component(
  ${PROJECT_INSTALLER_MAIN_COMP})
