android_grpc_service_lib(
  TARGET sensor-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/incubating/sensor
  SOURCES sensor_service.proto)

add_subdirectory(client)
add_subdirectory(server)
