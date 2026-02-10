# Sensors Agent Implementation Notes

## Goal
Implement the `QAndroidSensorsAgent` in Fishtank by forwarding calls to the emulator backend via gRPC.

## Understanding `QAndroidPhysicalStateAgent`

The `QAndroidPhysicalStateAgent` is a callback interface used by the emulator's Physical Model (which simulates the movement of the device, hinges, etc.) to notify the UI about state changes. This is a critical mechanism for coordinating smooth UI updates with the underlying simulation.

It consists of three main callbacks:

-   **`onTargetStateChanged`**: Called when a new "target" value is set (e.g., the user moved a slider or an external gRPC client updated the target).
-   **`onPhysicalStateChanging`**: Called when the physical model starts an interpolation towards a target. The UI (like `VirtualSensorsPage`) uses this signal to start high-frequency timers (typically 33ms / 30 FPS) to poll the model's *current* state and provide smooth visual animations.
-   **`onPhysicalStateStabilized`**: Called when the physical model has reached its target and the values are no longer changing. The UI uses this signal to stop its high-frequency timers, saving CPU and power.

### The Fishtank Challenge
In a standard integrated emulator, these are triggered directly by the C++ `PhysicalModel` class. In Fishtank, we are decoupled. If another gRPC client updates the physical model on the backend, Fishtank needs a way to detect this "movement" to trigger these local callbacks, ensuring the Fishtank UI stays in sync and animates correctly.

## Progress
- [x] Implement `getPhysicalParameterSize`
- [x] Implement `getPhysicalParameter`
- [x] Implement `setPhysicalParameterTarget`
- [x] Implement `getSensor`
- [x] Implement `getSensorSize`
- [x] Implement `setCoarseOrientation`
- [ ] Implement `setSensorOverride`
- [x] Implement `setPhysicalStateAgent` (with polling)

## Implementation Details

### Physical Parameters and Sensors
Most calls are straightforwardly forwarded to the corresponding `getSensor`, `setSensor`, `getPhysicalModel`, or `setPhysicalModel` gRPC RPCs. 

### Coarse Orientation
`setCoarseOrientation` is implemented by converting the coarse-grained orientation (Portrait, Landscape, etc.) into a 3D rotation vector (on the Z-axis) and forwarding it as a `ROTATION` physical parameter update via gRPC. This matches the behavior of the standard emulator.

### Polling for Physical State
Since `streamPhysicalModel` is not available in the backend, Fishtank implements a periodic polling mechanism (in `sensors_agent.cpp`) that triggers when a `QAndroidPhysicalStateAgent` is registered.

1.  **Background Thread**: A dedicated thread polls the backend's POSITION and ROTATION parameters every 100ms (an arbitrary choice that can be adjusted).
2.  **Change Detection**: If the values change significantly from the last poll, the thread triggers `onPhysicalStateChanging` (if not already moving) and `onTargetStateChanged`.
3.  **Stabilization**: When the values stop changing, it triggers `onPhysicalStateStabilized`.

This ensures that the Fishtank UI correctly animates even when changes are initiated by other gRPC clients.

## Debugging
Optional debug logs have been added to the implemented functions. They can be enabled by setting `#define DEBUG 1` (or higher) in `fishtank_agents.h`.
