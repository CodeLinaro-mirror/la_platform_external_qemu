load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "compat",
    srcs = ["src/mman_memfd_create.cc"],
    linkstatic = True,
    visibility = ["//visibility:public"],
    deps = ["@gcc_lib//:libutil"],
)
