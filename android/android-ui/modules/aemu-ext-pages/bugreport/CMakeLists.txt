android_qt_lib(
  TARGET ext-page-bugreport LICENSE "Apache-2.0"
  SRC android/skin/qt/extended-pages/bug-report-page.cpp
      android/skin/qt/extended-pages/bug-report-page.ui
      android/skin/qt/extended-pages/bugreport-controller.h
      android/skin/qt/extended-pages/grpc-bugreport-controller.cpp
      android/skin/qt/extended-pages/grpc-bugreport-controller.h
      android/skin/qt/extended-pages/legacy-bugreport-controller.cpp
      android/skin/qt/extended-pages/legacy-bugreport-controller.h
  DEPS android-emu
       emulator_controller-service-client
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::common
       ui::gl-qt-bridge
       ui::widgets)

target_include_directories(
  ext-page-bugreport
  PUBLIC src ${ANDROID_QEMU2_TOP_DIR}
         ${ANDROID_QEMU2_TOP_DIR}/android/android-grpc/client/include)
