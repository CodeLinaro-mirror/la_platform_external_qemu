android_qt_lib(
  TARGET aemu-ui-window
  # SHARED
  ALIAS ui::window
  SRC android/emulator-environment.cpp android/emulator-window.c android/main-common-ui.c android/resource.c
      android/skin/ui.c android/skin/user-config.c android/skin/window.c
  DEPS aemu-gl-init
       aemu-gl-init
       aemu-recording
       android-emu-agents
       android-emu-avd
       android-emu-base
       android-emu-base-headers
       android-emu-cmdline
       android-emu-files
       android-emu-metrics
       android-emu-sockets
       android-emu-utils
       virtual_environment_renderer
       qemu-host-common-headers
       png
       gfxstream_glm_headers
       webrtc-yuv
       ui::common)

target_compile_definitions(aemu-ui-window PRIVATE "-D_LIBCPP_VERSION=__GLIBCPP__")

target_include_directories(
  aemu-ui-window
  PRIVATE
    src
    # Fix this dependency once those libs have been sliced.
    ${ANDROID_QEMU2_TOP_DIR}/android/android-emu
    # use ui::gl-bridge once the cycles have been broken.
    # ../aemu-gl-bridge/include
    # TODO(jansene): We actually have a hard dependency on qemu-glue as there
    # are a lot of externs that are actually defined in qemu2-glue. this has to
    # be sorted out,
    ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/config/${ANDROID_TARGET_TAG}
  PUBLIC include)
