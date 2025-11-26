android_qt_lib(
  TARGET ext-page-cellular
  INCLUDES
  src
  SRC android/skin/qt/extended-pages/cellular-page.cpp
      android/skin/qt/extended-pages/cellular-page.ui
      android/skin/qt/extended-pages/grpc-cellular-controller.cpp
      android/skin/qt/extended-pages/legacy-cellular-controller.cpp
  DEPS android-emu
       glib2
       modem-service-client
       protobuf::libprotobuf
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)

android_add_test(
  TARGET ext-page-cellular-tests
  SOURCE_DIR
  src
  SRC android/skin/qt/extended-pages/cellular-page_test.cpp
      android/skin/qt/extended-pages/grpc-cellular-controller_test.cpp
      android/skin/qt/extended-pages/legacy-cellular-controller_test.cpp
  DEPS android-emu
       android-emu-test-launcher
       ext-page-cellular
       gmock
       modem-service-client
       protobuf::libprotobuf
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)
