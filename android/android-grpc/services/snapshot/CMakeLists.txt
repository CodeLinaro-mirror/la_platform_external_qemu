android_grpc_service_lib(
  TARGET snapshot-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/snapshot
  SOURCES snapshot_service.proto)

target_link_libraries(snapshot-service PUBLIC android-emu-protos)

# Lets distribute the proto file
install(FILES ${AEMU_PROTO_DIR}/services/snapshot/snapshot_service.proto
              ${AEMU_PROTO_DIR}/services/snapshot/snapshot.proto
        DESTINATION lib)

add_subdirectory(client)
add_subdirectory(server)

android_add_executable(
  TARGET snapshot-service-server-perf
  NODISTRIBUTE SOURCE_DIR test
  SRC android/emulation/control/snapshot/TarStreamPerf.cpp
  DEPS android-emu-base android-emu-base-headers android-emu-utils
       emulator-gbench)
