android_grpc_service_lib(
  TARGET emulator_controller-service SOURCE_DIR
         ${AEMU_PROTO_DIR}/services/emulator-controller
  SOURCES emulator_controller.proto)

# Lets distribute the proto file
install(
  FILES ${AEMU_PROTO_DIR}/services/emulator-controller/emulator_controller.proto
  DESTINATION lib)

add_subdirectory(client)
add_subdirectory(server)
