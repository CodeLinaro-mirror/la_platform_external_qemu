android_grpc_service_lib(
  TARGET modem-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/incubating/modem
  SOURCES modem_service.proto)

add_subdirectory(server)
add_subdirectory(client)
