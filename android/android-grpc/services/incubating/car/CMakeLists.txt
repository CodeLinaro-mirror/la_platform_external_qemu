android_grpc_service_lib(
  TARGET car-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/incubating/car
  SOURCES car_service.proto)

add_subdirectory(client)
add_subdirectory(server)
