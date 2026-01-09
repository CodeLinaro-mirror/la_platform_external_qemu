android_grpc_service_lib(
  TARGET ui-controller-service SOURCE_DIR
         ${AEMU_PROTO_DIR}/services/ui-controller
  SOURCES ui_controller_service.proto)

# Lets distribute the proto file
install(
  FILES ${AEMU_PROTO_DIR}/services/ui-controller/ui_controller_service.proto
  DESTINATION lib)

add_subdirectory(server)
