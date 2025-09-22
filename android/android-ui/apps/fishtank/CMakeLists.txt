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
      main.cpp)

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
          android-grpc-client
          android-grpc-interceptors
          emulator_controller-service-client
          android-emu-curl
          android-hw-config
          ext-page-battery
          ext-page-battery-grpc
          ext-page-bugreport
          ext-page-bugreport-grpc
          ext-page-camera-grpc
          ext-page-car
          ext-page-cellular-grpc
          ext-page-display-grpc
          ext-page-dpad
          ext-page-finger-grpc
          ext-page-google-play
          ext-page-help
          ext-page-location-ui
          ext-page-microphone
          ext-page-recording
          ext-page-rotary
          ext-page-sensor-replay
          ext-page-settings
          ext-page-snapshot-grpc
          ext-page-telephony-grpc
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
