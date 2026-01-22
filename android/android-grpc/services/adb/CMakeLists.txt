android_grpc_service_lib(
  TARGET adb-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/adb
  SOURCES adb_service.proto)

# Lets distribute the proto file from the canonical location
install(FILES ${AEMU_PROTO_DIR}/services/adb/adb_service.proto DESTINATION lib)

add_subdirectory(server)
