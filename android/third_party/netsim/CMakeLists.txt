add_subdirectory(proto)
add_subdirectory(util)
add_subdirectory(backend)

if(NOT LINUX_AARCH64)
    message(STATUS "Using prebuilt netsim binaries")
    prebuilt(NETSIM)

    # Binplace to objs
    android_target_dependency(packet-streamer-client-lib all
        NETSIM_DEPENDENCIES)

else()
    message(WARNING "Netsimd is not available for this platform, bluetooth/wifi emulation will require a remote instance.")
endif()
