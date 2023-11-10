load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "compat",
    srcs = ["src/mman_memfd_create.cc"],
    hdrs = [
    "include/asm/kvm.h",
    "include/asm/kvm_para.h",
    ],
    includes = ["include"],
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = ["@gcc_lib//:libutil"],
)
