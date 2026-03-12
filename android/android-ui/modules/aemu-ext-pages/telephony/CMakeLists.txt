android_qt_lib(
  TARGET ext-page-telephony
  INCLUDES src
  SRC android/skin/qt/extended-pages/telephony-page.cpp
      android/skin/qt/extended-pages/telephony-page.ui
      android/skin/qt/extended-pages/telephony-controller.cpp
      android/skin/qt/extended-pages/legacy-telephony-controller.cpp
  DEPS android-emu qemu-host-common-headers Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets ui::gl-qt-bridge ui::widgets)

if(ENABLE_QT_TESTS)
  target_compile_definitions(ext-page-telephony PRIVATE ENABLE_QT_TESTS)
endif()

android_add_test(
  TARGET ext-page-telephony-tests
  SOURCE_DIR src
  SRC android/skin/qt/extended-pages/legacy-telephony-controller_test.cpp
      android/skin/qt/extended-pages/telephony-controller_test.cpp
  DEPS android-emu
       android-emu-test-launcher
       ext-page-telephony
       gmock
       qemu-host-common-headers
       Qt${QT_VERSION_MAJOR}::Core
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::gl-qt-bridge
       ui::widgets)

if(ENABLE_QT_TESTS)
  target_compile_definitions(ext-page-telephony-tests PRIVATE ENABLE_QT_TESTS)
endif()
