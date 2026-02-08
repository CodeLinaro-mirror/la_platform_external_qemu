# Sensors Agent Implementation Notes

## Goal
Implement the `QAndroidSensorsAgent` in Fishtank by forwarding calls to the emulator backend via gRPC.

## Progress
- [x] Implement `getPhysicalParameterSize`
- [x] Implement `getPhysicalParameter`
- [x] Implement `setPhysicalParameterTarget`
- [ ] Implement `getSensor`
- [ ] Implement `setSensorOverride`

## Current Tasks
- Implement `getSensor` and `setSensorOverride` (mapping to `getSensor` and `setSensor` RPCs).
- Handle `onTargetStateChanged` and other callbacks if possible (might require `streamPhysicalModel`).

## Debugging
Optional debug logs have been added to the implemented functions. They can be enabled by setting `#define DEBUG 1` (or higher) in `fishtank_agents.h`.
