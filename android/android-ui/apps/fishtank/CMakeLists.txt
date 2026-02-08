# Define the executable and its source files
android_add_executable(
  TARGET fishtank
  LICENSE "Apache-2.0"
  SRC agents/automation_agent.cpp
      agents/battery_agent.cpp
      agents/car_data_agent.cpp
      agents/cellular_agent.cpp
      agents/clipboard_agent.cpp
      agents/display_agent.cpp
      agents/finger_agent.cpp
      agents/global_vars_agent.cpp
      agents/grpc_agent.cpp
      agents/http_proxy_agent.cpp
      agents/hw_control_agent.cpp
      agents/libui_agent.cpp
      agents/location_agent.cpp
      agents/multi_display_agent.cpp
      agents/net_agent.cpp
      agents/record_screen_agent.cpp
      agents/sensors_agent.cpp
      agents/telephony_agent.cpp
      agents/user_event_agent.cpp
      agents/virtual_scene_agent.cpp
      agents/vm_operations.cpp
      fishtank_agents.cpp
      fishtank_window_agent.cpp
      grpc/FishtankGrpcServer.cpp
      main.cpp)

if(APPLE)
  # Override the default RPATH to match the standalone distribution layout
  android_target_properties(fishtank all "INSTALL_RPATH=@executable_path/lib64")
  android_target_properties(
    fishtank all "INSTALL_RPATH>=@executable_path/lib64/qt/lib")
  android_target_properties(fishtank all "INSTALL_RPATH>=@loader_path")
  android_target_properties(fishtank all "INSTALL_RPATH>=@loader_path/lib64")
  android_target_properties(fishtank all "BUILD_WITH_INSTALL_RPATH=ON")
  android_target_properties(fishtank all "INSTALL_RPATH_USE_LINK_PATH=ON")
endif()

protobuf_generate_grpc_cpp(
  SOURCE_DIR ${AEMU_PROTO_DIR}
  SOURCES services/forwarder/service_forwarder.proto
  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
  INCLUDES ${AEMU_PROTO_DIR}/client ${AEMU_PROTO_DIR}/services/forwarder
  GENERATED FORWARDER_PROTO_SRC)

android_add_library(
  TARGET service_forwarder_client LICENSE Apache-2.0 SRC ${FORWARDER_PROTO_SRC}
  DEPS grpc++ protobuf::libprotobuf)

