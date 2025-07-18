
// clang-format off
// IWYU pragma: begin_keep
#include "qemu/osdep.h"
#include "qemu/module.h"
// IWYU pragma: end_keep
// clang-format on

#ifdef _WIN32
#define LIB_PREFIX ""
#else
#define LIB_PREFIX "lib"
#endif

#ifdef TARGET_X86_64
#define ARCH_SUFFIX "x86_64"
#elif defined TARGET_AARCH64
#define ARCH_SUFFIX "aarch64"
#elif defined TARGET_RISCV64
#define ARCH_SUFFIX "riscv64"
#else
#error Unsupported CPU architecture
#endif

const QemuModinfo qemu_modinfo[] = {
        {
                /* hw-display-virtio-gpu.modinfo */
                .name = LIB_PREFIX "hw-display-virtio-gpu",
                .objs = ((const char*[]){"virtio-gpu-base", "virtio-gpu-device", "vhost-user-gpu",
                                         NULL}),
        },
        {
                /* hw-display-virtio-gpu-rutabaga.modinfo */
                .name = LIB_PREFIX "hw-display-virtio-gpu-rutabaga",
                .objs = ((const char*[]){"virtio-gpu-rutabaga-device", NULL}),
                .deps = ((const char*[]){LIB_PREFIX "hw-display-virtio-gpu", NULL}),
        },
        {
                /* hw-display-virtio-gpu-pci.modinfo */
                .name = LIB_PREFIX "hw-display-virtio-gpu-pci",
                .objs = ((const char*[]){"virtio-gpu-pci-base", "virtio-gpu-pci",
                                         "vhost-user-gpu-pci", NULL}),
        },
        {
                /* hw-display-virtio-gpu-pci-rutabaga.modinfo */
                .name = LIB_PREFIX "hw-display-virtio-gpu-pci-rutabaga",
                .objs = ((const char*[]){"virtio-gpu-rutabaga-pci", NULL}),
                .deps = ((const char*[]){LIB_PREFIX "hw-display-virtio-gpu-pci", NULL}),
        },
        {
                /* hw-display-virtio-vga.modinfo */
                .name = LIB_PREFIX "hw-display-virtio-vga",
                .objs = ((const char*[]){"virtio-vga-base", "virtio-vga", "vhost-user-vga", NULL}),
        },
        {
                /* hw-display-virtio-vga-rutabaga.modinfo */
                .name = LIB_PREFIX "hw-display-virtio-vga-rutabaga",
                .objs = ((const char*[]){"virtio-vga-rutabaga", NULL}),
                .deps = ((const char*[]){LIB_PREFIX "hw-display-virtio-vga", NULL}),
        },
        {
                .name = LIB_PREFIX "avd",
                .opts = ((const char*[]){"device", NULL}),
                .objs = ((const char*[]){"avdstart", "virtio-goldfish-adb", "goldfish_battery",
                                         "avdend", NULL}),
                .deps = ((const char*[]){LIB_PREFIX "virtio-vsock-goldfish-" ARCH_SUFFIX, NULL}),
        },
        {
                .name = LIB_PREFIX "grpc",
                .opts = ((const char*[]){"device", NULL}),
                .objs = ((const char*[]){"grpc", NULL}),
                // The gRPC module contains gpu forwarding logic, and links against the
                // vga lib
                .deps = ((const char*[]){LIB_PREFIX "hw-display-virtio-gpu-rutabaga",
                                         LIB_PREFIX "avd", NULL}),
        },
        {
                .name = LIB_PREFIX "virtio-vsock-goldfish-" ARCH_SUFFIX,
                .objs = ((const char*[]){"virtio-goldfish-vsock", "virtio-goldfish-vsock-pci",
                                         "virtio-goldfish-hostfwd-socket", NULL}),
                .opts = ((const char*[]){"device", NULL}),
                .deps = ((const char*[]){LIB_PREFIX "avd", NULL}),
        },
#ifdef __linux__
        {
                /* audio-pa.modinfo */
                .name = LIB_PREFIX "audio-pa",
                .objs = ((const char*[]){"audio-pa", NULL}),
        },
#endif
#ifdef TARGET_X86_64
        {
                /* accel-tcg-x86_64.modinfo */
                .name = LIB_PREFIX "accel-tcg-" ARCH_SUFFIX,
                .arch = ARCH_SUFFIX,
                .objs = ((const char*[]){("tcg-accel-ops"), NULL}),
        },
#endif
        {
                /* end of list */
        }};
