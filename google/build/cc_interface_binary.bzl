"""Support for building plugins"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_import.bzl", "cc_import")

def cc_interface_binary(
        name,
        win_def_file = None,
        data = None,
        tags = None,
        visibility = None,
        linkopts = None,
        mac_entitlements = "@qemu//:google/build/darwin-entitlements.plist",
        **kwargs):
    """Creates a C++ binary and a shared library with an interface library.

    This can be used to create plugins, i.e. dlls that can be loaded into the main executable. It will
    create an interface library to which your plugin can link. The name of the interface library
    is `${name}.if`

    All the symbols in the win_def_file will be exported from the executable.
    On darwin the executable will be signed with the provided entitlements.


    Args:
        name (str): The name of the C++ binary target.
        win_def_file (str): The path to the Windows DEF file for the shared library.
        data (list, optional): Additional data files to be included in the C++ binary target. Defaults to None.
        tags (list, optional): A list of tags to be applied to the targets. Defaults to None.
        visibility (list, optional): A list of labels that control the visibility of the targets. Defaults to None.
        linkopts (list, optional): Additional linker options for the shared library target. Defaults to None.
        mac_entitlements (str): The path to the entlitlements to be used (Darwin only)
        **kwargs: Additional arguments to be passed to the C++ binary and shared library targets.

    Returns:
        None

    Example:
        cc_interface_binary(
            name = "my_binary",
            win_def_file = "my_binary.def",
            data = ["my_data.txt"],
            visibility = ["//visibility:public"],
            linkopts = ["-lmylib"],
            deps = [":my_lib"],
        )

    In this example, the `cc_interface_binary` function creates:
    1. A C++ binary target named `my_binary`.
    2. An interface library target named `my_binary.if`.

    Other targets can link against the interface library `my_binary.if`, and can be loaded by `my_binary`
    """

    cc_binary(
        name = name,
        tags = tags,
        visibility = visibility,
        linkopts = (linkopts or []) + select({
            # win_def_file is ignored if linkshared=False so we have to pass the linkopts manually.
            "@platforms//os:windows": ["/DEF:$(location " + win_def_file + ")"] if win_def_file else [],
            "//conditions:default": [],
        }),
        data = data,
        additional_linker_inputs = ([win_def_file] if win_def_file else []),
        **kwargs
    )

    # Sign the binary on darwin.
    native.genrule(
        name = name + "_signed",
        srcs = [name, mac_entitlements],
        outs = [name + ".signed"],
        executable = True,
        exec_compatible_with = ["@platforms//os:macos"],
        target_compatible_with = ["@platforms//os:macos"],
        cmd = "cp -L $(location {name}) \"$@\" && /usr/bin/codesign -s - --entitlements $(location {entitlements}) \"$@\"".format(name = name, entitlements = mac_entitlements),
        visibility = visibility,
    )

    # This DLL rule only exists because the EXE rule above doesn't export the interface_library group needed
    # by cc_import. See github.com/bazelbuild/bazel/issues/15107
    # It will never be used directly.
    dll_name = "_" + name + "_dll"
    cc_binary(
        name = dll_name,
        win_def_file = win_def_file,
        linkshared = True,
        tags = ["manual"],
        visibility = ["//visibility:private"],
        linkopts = linkopts,
        **kwargs
    )

    if_group_name = "_" + name + ".if"
    native.filegroup(
        name = if_group_name,
        srcs = [dll_name],
        output_group = "interface_library",
        tags = ["manual"],
        visibility = ["//visibility:private"],
    )

    if_name = name + ".if"
    cc_import(
        name = if_name,
        interface_library = if_group_name,
        system_provided = True,
        tags = ["manual"],
        visibility = visibility,
    )
