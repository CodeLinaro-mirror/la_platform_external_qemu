android_grpc_service_lib(
  TARGET emulator-stats-service SOURCE_DIR
         ${AEMU_PROTO_DIR}/services/emulator-stats SOURCES emulator_stats.proto)

# Lets distribute the proto file
install(FILES ${AEMU_PROTO_DIR}/services/emulator-stats/emulator_stats.proto
        DESTINATION lib)

add_subdirectory(server)
