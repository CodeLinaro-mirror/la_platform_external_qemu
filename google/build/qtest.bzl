"""Bazel macros for defining QEMU qtest targets and stub plugins."""

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

QTEST_COPTS = [
    "-fwrapv",
]

QTEST_LOCAL_DEFINES = [
    "_FILE_OFFSET_BITS=64",
    "_GNU_SOURCE",
    "_LARGEFILE_SOURCE",
]

# libqtest cannot drive qemu-system-<arch> on Windows (spawning and waiting on
# the child process fails), so the qtest stack is built and run on Linux and
# macOS only; Windows builds skip these targets.
QTEST_TARGET_COMPATIBLE_WITH = select({
    "@platforms//os:windows": ["@platforms//:incompatible"],
    "//conditions:default": [],
})

def qemu_qtest_goldfish_stub(name, **kwargs):
    """Defines a stub libgoldfish_<arch> shared library for standalone qtest runs.

    The stub is architecture independent; `name` selects the plugin file name
    that aemu_main.c loads (e.g. "goldfish_x86_64").

    Args:
        name: Target name of the shared library.
        **kwargs: Additional arguments forwarded to cc_binary.
    """
    visibility = kwargs.pop("visibility", ["//visibility:public"])
    cc_binary(
        name = name,
        testonly = True,
        srcs = ["@qemu//:google/system/qtest_goldfish_stub.c"],
        copts = QTEST_COPTS,
        dynamic_deps = [
            "@glib//glib:glib_shared",
            "@glib//gmodule:gmodule_shared",
        ],
        linkopts = select({
            "@platforms//os:macos": ["-undefined dynamic_lookup"],
            "//conditions:default": [],
        }),
        linkshared = True,
        local_defines = QTEST_LOCAL_DEFINES,
        target_compatible_with = QTEST_TARGET_COMPATIBLE_WITH,
        visibility = visibility,
        deps = [
            "@qemu//:aemu_func_defs",
            "@qemu//:qemu-headers",
            "@glib//glib:glib_import",
            "@glib//gmodule:gmodule_import",
        ] + select({
            # //google/compat/linux:compat provides x86_64 glibc 2.17 shims
            # via @gcc_lib//:libutil and is not used on linux_aarch64.
            "@goldfish_build//platforms:is_linux_x64": [
                "@qemu//google/compat/linux:compat",
            ],
            "//conditions:default": [],
        }),
        **kwargs
    )

def qemu_qtest(
        name,
        srcs,
        arch = "x86_64",
        plugin = None,
        extra_plugin = None,
        deps = None,
        data = None,
        env = None,
        args = None,
        copts = None,
        **kwargs):
    """Defines a QEMU qtest target running against qemu-system-<arch>.

    Args:
        name: Target name of the cc_test.
        srcs: C source files for the qtest binary.
        arch: Target QEMU architecture ("x86_64" or "aarch64").
        plugin: Goldfish plugin target loaded via ANDROID_EMULATOR_LIBRARY_DIR
            (defaults to "@qemu//:goldfish_<arch>" stub plugin).
        extra_plugin: Optional higher-layer plugin target chained by the stub
            plugin via QTEST_GOLDFISH_EXTRA_PLUGIN.
        deps: Additional cc_library dependencies.
        data: Additional runtime data dependencies.
        env: Additional environment variables.
        args: Test arguments (defaults to ["--tap", "-k"]).
        copts: Additional C compiler flags.
        **kwargs: Additional arguments forwarded to cc_test.
    """
    deps = deps or []
    data = data or []
    env = env or {}
    if args == None:
        args = ["--tap", "-k"]
    copts = copts or []

    qemu_bin = "@qemu//:qemu-system-" + arch
    gf_plugin = plugin if plugin != None else ("@qemu//:goldfish_" + arch)

    test_env = {
        # Upstream QEMU runs qtests with detect_leaks=0 under ASan because
        # qtest_irq_intercept_in (used by e.g. rtc-test) intentionally leaks
        # the previous IRQ handler state on the QEMU side.
        "ASAN_OPTIONS": "detect_leaks=0",
        "QTEST_QEMU_BINARY": "$(rootpath " + qemu_bin + ")",
        "QTEST_QEMU_IMG": "$(rootpath @qemu//:qemu-img)",
        "QTEST_GOLDFISH_PLUGIN": "$(rootpath " + gf_plugin + ")",
    }
    test_data = [
        qemu_bin,
        gf_plugin,
        "@qemu//:qemu-img",
        "@qemu//:qemu-keymaps",
        "@qemu//:qtest-roms",
    ]

    if extra_plugin != None:
        test_env["QTEST_GOLDFISH_EXTRA_PLUGIN"] = "$(rootpath " + extra_plugin + ")"
        test_data.append(extra_plugin)

    test_env.update(env)

    cc_test(
        name = name,
        srcs = srcs,
        args = args,
        copts = QTEST_COPTS + copts,
        data = test_data + data,
        dynamic_deps = [
            "@glib//glib:glib_shared",
            "@glib//gmodule:gmodule_shared",
        ],
        env = test_env,
        linkopts = select({
            "@platforms//os:macos": [
                "-framework CoreFoundation",
                "-framework IOKit",
            ],
            "@goldfish_build//platforms:is_linux_x64": ["-lrt"],
            "@goldfish_build//platforms:is_linux_aarch64": ["-lrt"],
            "//conditions:default": [],
        }),
        linkstatic = True,
        local_defines = QTEST_LOCAL_DEFINES,
        target_compatible_with = QTEST_TARGET_COMPATIBLE_WITH,
        deps = [
            "@qemu//:libprintstub",
            "@qemu//:libqemuutil",
            "@qemu//:libqom",
            "@qemu//:libqos",
            "@qemu//:qemu-headers",
        ] + select({
            "@goldfish_build//platforms:is_linux_x64": [
                "@qemu//google/compat/linux:compat",
            ],
            "//conditions:default": [],
        }) + deps,
        **kwargs
    )
