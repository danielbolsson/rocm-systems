#!/usr/bin/env python3
#
# Copyright (C) Advanced Micro Devices. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""Mock-based unit tests for CUID seed provisioning.

Two requirements, tested at the layers that actually enforce them.

**A seed of any length but 32 octets is refused, and nothing is provisioned.**
The C entry point cannot enforce this: ``amdsmi_set_cuid_seed()`` takes
``const uint8_t[AMDSMI_CUID_SEED_SIZE]`` and so has no length to check -- a
short buffer is a caller bug that reads off the end, not a value it can reject.
The check therefore lives twice above it, in the CLI (so the error names the
file the operator passed) and in the Python binding (so a binding caller cannot
bypass the CLI), and both are exercised here. A wrong-sized seed is corruption,
not a shorter secret: accepting one silently would change every derived CUID on
the machine with nothing in the values to show it.

**No octet of the seed reaches any output stream.** ``amd-smi`` is run casually
under sudo and its output is pasted into tickets, so the provisioning command
must report the fingerprint and never the secret. Nothing here provisions
anything: the library call is stubbed out, which is also what makes the test
safe to run -- a real provisioning re-keys the whole node.

Both classes stub the C library, so they run without GPU hardware and without a
compiled ``amdsmi``. The binding-level class needs a real importable ``amdsmi``
and skips when there is none.
"""

import importlib.util
import io
import json
import os
import sys
import types
import unittest
from contextlib import redirect_stderr, redirect_stdout

# ``common.common`` bootstraps the real amdsmi package at import time, which
# fails on a stale or mismatched install. The CLI classes below fully stub
# ``amdsmi`` and only need ``amdsmi_path`` to locate the *installed* CLI
# fallback, so degrade gracefully, exactly as test_cli_set_clk_limit.py does.
# Catching Exception rather than ImportError on purpose: common.common runs
# build_type_lists() against the installed amdsmi at import time, so an install
# that predates a new enum raises AttributeError here rather than failing to
# import. Either way this suite does not need it.
try:
    from common.common import amdsmi_path
except Exception:  # pragma: no cover - harness/install unavailable or stale
    amdsmi_path = None

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_SOURCE_CLI_DIR = os.path.normpath(os.path.join(_THIS_DIR, "..", "..", "..", "..", "amdsmi_cli"))
_INSTALLED_CLI_DIR = (
    os.path.join(os.path.dirname(os.path.dirname(amdsmi_path)), "libexec", "amdsmi_cli")
    if amdsmi_path
    else ""
)


def _resolve_cli_dir():
    for cli_dir in (_SOURCE_CLI_DIR, _INSTALLED_CLI_DIR):
        if cli_dir and os.path.isfile(os.path.join(cli_dir, "subcommands", "set_value.py")):
            return cli_dir
    return None


_CLI_DIR = _resolve_cli_dir()
SET_VALUE_PATH = os.path.join(_CLI_DIR, "subcommands", "set_value.py") if _CLI_DIR else ""

SEED_SIZE = 32

# Distinctive, so a leak is unmistakable in a captured stream: every octet is
# unique and none of it is 0x00 or 0xff, which turn up in unrelated output.
SEED_32 = bytes(range(0x40, 0x40 + SEED_SIZE))

# What the stubbed library reports after provisioning. Not the fingerprint of
# SEED_32 -- nothing here computes one -- and not the canonical fallback
# fingerprint be8937fba7ed4e6f either, so that "the command reported what the
# library told it" cannot be satisfied by a command that reports a constant.
PROVISIONED_FINGERPRINT = "1c2d3e4f50617283"


def _install_fake_amdsmi():
    """Register a stub ``amdsmi`` package so ``set_value.py`` imports cleanly."""
    amdsmi_pkg = types.ModuleType("amdsmi")
    interface = types.ModuleType("amdsmi.amdsmi_interface")
    exception = types.ModuleType("amdsmi.amdsmi_exception")
    wrapper = types.ModuleType("amdsmi.amdsmi_wrapper")

    # Constants set_value.py binds at import time.
    interface.AMDSMI_MAX_PPT_LIMIT = 0
    interface.AMDSMI_MAX_UTIL = 100
    interface.AMDSMI_CUID_SEED_SIZE = SEED_SIZE
    interface.amdsmi_wrapper = wrapper

    exception.AmdSmiLibraryException = type("AmdSmiLibraryException", (Exception,), {})

    amdsmi_pkg.amdsmi_interface = interface
    amdsmi_pkg.amdsmi_exception = exception

    sys.modules["amdsmi"] = amdsmi_pkg
    sys.modules["amdsmi.amdsmi_interface"] = interface
    sys.modules["amdsmi.amdsmi_exception"] = exception
    sys.modules["amdsmi.amdsmi_wrapper"] = wrapper
    return interface


def _load_set_value_module():
    if _CLI_DIR and _CLI_DIR not in sys.path:
        sys.path.insert(0, _CLI_DIR)
    spec = importlib.util.spec_from_file_location("set_value_cuid_under_test", SET_VALUE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _RecordingLogger:
    """``self.logger`` stub that keeps everything the command published.

    ``output`` is the complete input to every renderer -- human-readable, JSON
    and CSV are pure functions of it -- so a seed octet that is not in here and
    not in the process's own streams cannot appear in any of them.
    """

    def __init__(self):
        self.format = "human"
        self.output = {}
        self.printed = []

    def print_output(self, *args, **kwargs):
        self.printed.append(dict(self.output))

    def store_output(self, device, key, value):
        self.output[key] = value

    def clear_multiple_devices_output(self):
        pass


class _CuidSeedTestBase(unittest.TestCase):
    _SAVED_MODULE_NAMES = (
        "amdsmi",
        "amdsmi.amdsmi_interface",
        "amdsmi.amdsmi_exception",
        "amdsmi.amdsmi_wrapper",
    )

    @classmethod
    def setUpClass(cls):
        if not SET_VALUE_PATH:
            raise unittest.SkipTest("amd-smi CLI set_value.py not found (source or installed)")
        cls._saved_modules = {name: sys.modules.get(name) for name in cls._SAVED_MODULE_NAMES}
        cls.interface = _install_fake_amdsmi()
        cls.module = _load_set_value_module()

    @classmethod
    def tearDownClass(cls):
        for name, saved in cls._saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved

    def setUp(self):
        self.set_calls = []
        self.interface.amdsmi_set_cuid_seed = self.set_calls.append
        # A node that has just been provisioned: provisioned, and therefore
        # *not* fingerprinting the public fallback seed. Deliberately not
        # be8937fba7ed4e6f -- the canonical fallback fingerprint -- because this
        # asserts that the command reports back whatever the library said, and a
        # stub echoing the fallback constant would also be satisfied by a
        # command that printed that constant from its own source.
        self.interface.amdsmi_get_cuid_seed_info = lambda: {
            "provisioned": True,
            "fingerprint": PROVISIONED_FINGERPRINT,
        }
        self.logger = _RecordingLogger()
        self.cmd = self.module.SetValueCommands()
        self.cmd.logger = self.logger

    def _seed_file(self, payload):
        import tempfile

        handle = tempfile.NamedTemporaryFile(prefix="cuid-seed-", delete=False)
        handle.write(payload)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return handle.name

    def _provision(self, payload, from_stdin=False):
        """Drive the real ``_set_cuid_seed`` and return (stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        if from_stdin:
            saved_stdin = sys.stdin
            sys.stdin = types.SimpleNamespace(buffer=io.BytesIO(payload))
            self.addCleanup(setattr, sys, "stdin", saved_stdin)
            source = "-"
        else:
            source = self._seed_file(payload)
        with redirect_stdout(out), redirect_stderr(err):
            self.cmd._set_cuid_seed(source)
        return source, out.getvalue(), err.getvalue()


class TestCuidSeedLengthIsEnforced(_CuidSeedTestBase):
    """A seed that is not exactly 32 octets is refused, and nothing changes."""

    def test_sixteen_octet_seed_is_refused(self):
        # Half a seed. Not "a weaker secret" -- a truncated file.
        path = self._seed_file(b"\x01" * 16)
        with self.assertRaises(ValueError) as caught:
            self.cmd._set_cuid_seed(path)
        message = str(caught.exception)
        self.assertIn("exactly 32 bytes", message)
        self.assertIn("got 16", message)
        # The error names the file: that is why the check is duplicated here
        # rather than left to the binding.
        self.assertIn(path, message)
        self.assertEqual(self.set_calls, [], "a refused seed must not reach the library")
        self.assertEqual(self.logger.output, {}, "a refused seed must not report a new state")

    def test_sixty_four_octet_seed_is_refused(self):
        # Two seeds concatenated, or a hex-encoded one saved as bytes. Silently
        # truncating to the first 32 octets would provision something nobody
        # chose.
        path = self._seed_file(b"\x02" * 64)
        with self.assertRaises(ValueError) as caught:
            self.cmd._set_cuid_seed(path)
        message = str(caught.exception)
        self.assertIn("exactly 32 bytes", message)
        self.assertIn("got 64", message)
        self.assertIn(path, message)
        self.assertEqual(self.set_calls, [], "a refused seed must not reach the library")
        self.assertEqual(self.logger.output, {}, "a refused seed must not report a new state")

    def test_short_seed_on_stdin_is_refused(self):
        # stdin is the other accepted source, and it is the one an operator
        # reaches for when piping from a secret store, so it gets the same
        # check rather than a shorter path into the library.
        saved_stdin = sys.stdin
        sys.stdin = types.SimpleNamespace(buffer=io.BytesIO(b"\x03" * 16))
        self.addCleanup(setattr, sys, "stdin", saved_stdin)
        with self.assertRaises(ValueError) as caught:
            self.cmd._set_cuid_seed("-")
        self.assertIn("exactly 32 bytes", str(caught.exception))
        self.assertEqual(self.set_calls, [])

    def test_exactly_thirty_two_octets_is_accepted(self):
        # The control. Without it the two refusals above would also pass
        # against a command that refused everything.
        _source, _out, _err = self._provision(SEED_32)
        self.assertEqual(self.set_calls, [SEED_32])
        self.assertEqual(self.logger.output["cuid_seed_provisioned"], True)


class TestCuidSeedNeverReachesOutput(_CuidSeedTestBase):
    """No octet of a provisioned seed appears in anything the command emits."""

    def _assert_no_seed_material(self, blob, where):
        self.assertNotIn(SEED_32.hex(), blob.lower(), f"whole seed as hex in {where}")
        self.assertNotIn(SEED_32.decode("latin-1"), blob, f"whole seed verbatim in {where}")
        # Any eight consecutive octets of a 256-bit secret is a quarter of it
        # and enough to confirm a guess, so a partial leak is a leak. Eight is
        # also long enough not to collide with unrelated output by chance.
        for start in range(0, SEED_SIZE - 8 + 1):
            window = SEED_32[start : start + 8]
            self.assertNotIn(
                window.hex(), blob.lower(), f"seed[{start}:{start + 8}] hex in {where}"
            )
            self.assertNotIn(
                window.decode("latin-1"), blob, f"seed[{start}:{start + 8}] verbatim in {where}"
            )

    def test_no_seed_octet_in_any_output_stream(self):
        source, out, err = self._provision(SEED_32)

        # It really was provisioned; otherwise this asserts about nothing.
        self.assertEqual(self.set_calls, [SEED_32])
        self.assertTrue(self.logger.printed, "the command should report the new state")

        for name, blob in (
            ("stdout", out),
            ("stderr", err),
            ("logger.output", repr(self.logger.output)),
            ("logger.output as JSON", json.dumps(self.logger.output, default=repr)),
            ("printed payloads", repr(self.logger.printed)),
        ):
            self._assert_no_seed_material(blob, name)

    def test_no_seed_octet_in_any_output_stream_from_stdin(self):
        _source, out, err = self._provision(SEED_32, from_stdin=True)
        self.assertEqual(self.set_calls, [SEED_32])
        for name, blob in (
            ("stdout", out),
            ("stderr", err),
            ("logger.output", repr(self.logger.output)),
            ("logger.output as JSON", json.dumps(self.logger.output, default=repr)),
        ):
            self._assert_no_seed_material(blob, name)

    def test_what_is_reported_is_the_fingerprint_and_the_state(self):
        # The positive half: the command is useless if it reports nothing, and
        # "nothing was leaked" is trivially true of a command that prints
        # nothing. These two keys, and no third one carrying the secret.
        self._provision(SEED_32)
        self.assertEqual(
            sorted(self.logger.output), ["cuid_seed_fingerprint", "cuid_seed_provisioned"]
        )
        self.assertEqual(self.logger.output["cuid_seed_fingerprint"], PROVISIONED_FINGERPRINT)


class TestCuidSeedLengthEnforcedInTheBinding(unittest.TestCase):
    """The same refusal in the Python binding, below the CLI.

    A binding caller never runs ``_set_cuid_seed``. If the length check existed
    only in the CLI, ``amdsmi.amdsmi_set_cuid_seed(b"...")`` from a script would
    hand a short buffer to a C entry point that reads 32 octets from it.
    """

    @classmethod
    def setUpClass(cls):
        # The real package, not the stub the CLI classes install: this is a test
        # of the real binding. The sibling classes restore sys.modules in
        # tearDownClass, so whichever runs first, this import is the real one.
        try:
            from amdsmi import amdsmi_exception, amdsmi_interface
        except Exception as e:  # pragma: no cover - no amdsmi installed
            raise unittest.SkipTest(f"amdsmi package not importable: {e}")
        if not isinstance(getattr(amdsmi_interface, "__file__", None), str):
            raise unittest.SkipTest("a stubbed amdsmi is loaded in this interpreter")
        if not hasattr(amdsmi_interface, "amdsmi_set_cuid_seed") or not hasattr(
            amdsmi_interface.amdsmi_wrapper, "amdsmi_set_cuid_seed"
        ):
            # An amdsmi from before this change. Skipping is right: the check
            # under test does not exist in it, and asserting against the
            # in-tree source while importing an installed package would be
            # testing the wrong file anyway.
            raise unittest.SkipTest(
                f"installed amdsmi ({amdsmi_interface.__file__}) predates the CUID seed API"
            )
        cls.interface = amdsmi_interface
        cls.parameter_exception = amdsmi_exception.AmdSmiParameterException

    def setUp(self):
        self.calls = []
        self.saved = self.interface.amdsmi_wrapper.amdsmi_set_cuid_seed

        def _record(buffer):
            # Nothing is provisioned here: a real one re-keys the whole node.
            self.calls.append(bytes(buffer))
            return 0  # AMDSMI_STATUS_SUCCESS

        self.interface.amdsmi_wrapper.amdsmi_set_cuid_seed = _record
        self.addCleanup(setattr, self.interface.amdsmi_wrapper, "amdsmi_set_cuid_seed", self.saved)

    def test_wrong_length_seeds_are_refused_before_the_library(self):
        for length in (0, 16, 31, 33, 64):
            with self.subTest(length=length):
                with self.assertRaises(self.parameter_exception):
                    self.interface.amdsmi_set_cuid_seed(b"\x05" * length)
        self.assertEqual(self.calls, [], "a refused seed must not reach the library")

    def test_thirty_two_octets_reaches_the_library_unchanged(self):
        self.interface.amdsmi_set_cuid_seed(SEED_32)
        self.assertEqual(self.calls, [SEED_32])


if __name__ == "__main__":
    unittest.main()
