# Fishtank Design Document

Fishtank is a standalone Android Emulator UI that is decoupled from the main emulator backend. It leverages the existing Qt-based UI modules of the Android Emulator but redirects all hardware and emulator interactions over gRPC to a running emulator instance.

## Overview

Fishtank's primary goal is to provide a rich, native UI experience for the Android Emulator that can run on a different process (or even a different machine) than the emulator engine itself.

### Key Components

1.  **Main Application (`main.cpp`)**: Handles startup, command-line parsing, emulator discovery, and initialization of the Qt event loop and rendering engine.
2.  **Fishtank Agents (`fishtank_agents.cpp`, `agents/`)**: Re-implementations of the `QAndroid*Agent` interfaces. Instead of interacting with a local QEMU or Android state, these agents translate UI calls into gRPC requests sent to the emulator backend.
3.  **gRPC Client (`EmulatorControlClient`)**: A wrapper around the `EmulatorController` service defined in the emulator's gRPC API. It is used by the agents to send commands and receive state from the emulator.
4.  **gRPC Server (`FishtankGrpcServer`)**: A local gRPC server running within Fishtank that hosts the `UiController` service.
5.  **Service Forwarder**: Fishtank registers its local `UiController` service with the main emulator's `ServiceForwarder` service. This allows the emulator backend to make "callback" gRPC calls to Fishtank (e.g., to request the UI to show the extended controls window).
6.  **Guest Screen Streaming (`SharedStreamEmulator`)**: Uses the `streamScreenshot` gRPC call with `MMAP` (Shared Memory) transport for efficient, low-latency guest screen updates.

## Detailed Mechanisms

### Emulator Discovery and Connection

Fishtank can connect to an emulator in three ways:
-   `-fishtank default`: Automatically picks the first running emulator it finds.
-   `-fishtank <serial>`: Connects to the emulator with the specified serial port (e.g., 5554).
-   `-fishtank <file.ini>`: Uses a specific gRPC discovery file.

Fishtank now automatically configures the AVD name and necessary environment
variables (`ANDROID_EMULATOR_LAUNCHER_DIR`, `ANDROID_AVD_HOME`) from the
emulator's discovery file. This makes it possible to run fishtank using only the
serial number without needing `-avd` or manual environment setup.

Discovery is handled by the `EmulatorAdvertisement` class, which looks for `.ini` files in the emulator's runtime directory.

### Input Handling

All user input (keyboard, mouse, touch, etc.) is captured by the Qt UI and passed to the `QAndroidUserEventAgent` implementation in `agents/user_event_agent.cpp`. This implementation uses an asynchronous gRPC stream (`streamInput`) to forward events to the emulator.

### Guest Screen Rendering

Fishtank uses a zero-copy (or near zero-copy) mechanism for displaying the guest screen:
1.  Fishtank creates a temporary file to act as a shared memory handle.
2.  It calls `streamScreenshot` on the `EmulatorController` service, specifying `ImageTransport::MMAP` and providing the handle.
3.  The emulator backend maps this file and writes guest screen frames directly into it.
4.  The `SharedStreamEmulator` in Fishtank receives a notification over the gRPC stream whenever a new frame is ready.
5.  The `SharedMemoryRenderer` (a Qt `QObject`) is then triggered to update its internal `QImage` from the shared memory and signal the Qt window to repaint.

### UI Controller and Callbacks

The `UiController` service allows the emulator to control aspects of the UI. For example, when a user triggers an action in the guest OS that requires showing the emulator's "Extended Controls" window, the emulator uses the `UiController` service.

Since the emulator doesn't know Fishtank's address upfront, Fishtank uses the `ServiceForwarder` service:
1.  Fishtank starts its own gRPC server on a local port.
2.  It calls `ServiceForwarder.registerForwarder` on the emulator, providing its local address and the services it offers (`android.emulation.control.UiController`).
3.  The emulator backend then routes any `UiController` requests to Fishtank's local server.

### Physical State Callbacks

Many UI components (like the Virtual Sensors page) rely on asynchronous notifications to provide smooth updates. This is handled via the `QAndroidPhysicalStateAgent` interface, which provides callbacks for when a physical parameter starts changing (`onPhysicalStateChanging`) and when it stabilizes (`onPhysicalStateStabilized`).

Because some gRPC backends do not yet support streaming notifications for physical model changes, Fishtank may employ **polling** strategies to detect backend changes (e.g., from other gRPC clients) and manually trigger these local callbacks to ensure the UI remains responsive and synchronized.

### Rendering and SwiftShader

Fishtank is designed to be lightweight and compatible. It typically uses **SwiftShader** (a software GL implementation) for its own UI rendering to avoid dependencies on host GPU drivers, ensuring consistent behavior across different environments. This is configured in `main.cpp` by setting the library search paths and renderer configuration.

## Limitations and Future Work

### Audio Playback

Fishtank currently lacks a native host audio backend. In the standalone emulator, audio is handled by QEMU's internal audio system. Since Fishtank is decoupled from QEMU, `AudioOutputEngine::get()` returns `nullptr`.

The **Video Player** (used for recording playback) is designed to handle this gracefully by falling back to silent mode. To enable audio in Fishtank, a new implementation of `AudioOutputEngine` using a host-side library like **SDL2** or **Qt Audio** would need to be integrated into the Fishtank process.

### Agent Implementation Status

Fishtank is an ongoing project, and not all emulator agents are fully implemented. The following table summarizes the current status of the major agents:

| Agent | Status | Notes |
| :--- | :--- | :--- |
| **User Event** | ✅ Full | Key, Mouse, Touch, Wheel, and Rotary events are forwarded via gRPC stream. |
| **Location** | 🟡 Partial | `gpsSendLoc` and `gpsGetLoc` are implemented. NMEA and GNSS specific calls are currently `NOT_IMPLEMENTED`. |
| **Clipboard** | ✅ Full | Bi-directional clipboard sync is implemented using gRPC streaming and unary calls. |
| **Battery** | ❌ None | All calls are currently `NOT_IMPLEMENTED`. |
| **Sensors** | ✅ Full | All physical parameter and sensor calls are implemented and forwarded via gRPC. Includes a background polling mechanism for physical state changes. |
| **Cellular** | ❌ None | All calls are currently `NOT_IMPLEMENTED`. |
| **Display** | 🟡 Partial | Basic display information is available, but complex multi-display management is still being hooked up. |
| **VM Operations** | 🟡 Partial | Basic VM control (start/stop/pause) is partially implemented via gRPC. |

## Developer Notes

### Adding New Agent Features

To add a new feature that requires a new agent:
1.  Define the agent interface in `host-common/`.
2.  Provide a gRPC-based implementation in `android/android-ui/apps/fishtank/agents/`.
3.  Register the new agent in `FishtankAgentConsoleFactory` within `fishtank_agents.cpp`.
4.  Ensure the corresponding gRPC service method exists in `EmulatorController` or the relevant gRPC service.

### Debugging

-   Use `-verbose grpc` to see all gRPC traffic between Fishtank and the emulator.
-   Fishtank logs are typically sent to stderr and can be controlled using standard `AEMU_LOG` environment variables.
