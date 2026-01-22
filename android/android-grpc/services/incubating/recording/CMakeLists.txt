android_grpc_service_lib(
  TARGET screen-recording-service SOURCE_DIR
         ${AEMU_PROTO_DIR}/services/incubating/recording
  SOURCES screen_recording_service.proto)

add_subdirectory(server)
add_subdirectory(client)