target_include_directories(
  service_forwarder_client PRIVATE ${PROTOBUF_INCLUDE_DIR}
  PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(service_forwarder_client PRIVATE ${PROTOBUF_LIBRARIES}
                      PUBLIC android-grpc-endpoint)

# Link against the necessary libraries
target_link_libraries(
  fishtank
  PRIVATE aemu-ui-qt
          aemu-ui-qt-base
          android-emu
          qemu-host-common-headers
          Qt${QT_VERSION_MAJOR}::Core
          Qt${QT_VERSION_MAJOR}::Svg
          ui::gl-qt-bridge
          aemu-ui-car-property-utils
          aemu-ui-headless # Yes! The QT ui depends on the headless one..
          aemu-ui-qt-base
          aemu::recording
          android-emu
          android-emu-base-process
          android-grpc-client
          android-grpc-interceptors
          android-grpc-services-stack
          android-grpc-certificate-factory
          service_forwarder_client
          emulator_controller-service-client
          android-emu-curl
          android-hw-config
          ext-page-battery
          ext-page-bugreport
          ext-page-car
          ext-page-dpad
          ext-page-google-play
          ext-page-help
          ext-page-location-ui
          ext-page-microphone
          ext-page-recording
          ext-page-rotary
          ext-page-sensor-replay
          ext-page-settings
          ext-page-tv-remote
          FFMPEG::FFMPEG
          protobuf::libprotobuf
          qemu-host-common-headers
          Qt${QT_VERSION_MAJOR}::Core
          Qt${QT_VERSION_MAJOR}::Gui
          Qt${QT_VERSION_MAJOR}::Svg
          ui::common
          ui::gl-qt-bridge
          ui::widgets
          webrtc-yuv
          zlib
          ui::widgets)

# Include the current directory for headers
target_include_directories(fishtank PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

prebuilt(QT5)
android_target_properties(fishtank all "${QT5_SHARED_PROPERTIES}")

# Helper function to install dependencies with platform-specific destination logic.
#
# This function handles the distribution of various dependency types to the standalone
# fishtank directory. It supports:
#   1. CMake Targets: Shared libraries are installed to lib64/ on Unix or the root on Windows.
#   2. Mapping Strings: Strings in the format "source/path>dest/rel/path" are parsed
#      and installed. On Windows, the destination is flattened to the root.
#
# Arguments:
#   DEPS: A list of CMake targets or "src>dst" mapping strings.
function(fishtank_install_dependency)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs DEPS)
  cmake_parse_arguments(INST "${options}" "${oneValueArgs}" "${multiValueArgs}"
                        ${ARGN})

  foreach(dep ${INST_DEPS})
    if(dep MATCHES ".*>.*")
      # Handle "src>dst" mapping
      string(REPLACE ">" ";" SRC_DST ${dep})
      list(GET SRC_DST 0 SRC)
      list(GET SRC_DST 1 DST)
      get_filename_component(DST_DIR ${DST} DIRECTORY)
      get_filename_component(DST_NAME ${DST} NAME)
      get_filename_component(REAL_SRC ${SRC} REALPATH)
      if(WINDOWS_MSVC_X86_64)
        install(
          FILES ${REAL_SRC}
          DESTINATION fishtank
          RENAME ${DST_NAME}
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      else()
        install(
          FILES ${REAL_SRC}
          DESTINATION fishtank/${DST_DIR}
          RENAME ${DST_NAME}
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
        # Extra logic for libc++ on Linux to support QtWebEngineProcess.
        if(LINUX_X86_64 AND (DST_NAME STREQUAL "libc++.so.1"
                             OR DST_NAME STREQUAL "libc++.so"))
          install(
            FILES ${REAL_SRC}
            DESTINATION fishtank/lib64/qt/lib
            RENAME ${DST_NAME}
            COMPONENT fishtank
            EXCLUDE_FROM_ALL)
        endif()
      endif()
    elseif(TARGET ${dep})
      # Handle CMake targets
      if(WINDOWS_MSVC_X86_64)
        install(
          FILES $<TARGET_FILE:${dep}>
          DESTINATION fishtank
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      else()
        install(
          TARGETS ${dep}
          RUNTIME DESTINATION fishtank/lib64
          LIBRARY DESTINATION fishtank/lib64
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      endif()
    endif()
  endforeach()
endfunction()

# We use EXCLUDE_FROM_ALL on these install rules to ensure that fishtank and its
# dependencies are NOT included in the default emulator distribution.
# This allows fishtank to be deployed exclusively to its isolated standalone
# directory (objs/distribution-fishtank) during the specialized build phase.
install(
  TARGETS fishtank
  RUNTIME DESTINATION fishtank
  COMPONENT fishtank
  EXCLUDE_FROM_ALL)

if(NOT LINUX_AARCH64)
  fishtank_install_dependency(DEPS
                              emugl_common
                              android-emu-tracing
                              android-emu-metrics
                              android-emu-curl
                              android-emu-agents
                              android-emu-protos
                              android-emu-base-logging
                              glib2_${ANDROID_TARGET_TAG}
                              libprotobuf
                              abseil_dll
                              gfxstream_backend
                              ${RUNTIME_OS_DEPENDENCIES}
                              ${ANGLE_DEPENDENCIES}
                              ${SWIFTSHADER_DEPENDENCIES})

  if(WINDOWS_MSVC_X86_64)
    # Include MSVC runtime dependencies
    prebuilt(MSVC)
    foreach(dep ${MSVC_DEPENDENCIES})
      string(REPLACE ">" ";" SRC_DST ${dep})
      list(GET SRC_DST 0 SRC)
      list(GET SRC_DST 1 DST)
      get_filename_component(DST_NAME ${DST} NAME)
      install(
        FILES ${SRC}
        DESTINATION fishtank
        RENAME ${DST_NAME}
        COMPONENT fishtank
        EXCLUDE_FROM_ALL)
    endforeach()
  endif()
endif()

# For Qt dependencies, the only special thing we do in here is, for windows, flatten all dlls in
# qt/lib to the root. Everything else is installed as described in emu-qt5-config.cmake.
foreach(qt_dep ${QT5_SHARED_DEPENDENCIES} ${QT5_WEBENGINE_SHARED_DEPENDENCIES})
  if(qt_dep MATCHES ".*>>.*")
    string(REPLACE ">>" ";" SRC_DST ${qt_dep})
    list(GET SRC_DST 0 SRC)
    list(GET SRC_DST 1 DST)
    if(SRC MATCHES ".*\\*.*")
      file(GLOB GLOBBED_FILES ${SRC})
      install(
        FILES ${GLOBBED_FILES}
        DESTINATION fishtank/${DST}
        COMPONENT fishtank
        EXCLUDE_FROM_ALL)
    else()
      install(
        DIRECTORY ${SRC}
        DESTINATION fishtank/${DST}
        COMPONENT fishtank
        EXCLUDE_FROM_ALL)
    endif()
  else()
    string(REPLACE ">" ";" SRC_DST ${qt_dep})
    list(GET SRC_DST 0 SRC)
    list(GET SRC_DST 1 DST)
    get_filename_component(DST_DIR ${DST} DIRECTORY)
    if(WINDOWS_MSVC_X86_64)
      # On Windows, the DLLs must be next to the executable.
      # Plugins and resources can stay in their subdirectories,
      # but core DLLs like Qt6Core must be findable by the OS loader.
      # We can identify core DLLs because they are installed in lib64/qt/lib.
      if(SRC MATCHES ".*\\.dll$" AND DST MATCHES "lib64/qt/lib/.*")
        install(
          FILES ${SRC}
          DESTINATION fishtank
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      else()
        install(
          FILES ${SRC}
          DESTINATION fishtank/${DST_DIR}
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      endif()
    else()
      # On Linux/MacOS, the Qt installation rules defined in emu-qt5-config.cmake don't need any
      # modification.
      if(SRC MATCHES "QtWebEngineProcess$")
        install(
          PROGRAMS ${SRC}
          DESTINATION fishtank/${DST_DIR}
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      else()
        install(
          FILES ${SRC}
          DESTINATION fishtank/${DST_DIR}
          COMPONENT fishtank
          EXCLUDE_FROM_ALL)
      endif()
    endif()
  endif()
endforeach()

android_add_test(
  TARGET fishtank_unittests
  SRC grpc/FishtankGrpcServer_unittest.cpp
      grpc/FishtankGrpcServer.cpp
      agents/sensors_agent_unittest.cpp
      agents/sensors_agent.cpp)

target_link_libraries(
  fishtank_unittests
  PRIVATE android-emu
          android-grpc-client
          android-grpc-services-stack
          android-grpc-certificate-factory
          service_forwarder_client
          emulator_controller-service-client
          gmock
          android-emu-test-launcher)

target_include_directories(fishtank_unittests
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})