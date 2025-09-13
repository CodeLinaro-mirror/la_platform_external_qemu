set(ANDROID_EMU_PROTOS_PATH ${ANDROID_QEMU2_TOP_DIR}/android/emu/protos)
android_grpc_service_lib(
  TARGET emulator_controller-service SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/proto
  SOURCES emulator_controller.proto INCLUDES ${ANDROID_EMU_PROTOS_PATH})
target_link_libraries(emulator_controller-service PUBLIC android-emu-protos)
# Lets distribute the proto file
install(FILES proto/emulator_controller.proto DESTINATION lib)

add_subdirectory(client)
add_subdirectory(server)
