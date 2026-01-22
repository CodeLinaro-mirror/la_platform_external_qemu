android_grpc_service_lib(
  TARGET avd-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/incubating/avd
  SOURCES avd_service.proto)

add_subdirectory(client)
add_subdirectory(server)
