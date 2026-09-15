android_qt_lib(
  TARGET ext-page-help LICENSE "Apache-2.0"
  SRC android/skin/qt/extended-pages/help-controller.h
      android/skin/qt/extended-pages/grpc-help-controller.cpp
      android/skin/qt/extended-pages/grpc-help-controller.h
      android/skin/qt/extended-pages/legacy-help-controller.cpp
      android/skin/qt/extended-pages/legacy-help-controller.h
      android/skin/qt/extended-pages/help-page.cpp
      android/skin/qt/extended-pages/help-page.ui
  DEPS aemu-ui-qt-base
       android-emu
       emulator_controller-service-client
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::common
       ui::gl-qt-bridge
       ui::widgets)

target_include_directories(
  ext-page-help
  PUBLIC src ${ANDROID_QEMU2_TOP_DIR}
         ${ANDROID_QEMU2_TOP_DIR}/android/android-grpc/client/include)

