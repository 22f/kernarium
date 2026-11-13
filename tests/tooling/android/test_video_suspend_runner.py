#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The video-suspend runner's suspend step, without a guest.

run_android_video_suspend.py suspends the guest by the host's route and by
Android's own, with a clip playing and the screen recorded. After each wake it
reads the launcher's log from where it stood before the suspend: a host GPU
device reset across the suspend (tests/integration/support/host_gpu_reset.py)
fails the step by that name with the first line, before any check of the
player or the recorder fails on a guest whose graphics are gone.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "tests/integration/android/run_android_video_suspend.py"
SPEC = importlib.util.spec_from_file_location("run_android_video_suspend", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)
sys.path.insert(0, str(REPOSITORY / "tests/integration/support"))
import host_gpu_reset  # noqa: E402

KERNEL_GPU_ERROR = "[  186.505954][   T11] [drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1204 (command 0x202)\n"
KERNEL_RESOURCE_ERROR = "[  186.505990][   T11] [drm:virtio_gpu_dequeue_ctrl_func] *ERROR* response 0x1203 (command 0x104)\n"
HOST_GPU_REFUSAL = "Virtio-GPU submit rejected unknown context: id=3 bytes=44\n"
RESUMED = "[  186.1][  T1] PM: suspend exit\n"


class FakeGuest:
    """The parts of the runner's Guest that a suspend step touches: its launcher log, its shell and its stream list."""

    def __init__(self, directory: str, silent: bool = False) -> None:
        self.log = Path(directory) / "emulator.log"
        self.log.write_text("boot\n")
        self.silent = silent
        self.commands: list[str] = []

    def shell(self, command: str, timeout: float = 0) -> str:
        self.commands.append(command)
        if self.silent:
            raise runner.Failure(f"'{command}' exited 255: closed")
        return ""

    def adb_run(self, *arguments: str, timeout: float = 0) -> subprocess.CompletedProcess:
        self.commands.append(" ".join(arguments))
        return subprocess.CompletedProcess(list(arguments), 0, stdout=b"")

    def closes(self) -> list[tuple[bool, str, str]]:
        return []


class SuspendStepTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = directory.name
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def route(self, guest: FakeGuest, text: str):
        def route(_: object) -> None:
            with guest.log.open("a") as stream:
                stream.write(text)
        return mock.patch.dict(runner.ROUTES, {"host": route, "guest": route})

    def test_a_wake_with_no_reset_in_the_log_passes_and_the_guest_answers(self) -> None:
        guest = FakeGuest(self.directory)
        with self.route(guest, RESUMED + "Virtio-GPU vhost-user transport\n"):
            for route in ("host", "guest"):
                runner.suspend(guest, route)
        self.assertEqual(guest.commands, ["echo guest-answers"] * 2)

    def test_the_kernels_refused_commands_fail_the_step_with_the_first_line(self) -> None:
        for error in (KERNEL_GPU_ERROR, KERNEL_RESOURCE_ERROR):
            with self.subTest(error=error.split("response ")[1][:6]):
                guest = FakeGuest(self.directory)
                with self.route(guest, RESUMED + error + KERNEL_GPU_ERROR), self.assertRaises(runner.Failure) as raised:
                    runner.suspend(guest, "host")
                self.assertEqual(str(raised.exception),
                                 f"after the host suspend: {host_gpu_reset.FAILURE}: {error.strip()}")

    def test_the_hosts_refusal_fails_the_step_of_either_route(self) -> None:
        for name in ("host", "guest"):
            with self.subTest(route=name):
                guest = FakeGuest(self.directory)
                with self.route(guest, HOST_GPU_REFUSAL), self.assertRaises(runner.Failure) as raised:
                    runner.suspend(guest, name)
                self.assertEqual(str(raised.exception),
                                 f"after the {name} suspend: {host_gpu_reset.FAILURE}: {HOST_GPU_REFUSAL.strip()}")

    def test_lines_from_before_the_suspend_are_not_a_reset(self) -> None:
        guest = FakeGuest(self.directory)
        guest.log.write_text(KERNEL_GPU_ERROR + HOST_GPU_REFUSAL)
        with self.route(guest, RESUMED):
            runner.suspend(guest, "host")

    def test_the_named_defect_comes_before_the_silence_it_causes(self) -> None:
        guest = FakeGuest(self.directory, silent=True)
        with self.route(guest, KERNEL_GPU_ERROR), self.assertRaisesRegex(runner.Failure, host_gpu_reset.FAILURE):
            runner.suspend(guest, "host")

    def test_a_guest_that_does_not_answer_after_the_wake_fails_without_a_reset(self) -> None:
        guest = FakeGuest(self.directory, silent=True)
        with self.route(guest, RESUMED), self.assertRaisesRegex(runner.Failure, "echo guest-answers' exited 255"):
            runner.suspend(guest, "host")

    def test_a_route_that_fails_fails_the_step_untouched(self) -> None:
        guest = FakeGuest(self.directory)

        def refused(_: object) -> None:
            raise runner.Failure("host suspend: the guest replied 'busy'")

        with mock.patch.dict(runner.ROUTES, {"host": refused}), self.assertRaisesRegex(runner.Failure, "replied 'busy'"):
            runner.suspend(guest, "host")
        self.assertEqual(guest.commands, [])

    def test_the_player_and_the_recorder_suspend_through_the_step(self) -> None:
        # Both checks reach their suspend, and a reset there is the failure they report, before any check of the
        # picture or the recording.
        guest = FakeGuest(self.directory)
        with mock.patch.object(runner, "play"), mock.patch.object(runner.time, "sleep"), \
                self.route(guest, KERNEL_GPU_ERROR):
            with self.assertRaisesRegex(runner.Failure, f"after the host suspend: {host_gpu_reset.FAILURE}"):
                runner.check_player(guest, 1, "host")
            with self.assertRaisesRegex(runner.Failure, f"after the guest suspend: {host_gpu_reset.FAILURE}"):
                runner.check_recording(guest, "guest")

    def test_no_check_calls_a_route_without_the_step(self) -> None:
        source = SCRIPT.read_text()
        self.assertEqual(source.count("ROUTES[route](guest)"), 1)


class SharedReaderTests(unittest.TestCase):
    def test_the_log_is_read_from_the_mark_and_a_missing_log_is_empty(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "emulator.log"
            log.write_text("before\n")
            size = host_gpu_reset.log_size(log)
            log.write_text("before\n" + KERNEL_GPU_ERROR)
            self.assertEqual(host_gpu_reset.log_since(log, size), KERNEL_GPU_ERROR)
            self.assertEqual(host_gpu_reset.failure_since(log, size),
                             f"{host_gpu_reset.FAILURE}: {KERNEL_GPU_ERROR.strip()}")
            self.assertIsNone(host_gpu_reset.failure_since(log, log.stat().st_size))
            self.assertEqual(host_gpu_reset.log_size(Path(directory) / "gone"), 0)

    def test_a_long_line_is_cut(self) -> None:
        line = KERNEL_GPU_ERROR.strip() + " " + "x" * 400
        self.assertEqual(len(host_gpu_reset.reset_line(line)), 300)

    def test_an_error_of_another_driver_or_another_level_is_not_a_reset(self) -> None:
        for text in ("[drm] virtio_gpu_dequeue_ctrl_func: ok\n", "[drm:other_func] *ERROR* response 0x1204\n", "\n"):
            self.assertIsNone(host_gpu_reset.reset_line(text), text)


if __name__ == "__main__":
    unittest.main()
