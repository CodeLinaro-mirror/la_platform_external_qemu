android_grpc_service_lib(
  TARGET emulated-bluetooth-service SOURCE_DIR
         ${AEMU_PROTO_DIR}/services/bluetooth
  SOURCES emulated_bluetooth.proto emulated_bluetooth_device.proto)
target_link_libraries(emulated-bluetooth-service PUBLIC android-grpc-endpoint)

# Lets distribute the proto file
install(
  FILES ${AEMU_PROTO_DIR}/services/bluetooth/emulated_bluetooth.proto
        ${AEMU_PROTO_DIR}/services/bluetooth/emulated_bluetooth_device.proto
  DESTINATION lib)
add_subdirectory(server)
