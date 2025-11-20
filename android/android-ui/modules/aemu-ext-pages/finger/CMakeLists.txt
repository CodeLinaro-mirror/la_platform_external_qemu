android_qt_lib(
  TARGET ext-page-finger
  SRC android/skin/qt/extended-pages/finger-page.cpp
      android/skin/qt/extended-pages/finger-page.ui
      android/skin/qt/extended-pages/grpc-finger-controller.h
      android/skin/qt/extended-pages/legacy-finger-controller.h
  INCLUDES
  src
  objs/android/android-grpc/services/emulator-controller
  DEPS android-emu
       emulator_controller-service-client
       glib2
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)
