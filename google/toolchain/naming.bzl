load("@rules_pkg//pkg:providers.bzl", "PackageVariablesInfo")

def _build_id_from_cmdline_impl(ctx):
    return PackageVariablesInfo(values = {"build_id": ctx.build_setting_value})

build_id_from_command_line = rule(
    implementation = _build_id_from_cmdline_impl,
    build_setting = config.string(flag = True),
)
