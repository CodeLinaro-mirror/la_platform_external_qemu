# AEMU Qt UI Expert Architecture

## Multi-threading Model

The Android Emulator UI operates in a multi-threaded environment where the **Qt Main Thread** (GUI thread) and the **Emulator Thread** (QEMU/Guest thread) must interact safely.

### Cross-thread Communication

To call from the Emulator thread to the Qt thread:
1. **Signals and Slots**: Use Qt's signal/slot mechanism. Emit a signal from the emulator thread; the corresponding slot will execute on the Qt thread.
2. **Blocking Calls**: If the emulator thread needs to wait for the UI operation to complete, pass a `QSemaphore*` to the signal. The slot should release the semaphore upon completion.

```cpp
// In EmulatorQtWindow.h
signals:
    void getDevicePixelRatio(double* out_dpr, QSemaphore* semaphore = NULL);

// In winsys-qt.cpp
if (onMainQtThread()) {
    window->getDevicePixelRatio(dpr, nullptr);
} else {
    QSemaphore semaphore;
    window->getDevicePixelRatio(dpr, &semaphore);
    semaphore.acquire();
}
```

## Core Components

### 1. `EmulatorQtWindow`
The primary window class. It handles:
- Rendering the guest screen via `SharedMemoryRenderer`.
- Input event translation (keyboard, mouse, touch, pen).
- Window management (resize, zoom, rotation).
- Drag and drop (APK installation, file pushing).

### 2. `ExtendedWindow`
Manages the "Extended Controls" dialog. It uses a sidebar-based navigation to switch between different control pages.

### 3. `ToolWindow`
The vertical toolbar next to the emulator screen, providing quick access to common actions (power, volume, rotation, etc.).

### 4. `MultiDisplayWidget`
Handles additional displays for the emulator, allowing for multi-monitor guest configurations.

## UI Design and Resources

### Qt Designer (`.ui` files)
Layouts are primarily defined in `.ui` files. These are compiled into C++ code at build time.
- `extended.ui`: Layout for the extended controls container.
- `tools.ui`: Layout for the vertical toolbar.

### Qt Resources (`.qrc` files)
Images, fonts, and stylesheets are bundled into the executable via `.qrc` files (e.g., `resources.qrc`).

## Interaction with Backend

The UI interacts with the emulator backend through **Agents**. Key agents include:
- `UiEmuAgent`: General emulator control.
- `AdbInterface`: Interaction with the guest via ADB.
- `MultiDisplayAgent`: Management of guest displays.

## Testing Pattern

Extended control pages typically follow a "Controller" pattern to facilitate testing:
- **Page**: The UI class (e.g., `BatteryPage`).
- **Controller**: An interface defining the backend operations (e.g., `BatteryController`).
- **Implementations**: A "Legacy" controller using agents or a "gRPC" controller.
- **Mocks**: Used in unit tests to verify UI interactions without a full emulator backend.

Example Test:
```cpp
TEST_F(BatteryPageTest, SliderUpdatesChargeLevel) {
    auto mockController = std::make_unique<MockBatteryController>();
    EXPECT_CALL(*mockController, setChargeLevel(80));
    m_page->setControllerForTest(std::move(mockController));
    // Simulate UI slider move to 80...
}
```
