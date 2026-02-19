---
name: aemu-ui-qt-expert
description: Expert in the Android Emulator standalone UI (Qt-based). Use when modifying, debugging, or extending the emulator's host-side user interface, including the main window, extended controls, toolbars, and multi-display support.
---

# AEMU Qt UI Expert

Expert guidance for working with the Android Emulator standalone UI, written in C++ using the Qt framework.

## Core Procedures

### Architectural Navigation
- **Main Window**: `EmulatorQtWindow` in `src/android/skin/qt/emulator-qt-window.cpp`.
- **Extended Controls**: `ExtendedWindow` in `src/android/skin/qt/extended-window.cpp`.
- **Toolbar**: `ToolWindow` in `src/android/skin/qt/tool-window.cpp`.
- **Entry Point**: `winsys-qt.cpp` implements the `winsys.h` interface.

### Implementing Thread-Safe UI Calls
When calling UI code from the emulator thread, you MUST use Qt signals and slots to ensure the code executes on the Qt Main Thread.

1.  **Define Signal**: Add a signal to the class header.
2.  **Add Slot**: Add a corresponding private slot that performs the actual work.
3.  **Cross-Thread Execution**:
    - For async calls: `emit mySignal(args);`
    - For sync (blocking) calls: Pass a `QSemaphore*`, emit the signal, and call `semaphore->acquire()`.

### UI Development
- **Layouts**: Prefer editing `.ui` files with Qt Designer.
- **Resources**: Add icons and assets to `resources.qrc` or `static_resources.qrc`.
- **Styling**: Stylesheets are used for branding and dark mode support; see `stylesheet.cpp`.

### Backend Interaction
Interact with the emulator exclusively via agents.
- **Agent Abstraction Mandate**: To support the ongoing transition to a decoupled architecture (Fishtank), UI code must NEVER access `qemulator->ui` or global backend state directly.
- **The Bridge Pattern**: Agents (via `getConsoleAgents()`) are the mandatory abstraction layer. The UI should call an Agent method; whether that Agent then performs a direct C call (legacy mode) or a gRPC call (Fishtank mode) is an implementation detail hidden from the UI.
- **Agent Stub Audit**: When functionality is missing or broken in decoupled modes, always audit the agent implementations (e.g., `fishtank_window_agent.cpp`) for incomplete `TODO` stubs or commented-out code.

### Fishtank UI (Standalone Mode)
Fishtank is a specialized, standalone version of the emulator UI (`android/android-ui/apps/fishtank`) in a transitional decoupled state.
- **Goal**: Move all functionality to a decoupled gRPC interface with Agents serving as the abstraction layer.
- **Event Loop**: Uses `receivePhysicalStateEvents` (gRPC stream) for real-time physical model updates (Position, Rotation, Hinge), replacing legacy polling.
- **Workflow**: Strictly follows TDD and atomic commits.
    - **TDD**: Write unit tests for all logic changes (see `*_unittest.cpp`).
    - **Documentation**: Update `DESIGN.md`, `SENSORS_AGENT.md`, and `WORKFLOW.md` after every task.
- **Agents**: Implements its own specialized agents (e.g., `sFishtankQAndroidSensorsAgent`) that proxy calls to the gRPC backend.

### Testing and Verification
- **Unit Tests**: Found in `test/` or alongside source as `*_unittest.cpp`.
- **Note on Unit Testing**: Currently, the Qt UI lacks comprehensive unit tests. A large refactor is required to decouple components enough to allow for effective testing.
- **Page/Controller Pattern**: For new pages in Extended Controls, use the Page/Controller pattern to allow mocking the backend in tests.
- **Visual Verification**: Always verify changes visually, as many UI issues (layout, clipping, focus) are not caught by unit tests.

## Common Tasks

### Adding a new page to Extended Controls
1. Create a `.ui` file for the layout.
2. Create a page class inheriting from `QWidget`.
3. Create a controller interface and mock for testing.
4. Register the new page in `ExtendedWindow::adjustTabs`.

### Debugging UI Events
- Trace events in `EmulatorQtWindow::event` or specific handlers like `keyPressEvent`.
- Use `QtLogger` for UI-side logging.

## References
- See [architecture.md](references/architecture.md) for detailed multi-threading and component details.
