# Waterfall is not supported on windows.
# The protobuf service definitions contain
# stdout/stderr, which the compiler cannot handle
if(NOT WINDOWS_MSVC_X86_64)
  android_grpc_service_lib(
    TARGET waterfall-service SOURCE_DIR ${AEMU_PROTO_DIR}/services/waterfall
    SOURCES control_socket.proto waterfall.proto)
  # Lets distribute the proto file
  install(FILES ${AEMU_PROTO_DIR}/services/waterfall/control_socket.proto
                ${AEMU_PROTO_DIR}/services/waterfall/waterfall.proto
          DESTINATION lib)
endif()

add_subdirectory(server)
