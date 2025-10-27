android_qt_lib(
  TARGET ext-page-battery
  SRC android/skin/qt/extended-pages/battery-page.cpp
      android/skin/qt/extended-pages/battery-page.ui
      android/skin/qt/extended-pages/grpc-battery-controller.cpp
      android/skin/qt/extended-pages/legacy-battery-controller.cpp
  INCLUDES
  src
  DEPS android-emu
       emulator_controller-service-client
       glib2
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)

android_add_test(
  TARGET ext-page-battery-tests
  SOURCE_DIR
  src
  SRC android/skin/qt/extended-pages/battery-page_test.cpp
      android/skin/qt/extended-pages/grpc-battery-controller_test.cpp
      android/skin/qt/extended-pages/legacy-battery-controller_test.cpp
  DEPS android-emu
       android-emu-test-launcher
       emulator_controller-service-client
       ext-page-battery
       gmock
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)
