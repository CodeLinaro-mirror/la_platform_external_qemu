android_qt_lib(
  TARGET ext-page-keyboard
  INCLUDES src
  SRC android/skin/qt/extended-pages/keyboard-page.cpp
      android/skin/qt/extended-pages/keyboard-page.ui
  DEPS aemu-ui-qt-base
       android-emu
       Qt${QT_VERSION_MAJOR}::Widgets
       ui::widgets
)
