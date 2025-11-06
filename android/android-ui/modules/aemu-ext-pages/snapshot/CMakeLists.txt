android_qt_lib(
  TARGET ext-page-snapshot
  INCLUDES
  src
  SRC android/skin/qt/extended-pages/grpc_snapshot_controller.cpp
      android/skin/qt/extended-pages/legacy_snapshot_controller.cpp
      android/skin/qt/extended-pages/snapshot-page.cpp
      android/skin/qt/extended-pages/snapshot-page.ui
  DEPS absl::statusor
       android-emu
       emulator_controller-service-client
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)
target_link_libraries(ext-page-snapshot PUBLIC snapshot-service-client)
