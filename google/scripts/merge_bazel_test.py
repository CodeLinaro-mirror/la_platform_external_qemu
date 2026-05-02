import unittest
import ast
from unittest.mock import mock_open, patch
from merge_bazel import *


class TestMergeBazel(unittest.TestCase):

    def test_simple_rule(self):
        content = """
cc_library(
    name = "my_lib",
    srcs = ["foo.c"],
)
"""
        library = BazelRuleLibrary(unique=set())
        with patch("builtins.open", mock_open(read_data=content)):
            transform_bazel("dummy_file", "linux", library)

        rule = library.get("my_lib")
        self.assertIsNotNone(rule)
        self.assertEqual(rule.sort, "cc_library")
        self.assertEqual(rule.params["name"].value, "my_lib")
        self.assertEqual(list(rule.params["srcs"].value), ["foo.c"])

    def test_load_command(self):
        content = """
load("@rules_cc//cc:defs.bzl", "cc_library")
"""
        library = BazelRuleLibrary(unique=set())
        with patch("builtins.open", mock_open(read_data=content)):
            transform_bazel("dummy_file", "linux", library)

        self.assertEqual(len(library.load_cmds), 1)
        load_cmd = list(library.load_cmds)[0]
        self.assertEqual(load_cmd.label, "@rules_cc//cc:defs.bzl")
        self.assertEqual(load_cmd.rules, ["cc_library"])

    def test_variable_reference(self):
        """Test that variable references are preserved as BazelExpression.

        This test fails with the old exec() approach due to NameError
        because TRACE_BACKEND is not defined in the Python environment.
        """
        content = """
genrule(
    name = "test_rule",
    cmd = "$(location :tracetool) --backend=" + TRACE_BACKEND + " --group=foo",
)
"""
        library = BazelRuleLibrary(unique=set())
        with patch("builtins.open", mock_open(read_data=content)):
            transform_bazel("dummy_file", "linux", library)

        rule = library.get("test_rule")
        self.assertIsNotNone(rule)
        cmd_val = rule.params["cmd"].value
        self.assertIsInstance(cmd_val, BazelExpression)
        # We check that it contains the variable reference
        self.assertIn("TRACE_BACKEND", str(cmd_val))


if __name__ == "__main__":
    unittest.main()
