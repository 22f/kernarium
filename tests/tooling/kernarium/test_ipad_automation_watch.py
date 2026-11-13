#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""`ipad-automation.py watch` keeps a device's UI automation host serving.

A host that ended minutes ago is replaced by a new one without the device's
approval prompt; one that ended hours ago is not. The watch therefore starts
the next host the moment the old one stops serving, and fails, once and for
good, when the device asks for approval. Every test runs the real script (its
supervisor included) against `automation_fakes.FakeDevice`, an `xcrun` and an
`xcodebuild` that are files in a scratch folder, so no call reaches a device or
Xcode; the rules about time run on a `ManualClock` that only the test moves.
The watch's waits are scaled down for the tests that run real processes; each
test names the rule it pins.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import fcntl
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
import unittest
from unittest import mock
import warnings

sys.path.insert(0, str(Path(__file__).resolve().parent))
from automation_fakes import FakeDevice  # noqa: E402

SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

# The watch's real bounds, scaled so that a test takes seconds. HEARTBEAT_STALE
# stays well above the fake host's 0.1 s beat, and a test that does not wait
# for it to pass raises it further.
SCALED = {
    "WATCH_POLL": 0.1, "WATCH_PULL_EVERY": 0.3, "HEARTBEAT_STALE": 6.0,
    "WATCH_GONE_AGE": 1.0, "WATCH_RUNNER_WAIT": 4.0, "WATCH_BUSY_LIMIT": 1000,
    "WATCH_ATTEMPTS": 4, "WATCH_BACKOFF": (0.05, 0.05, 0.05),
    "WATCH_COMMAND_WAIT": 30, "WATCH_ALIVE_FRESH": 2.0, "WATCH_END_WAIT": 20,
    "SUPERVISOR_TERM_WAIT": 5, "CHILD_TERM_WAIT": 1, "SEND_LIVENESS_EVERY": 0.5,
    "WATCH_ADOPT_GAP": 0.3, "WATCH_APPROVAL_WINDOW": 4.0,
}
SECONDS = 3 * 3600
PROMPT = (
    "2026-11-13 16:04:14.424136+0900 KernariumAutomation-Runner[11111:2222222] "
    "[Default] Failed to initialize for UI testing: Error Domain="
    'com.apple.dt.XCTest.XCTFuture Code=1000 "Timed out while enabling '
    'automation mode." UserInfo={NSLocalizedDescription=Timed out while '
    "enabling automation mode.}")


class ManualClock:
    """Time that moves only when the code under test waits or a test says so.

    A wait advances both clocks and also yields the processor for a moment, so
    that the real processes the watch looks at get to run."""

    def __init__(self) -> None:
        self.mono = 5000.0
        self.wall = 1_800_000_000.0
        self.hooks: list = []

    def monotonic(self) -> float:
        return self.mono

    def time(self) -> float:
        return self.wall

    def advance(self, seconds: float) -> None:
        self.mono += seconds
        self.wall += seconds
        if self.mono > 5000 + 100_000:
            raise RuntimeError("the code under test waits without a bound")
        for hook in list(self.hooks):
            hook(self)

    def suspend(self, seconds: float) -> None:
        """The Mac sleeps: the wall clock moves, the monotonic one does not."""
        self.wall += seconds

    def sleep(self, seconds: float) -> None:
        self.advance(seconds)
        time.sleep(0.01)

    def wait(self, event: threading.Event, seconds: float) -> bool:
        if event.is_set():
            return True
        self.sleep(seconds)
        return event.is_set()


class Running:
    """A `watch` running in a thread of the test."""

    def __init__(self, options, stop: threading.Event) -> None:
        self.stop = stop
        self.code: int | None = None
        self.thread = threading.Thread(target=self.run, args=(options,), daemon=True)
        self.thread.start()

    def run(self, options) -> None:
        self.code = automation.command_watch(options, self.stop)

    def join(self, seconds: float = 30) -> int | None:
        self.thread.join(seconds)
        return None if self.thread.is_alive() else self.code

    def end(self) -> int | None:
        self.stop.set()
        return self.join()


class WatchTestCase(unittest.TestCase):
    def setUp(self) -> None:
        # A watch that ends leaves the hosts it started running; the device's
        # cleanup ends them, after their Popen objects are gone.
        warnings.simplefilter("ignore", ResourceWarning)
        self.device = FakeDevice()
        self.addCleanup(self.device.cleanup)
        environment = mock.patch.dict(os.environ, self.device.environment)
        environment.start()
        self.addCleanup(environment.stop)
        self.messages: list[str] = []
        self.patch(automation, "log", self.messages.append)
        for name, value in SCALED.items():
            self.patch(automation, name, value)
        self.launches: list = []
        self.watches: list[Running] = []
        self.addCleanup(self.reap)

    def patch(self, target, name: str, value) -> None:
        patcher = mock.patch.object(target, name, value)
        patcher.start()
        self.addCleanup(patcher.stop)

    def tune(self, **constants) -> None:
        for name, value in constants.items():
            self.patch(automation, name, value)

    def reap(self) -> None:
        for watch in self.watches:
            watch.end()
        for launch in self.launches:
            automation.end_host({"nonce": launch.nonce}, launch.process, 2)

    def options(self, *command: str):
        return automation.parse([
            "--device", self.device.udid, "--state-dir", str(self.device.state_dir),
            "--derived-data", str(self.device.derived), *command])

    def reports(self) -> list[str]:
        return ["--device-reports-dir", str(self.device.reports),
                "--leave-device-reports"]

    def watch_options(self, *extra: str):
        return self.options("watch", "--hours", "3", "--wait", "20",
                            *self.reports(), *extra)

    def run_watch(self, *extra: str) -> Running:
        watch = Running(self.watch_options(*extra), threading.Event())
        self.watches.append(watch)
        return watch

    def start_host(self, hours: float = 1):
        """A host the way `start` makes one, serving before it returns."""
        launch = automation.launch_host(
            self.options("start", "--hours", str(hours), *self.reports()))
        self.launches.append(launch)
        self.device.wait(lambda: self.device.serving(launch.nonce), 30,
                         "the first host serves")
        return launch

    def record(self) -> dict:
        return automation.read_watch_record(self.options("status"))

    def wait_record(self, **expected) -> dict:
        def held():
            record = self.record()
            return record if all(record.get(key) == value
                                 for key, value in expected.items()) else None
        return self.device.wait(held, 30, f"the watch record says {expected}")

    def nonces(self) -> list[str]:
        return [item["nonce"] for item in self.device.invocations()]

    def watching(self, restarts: int) -> str:
        """Waits until the watch has the host that is its restart number
        `restarts`, and returns its nonce."""
        record = self.wait_record(state=automation.WATCHING, restarts=restarts)
        return record["host"]

    def progress(self, count: int = 3) -> None:
        """Waits until the watch has seen the host's heartbeat advance `count`
        more times (its record's alive_at moves each time): it has looked at
        the host that often, and whatever it did, it did by then."""
        last = [self.record().get("alive_at")]
        seen = [0]

        def moved() -> bool:
            now = self.record().get("alive_at")
            if now != last[0]:
                last[0] = now
                seen[0] += 1
            return seen[0] >= count
        self.device.wait(moved, 60, "the watch sees the heartbeat advance")

    def invoked(self, count: int) -> None:
        self.assertEqual(len(self.device.invocations()), count, self.messages)


class AdoptionTests(WatchTestCase):
    def test_a_serving_host_is_adopted_and_left_alone(self) -> None:
        # Rule: the watch never stops or restarts a host that serves.
        self.tune(HEARTBEAT_STALE=30)
        host = self.start_host()
        # Nothing but the heartbeat says which host this is, as for a host
        # that something else started.
        (self.device.state_dir / self.device.udid / "host.json").unlink()
        watch = self.run_watch()
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], host.nonce)
        self.assertEqual(record["restarts"], 0)
        self.progress()
        self.invoked(1)
        self.assertIsNone(host.process.poll())
        self.assertTrue(self.device.serving(host.nonce))
        self.assertEqual(watch.end(), 0)
        self.assertEqual(self.record()["state"], automation.STOPPED)
        # Ending the watch alone leaves the host as it was.
        self.assertIsNone(host.process.poll())
        self.assertTrue(self.device.serving(host.nonce))

    def test_the_last_beat_of_a_host_that_just_ended_is_not_a_host(self) -> None:
        # Rule: adoption needs a heartbeat that advances between two reads. The
        # last beat of a host that ended a moment ago still looks recent, and
        # adopting it would leave the device without a host until it aged out.
        self.tune(HEARTBEAT_STALE=60)
        host = self.start_host()
        self.device.control(host.nonce, "die")
        self.device.wait(lambda: host.process.poll() is not None, 15,
                         "the host ended")
        self.run_watch()
        record = self.wait_record(state=automation.WATCHING)
        self.assertNotEqual(record["host"], host.nonce)
        self.assertEqual(record["restarts"], 1)
        self.invoked(2)

    def test_a_host_that_is_still_starting_is_waited_for(self) -> None:
        # Rule: a `start` in flight is not taken over; the owner may be
        # approving it, and a second host would put a second prompt up.
        self.tune(HEARTBEAT_STALE=30)
        self.device.scenarios([{"kind": "serve", "hold": True}])
        launch = automation.launch_host(
            self.options("start", "--hours", "1", *self.reports()))
        self.launches.append(launch)
        self.device.wait(lambda: self.device.invocations()
                         and automation.recorded_supervisor(
                             automation.local_record(self.options("status"))),
                         30, "the supervisor runs the held host")
        watch = self.run_watch()
        self.device.wait(lambda: any("waiting for it" in message
                                     for message in self.messages), 30,
                         "the watch waits for the host")
        self.invoked(1)
        self.assertEqual(self.record()["state"], automation.STARTING)
        self.assertIsNone(launch.process.poll())
        self.device.control(launch.nonce, "go")
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], launch.nonce)
        self.progress()
        self.invoked(1)
        self.assertEqual(watch.end(), 0)


class ReplacementTests(WatchTestCase):
    def test_a_host_that_ends_is_replaced_with_the_watchs_options(self) -> None:
        # Rule: a host that stops serving is replaced at once, with the watch's
        # own options (here --hours 3 against the first host's 1).
        host = self.start_host(hours=1)
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        self.assertEqual(self.device.invocations()[0]["seconds"], 3600)
        self.device.control(host.nonce, "die")
        new = self.watching(restarts=1)
        invocations = self.device.invocations()
        self.assertEqual([item["nonce"] for item in invocations], [host.nonce, new])
        replacement = invocations[1]
        self.assertEqual(replacement["seconds"], SECONDS)
        supervisor = replacement["supervisor"]
        self.assertIn(f"--seconds {SECONDS}", supervisor)
        # Each collection of the device's reports gets a directory of its own
        # beneath the one configured.
        self.assertIn(f"--device-reports-dir {self.device.reports}/host-", supervisor)
        self.assertIn("--leave-device-reports", supervisor)
        self.assertIn(f"--state-dir {self.device.state_dir}", supervisor)
        self.assertIn(f"platform=iOS,id={self.device.udid}", replacement["argv"])
        self.assertTrue(self.device.serving(new))
        self.assertIn("supervisor ended", self.record()["last_end"]["reason"])
        self.device.wait(lambda: host.process.poll() is not None, 15,
                         "the first host's supervisor has ended")

    def test_the_host_the_watch_starts_is_watched_too(self) -> None:
        # Rule: with no host serving the watch starts one, and watches it.
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        second = self.watching(restarts=2)
        self.assertNotEqual(first, second)
        self.assertEqual(self.nonces(), [first, second])

    def test_a_runner_started_again_under_a_live_xcodebuild_is_replaced(self) -> None:
        # Rule: xcodebuild starting the runner again (the old runner died; the
        # new one serves nothing) is read from its log. The watch waits the
        # approval window for what the new runner says (nothing here), then
        # replaces the host and ends the live xcodebuild. The heartbeat's own
        # bound is far away, so only the log signal can pass.
        self.tune(HEARTBEAT_STALE=60)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "relaunch")
        second = self.watching(restarts=2)
        old = self.device.invocations()[0]
        self.device.wait(lambda: not self.device.alive(old["pid"])
                         and not self.device.alive(old["ppid"]), 15,
                         "the old xcodebuild and its supervisor end")
        self.assertIn("started the runner again",
                      self.record()["last_end"]["reason"])
        self.assertTrue(self.device.serving(second))

    def test_a_silent_host_is_replaced_when_its_heartbeat_goes_stale(self) -> None:
        # Rule: with the processes alive and nothing logged, a host whose
        # heartbeat stops is replaced after HEARTBEAT_STALE, and ended.
        self.tune(HEARTBEAT_STALE=2.0)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "silent")
        began = time.monotonic()
        second = self.watching(restarts=2)
        self.assertGreater(time.monotonic() - began, 1.0)
        old = self.device.invocations()[0]
        self.device.wait(lambda: not self.device.alive(old["pid"]), 15,
                         "the silent xcodebuild is ended")
        self.assertIn("heartbeat", self.record()["last_end"]["reason"])
        self.assertTrue(self.device.serving(second))

    def test_a_host_whose_supervisor_alone_ended_still_serves(self) -> None:
        # Rule: the processes only hurry the verdict. A runner that keeps
        # beating serves, whatever happened to its supervisor.
        self.tune(HEARTBEAT_STALE=30)
        self.run_watch()
        first = self.watching(restarts=1)
        supervisor = self.device.invocations()[0]["ppid"]
        os.kill(supervisor, 9)
        self.device.wait(lambda: not self.device.alive(supervisor), 10,
                         "the supervisor ends")
        # Each read the watch makes from here on is one that the supervisor's
        # end makes quick, and none of them may end the host.
        self.progress(10)
        self.assertTrue(self.device.serving(first))
        self.assertEqual(self.record()["restarts"], 1)
        self.invoked(1)

    def test_a_busy_host_is_not_a_dead_one(self) -> None:
        # Rule: a command that runs long leaves the heartbeat fresh, because
        # the host beats from a queue no command blocks, and the host is left
        # alone; only a command past WATCH_BUSY_LIMIT is a hung host.
        self.tune(WATCH_BUSY_LIMIT=1000, HEARTBEAT_STALE=2.0)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "busy")
        self.device.wait(lambda: (self.device.heartbeat() or {}).get("command"),
                         10, "the heartbeat shows the command")
        self.progress(10)
        self.assertTrue(self.device.serving(first))
        self.invoked(1)

    def test_a_host_stuck_in_one_command_is_replaced(self) -> None:
        self.tune(WATCH_BUSY_LIMIT=1.5)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "busy")
        second = self.watching(restarts=2)
        self.assertNotEqual(first, second)
        self.assertIn("hung", self.record()["last_end"]["reason"])

    def test_a_host_that_reaches_its_lifetime_is_replaced(self) -> None:
        # Rule: a host whose own lifetime (--hours) ended is replaced too.
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "lifetime")
        self.watching(restarts=2)
        self.assertIn("lifetime", self.record()["last_end"]["reason"])

    def test_a_failed_read_does_not_end_a_serving_host(self) -> None:
        # Rule (R3): a heartbeat that cannot be read is not a heartbeat that
        # stopped. The fake host beats on while every read fails for far longer
        # than HEARTBEAT_STALE, and the watch leaves it alone.
        self.tune(HEARTBEAT_STALE=1.0, WATCH_GONE_AGE=0.5)
        host = self.start_host()
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        real = automation.host_state
        failures = []
        failing = threading.Event()
        failing.set()

        def host_state(options, timeout=60):
            if failing.is_set() and threading.current_thread().name != "MainThread":
                failures.append(time.monotonic())
                return None
            return real(options, timeout)
        self.patch(automation, "host_state", host_state)
        self.device.wait(lambda: len(failures) >= 1 and
                         failures[-1] - failures[0] > 3 * automation.HEARTBEAT_STALE,
                         60, "reads fail for longer than the heartbeat bound")
        failing.clear()
        self.progress()
        self.invoked(1)
        self.assertTrue(self.device.serving(host.nonce))
        self.assertEqual(self.record()["restarts"], 0)


class FailureTests(WatchTestCase):
    def test_the_approval_prompt_fails_the_watch_once(self) -> None:
        # Rule: when the device asks for approval the watch ends that attempt,
        # records it, exits non-zero and does not try again. The signal is
        # XCTest's own line; a new watch starts nothing until a `start` served.
        self.tune(HEARTBEAT_STALE=3.0)
        self.device.scenarios([{"kind": "serve"}, {"kind": "approval"}])
        watch = self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.assertEqual(watch.join(), automation.EXIT_NEEDS_APPROVAL)
        invocations = self.device.invocations()
        self.assertEqual(len(invocations), 2)
        record = self.record()
        self.assertEqual(record["state"], automation.NEEDS_APPROVAL)
        self.assertIn("start", record["detail"])
        self.assertGreater(record["since"], 0)
        self.assertFalse(self.device.alive(invocations[1]["ppid"]))
        status = automation.watch_summary(self.options("status"))
        self.assertFalse(status["running"])
        self.assertEqual(status["meaning"], "needs approval at the device")
        self.assertIn("observed", status["approval"])
        # A second watch does not start a host either.
        self.assertEqual(self.run_watch().join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)
        self.assertEqual(self.record()["state"], automation.NEEDS_APPROVAL)
        # A `start` the owner approved ends the refusal. The first host's last
        # heartbeat still counts as serving for HEARTBEAT_STALE.
        self.device.wait(lambda: self.device.silent_for() > automation.HEARTBEAT_STALE,
                         30, "the first host's heartbeat goes stale")
        self.device.scenarios([{"kind": "serve"}])
        self.assertEqual(automation.command_start(self.options(
            "start", "--hours", "1", *self.reports())), 0)
        third = self.device.invocations()[-1]
        self.assertEqual(self.record()["state"], automation.STOPPED)
        self.assertNotIn("approval", automation.watch_summary(self.options("status")))
        again = self.run_watch()
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], third["nonce"])
        self.assertEqual(again.end(), 0)

    def test_a_cancelled_authentication_fails_the_watch_like_the_timeout(self) -> None:
        # Rule (R1/R2): the device's own failure when the owner's
        # authentication for UI automation was cancelled (by the system, here)
        # is the approval signal too: exit 4, no retry, the latch kept.
        self.tune(HEARTBEAT_STALE=3.0)
        self.device.scenarios([{"kind": "serve"},
                               {"kind": "approval", "signal": "authentication"}])
        watch = self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.assertEqual(watch.join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)
        self.assertEqual(self.record()["state"], automation.NEEDS_APPROVAL)
        latch = automation.ApprovalLatch(self.options("status")).read()
        self.assertEqual(latch["kind"], "observed")
        self.assertIn("LocalAuthentication", latch["detail"])
        self.assertEqual(self.run_watch().join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)

    def test_start_says_a_cancelled_authentication_was_not_approved(self) -> None:
        self.device.scenarios([{"kind": "approval", "signal": "authentication"}])
        status = automation.command_start(self.options(
            "start", "--hours", "1", "--wait", "30", *self.reports()))
        self.assertEqual(status, automation.EXIT_NO_HOST)
        self.assertTrue(any("approval it asked for was not given" in message
                            for message in self.messages), self.messages)

    def test_a_relaunched_runner_the_device_would_not_approve_ends_the_watch(self) -> None:
        # Rule (R1): a host served, its runner died, and xcodebuild started the
        # runner again; that runner asks the device for its approval and the
        # failure arrives within a minute. The watch waits for it, instead of
        # ending the runner mid-prompt and starting a host that would put the
        # same prompt up again: it exits 4, with the latch, having started
        # nothing.
        for failure in ("authentication", "authentication-localized", "timeout"):
            with self.subTest(failure):
                self.setUp()
                self.device.scenarios([{"kind": "serve"}])
                watch = self.run_watch()
                first = self.watching(restarts=1)
                self.device.control(first, f"relaunch-{failure}")
                self.assertEqual(watch.join(), automation.EXIT_NEEDS_APPROVAL,
                                 self.messages)
                self.invoked(1)
                self.assertEqual(self.record()["state"], automation.NEEDS_APPROVAL)
                latch = automation.ApprovalLatch(self.options("status")).read()
                self.assertEqual(latch["kind"], "observed")
                self.assertEqual(self.record()["restarts"], 1)
                self.assertEqual(self.run_watch().join(), automation.EXIT_NEEDS_APPROVAL)
                self.invoked(1)

    def test_a_relaunched_runner_that_asks_nothing_is_replaced_at_once(self) -> None:
        # The runner started again needs no approval (the device still holds the
        # last one) and ends: nothing was asked, so the host is replaced.
        self.tune(WATCH_APPROVAL_WINDOW=60)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "relaunch-quiet")
        second = self.watching(restarts=2)
        self.assertNotEqual(first, second)
        self.assertIsNone(automation.ApprovalLatch(self.options("status")).read())

    def test_a_runner_that_never_serves_is_not_the_approval_prompt(self) -> None:
        # Rule: a runner that is up and silent is an ordinary failure, retried
        # a bounded number of times: only XCTest's line means approval.
        self.tune(WATCH_ATTEMPTS=2, WATCH_RUNNER_WAIT=1.0)
        self.device.scenarios([{"kind": "up-silent"}])
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.invoked(2)
        record = self.record()
        self.assertEqual(record["state"], automation.START_FAILED)
        self.assertIn("did not serve", record["detail"])
        self.assertIsNone(automation.ApprovalLatch(self.options("status")).read())
        for item in self.device.invocations():
            self.assertFalse(self.device.alive(item["ppid"]))

    def test_a_missing_device_is_retried_a_bounded_number_of_times(self) -> None:
        # Rule: other failures get a few attempts with a short backoff, then a
        # state of their own; never an endless loop.
        self.device.scenarios([{"kind": "exit", "status": 70}])
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.invoked(automation.WATCH_ATTEMPTS)
        record = self.record()
        self.assertEqual(record["state"], automation.START_FAILED)
        self.assertIn("status 70", record["detail"])

    def test_a_report_store_without_free_slots_starts_nothing(self) -> None:
        # Rule: the device's report store is made room for before anything
        # starts, so nothing starts while it is short; the refusal is retried
        # within the same bound.
        calls = []
        self.patch(automation, "check_report_slots",
                   lambda options, processes: calls.append(1) and False)
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(len(calls), automation.WATCH_ATTEMPTS)
        self.assertEqual(self.device.invocations(), [])
        record = self.record()
        self.assertEqual(record["state"], automation.START_FAILED)
        self.assertIn("report store", record["detail"])

    def test_no_built_host_ends_the_watch_at_once(self) -> None:
        # Rule: a failure that trying again cannot change is not retried.
        for run in (self.device.derived / "Build/Products").glob("*.xctestrun"):
            run.unlink()
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(self.record()["state"], automation.CANNOT_START)
        self.assertIn("run `build` first", self.record()["detail"])
        self.assertEqual(self.device.invocations(), [])

    def test_too_many_restarts_in_an_hour_end_the_watch(self) -> None:
        # Rule: more than --max-restarts replacements in an hour end the watch
        # with a state of its own, and a new watch inside that hour does too.
        self.tune(HEARTBEAT_STALE=2.0)
        watch = self.run_watch("--max-restarts", "2")
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        second = self.watching(restarts=2)
        self.device.control(second, "die")
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        record = self.record()
        self.assertEqual(record["state"], automation.RESTART_STORM)
        self.assertIn("bound is 2", record["detail"])
        self.invoked(2)
        again = self.run_watch("--max-restarts", "2")
        self.assertEqual(again.join(), automation.EXIT_WATCH_GAVE_UP)
        self.invoked(2)
        self.assertEqual(self.record()["state"], automation.RESTART_STORM)

    def test_restarts_older_than_an_hour_do_not_count(self) -> None:
        # Rule: only replacements inside the window count. Two old ones, with
        # a bound of two, would end the watch before it starts a host.
        directory = self.device.state_dir / self.device.udid
        directory.mkdir(parents=True)
        old = time.time() - 2 * automation.WATCH_STORM_WINDOW
        (directory / "watch.json").write_text(json.dumps(
            {"state": automation.STOPPED, "restart_times": [old, old + 1]}))
        watch = self.run_watch("--max-restarts", "2")
        self.watching(restarts=1)
        self.assertEqual(watch.end(), 0)


class OneWatchTests(WatchTestCase):
    def test_a_second_watch_for_the_device_refuses(self) -> None:
        # Rule: one watch per device and state directory.
        self.tune(HEARTBEAT_STALE=30)
        host = self.start_host()
        first = self.run_watch()
        self.wait_record(state=automation.WATCHING)
        status = automation.watch_summary(self.options("status"))
        self.assertTrue(status["running"])
        self.assertEqual(status["pid"], os.getpid())
        second = Running(self.watch_options(), threading.Event())
        self.watches.append(second)
        self.assertEqual(second.join(10), automation.EXIT_WATCH_LOCKED)
        self.assertEqual(self.record()["state"], automation.WATCHING)
        self.assertEqual(first.end(), 0)
        self.assertFalse(automation.watch_summary(self.options("status"))["running"])
        third = self.run_watch()
        self.assertEqual(self.wait_record(state=automation.WATCHING)["host"], host.nonce)
        self.assertEqual(third.end(), 0)


class CommandTests(WatchTestCase):
    def ping(self, results: list, timeout: int = 20) -> threading.Thread:
        def send() -> None:
            try:
                results.append(automation.send(
                    self.options("ping"), {"op": "ping"}, timeout))
            except automation.NoHost as error:
                results.append(error)
        thread = threading.Thread(target=send, daemon=True)
        thread.start()
        return thread

    def test_a_command_waits_through_a_replacement(self) -> None:
        # Rule: a command sent while the watch is starting a host waits for it
        # and is answered by it.
        self.device.scenarios([{"kind": "serve"}, {"kind": "serve", "hold": True}])
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.wait_record(state=automation.RESTARTING)
        self.device.wait(lambda: len(self.device.invocations()) == 2, 15,
                         "the replacement starts")
        results: list = []
        thread = self.ping(results)
        self.device.wait(lambda: any("its watch is starting" in message
                                     for message in self.messages), 15,
                         "the command says it waits")
        self.assertEqual(results, [])
        second = self.device.invocations()[1]["nonce"]
        self.device.control(second, "go")
        thread.join(30)
        self.assertEqual(len(results), 1, self.messages)
        self.assertEqual(results[0]["nonce"], second)

    def test_a_command_waits_for_a_host_that_stopped_beating(self) -> None:
        # Rule: the watch may not have noticed a host that stopped beating
        # yet; a command is sent only while the watch has seen the heartbeat
        # advance lately, and is answered by the successor.
        self.tune(HEARTBEAT_STALE=3.0, WATCH_ALIVE_FRESH=1.0)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "silent")
        self.device.wait(lambda: self.device.silent_for() > 1.2, 10,
                         "the host stops beating")
        results: list = []
        self.ping(results).join(30)
        self.assertEqual(len(results), 1, self.messages)
        second = self.device.invocations()[-1]["nonce"]
        self.assertNotEqual(second, first)
        self.assertEqual(results[0]["nonce"], second)

    def test_a_command_is_not_held_back_by_the_devices_clock(self) -> None:
        # Rule (R3): freshness is the watch's own observation, in the Mac's
        # time. A device whose clock is hours off beats as it always did, and a
        # command is sent at once.
        self.tune(HEARTBEAT_STALE=30)
        host = self.start_host()
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        self.progress()
        state = self.device.heartbeat()
        self.assertIsNotNone(state)
        real = automation.host_state
        self.patch(automation, "host_state", lambda options, timeout=60: dict(
            real(options, timeout) or {}, time=time.time() + 7200))
        results: list = []
        self.ping(results).join(30)
        self.assertEqual(len(results), 1, self.messages)
        self.assertEqual(results[0]["nonce"], host.nonce)
        self.assertFalse(any("waiting up to" in message for message in self.messages))

    def test_a_command_still_fails_without_a_watch(self) -> None:
        # Rule: with no watch running, a command to a host that ended fails
        # with status 3 as before.
        host = self.start_host()
        self.device.control(host.nonce, "die")
        self.tune(HEARTBEAT_STALE=1.5)
        self.device.wait(lambda: not self.device.serving(), 10, "the host ended")
        self.assertEqual(automation.main([
            "--device", self.device.udid, "--state-dir", str(self.device.state_dir),
            "--timeout", "5", "ping"]), automation.EXIT_NO_HOST)
        self.assertNotIn("replacing", " ".join(self.messages))

    def test_a_command_gives_up_when_the_replacement_is_not_made(self) -> None:
        # Rule: the wait is bounded. The replacement never serves here.
        self.tune(WATCH_COMMAND_WAIT=2, WATCH_RUNNER_WAIT=30)
        self.device.scenarios([{"kind": "serve"}, {"kind": "up-silent"}])
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.wait_record(state=automation.RESTARTING)
        self.device.wait(lambda: len(self.device.invocations()) == 2, 15,
                         "the replacement starts")
        results: list = []
        self.ping(results).join(20)
        self.assertEqual(len(results), 1)
        self.assertIsInstance(results[0], automation.NoHost)
        self.assertIn("still being replaced", str(results[0]))

    def test_a_command_lost_to_a_replacement_gets_the_no_host_result(self) -> None:
        # Rule (D): a command pushed to a host that has just ended is not
        # replayed to its successor, which clears the inbox: the sender is told
        # the host was replaced, and does not wait out its whole timeout. The
        # dead host's last beat is made to count for ever, so that only the
        # replacement can tell the sender; the wait before sending is made to
        # pass, as when the watch has not yet noticed.
        self.tune(HEARTBEAT_STALE=3600, SEND_LIVENESS_EVERY=2.0)
        self.patch(automation, "wait_for_replacement", lambda options, clock=None: None)
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.device.wait(lambda: not self.device.serving(first), 10,
                         "the host ended")
        results: list = []
        self.ping(results, timeout=3).join(60)
        self.assertEqual(len(results), 1, self.messages)
        self.assertIsInstance(results[0], automation.NoHost)
        self.assertIn("replaced", str(results[0]))
        self.assertNotIn("no result", str(results[0]))
        self.assertEqual(self.device.heartbeat()["served"], 0)


class ClockBoundTests(unittest.TestCase):
    """What the watch does with time, run on a clock the test moves."""

    def setUp(self) -> None:
        self.scratch = __import__("tempfile").TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.clock = ManualClock()
        self.messages: list[str] = []
        for name, value in (("log", self.messages.append),):
            patcher = mock.patch.object(automation, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def options(self, *command: str):
        return automation.parse(["--device", "00000000-0000000000000000",
                                 "--state-dir", self.scratch.name, *command])

    def record(self, **fields) -> None:
        directory = Path(self.scratch.name) / "00000000-0000000000000000"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "watch.json").write_text(json.dumps(fields))

    def test_the_command_wait_is_a_hard_bound_with_no_device_read(self) -> None:
        # Rule (R9): a command waits at most WATCH_COMMAND_WAIT for a
        # replacement, however slow the device is: the wait makes no device
        # read at all, so no read can carry it past the bound.
        options = self.options("ping")
        self.record(state=automation.STARTING, alive_at=0)

        def never(*arguments, **keywords):
            raise AssertionError("the wait read the device")
        with mock.patch.object(automation, "watcher_pid", lambda options: 1), \
                mock.patch.object(automation, "host_state", never), \
                mock.patch.object(automation, "pull", never):
            began = self.clock.monotonic()
            with self.assertRaises(automation.NoHost):
                automation.wait_for_replacement(options, self.clock)
        waited = self.clock.monotonic() - began
        self.assertGreaterEqual(waited, automation.WATCH_COMMAND_WAIT)
        self.assertLessEqual(waited, automation.WATCH_COMMAND_WAIT + 2)

    def test_the_reads_of_a_command_never_outlast_its_own_wait(self) -> None:
        # Rule (R9): each device read of `send` is given what is left of the
        # command's own wait (at most 60 s), not a flat 60 s that could carry it
        # past its deadline.
        options = self.options("ping")
        timeouts = []

        def read(options, *arguments, **keywords):
            timeouts.append(arguments[-1] if arguments else keywords["timeout"])
            return None

        def state(options, timeout=60):
            timeouts.append(timeout)
            return {"nonce": "n" * 32, "time": time.time()}
        with mock.patch.object(automation, "wait_for_replacement",
                               lambda options, clock=None: None), \
                mock.patch.object(automation, "host_state", state), \
                mock.patch.object(automation, "push", lambda *a, **k: None), \
                mock.patch.object(automation, "pull", lambda options, source, destination, timeout=60: (
                    timeouts.append(timeout), False)[1]):
            with self.assertRaises(automation.NoHost):
                automation.send(options, {"op": "ping"}, 20, self.clock)
        self.assertTrue(timeouts)
        self.assertLessEqual(max(timeouts), 50)
        # Near the end of the wait, a read is given only what remains of it.
        self.assertLessEqual(min(timeouts), 2)

    def test_a_command_goes_once_the_watch_has_seen_the_host_beat(self) -> None:
        options = self.options("ping")
        self.record(state=automation.WATCHING, alive_at=0)

        def beat(clock) -> None:
            if clock.monotonic() - 5000 >= 10:
                self.record(state=automation.WATCHING, alive_at=clock.time())
        self.clock.hooks.append(beat)
        with mock.patch.object(automation, "watcher_pid", lambda options: 1):
            automation.wait_for_replacement(options, self.clock)
        self.assertGreaterEqual(self.clock.monotonic() - 5000, 10)
        self.assertLess(self.clock.monotonic() - 5000, 14)

    def test_a_start_that_waits_longer_than_the_watch_is_waited_for(self) -> None:
        # Rule (R4): the wait the starting `start` recorded is the one the
        # watch honours, not the watch's own --wait.
        options = self.options("watch", "--wait", "10")
        directory = Path(self.scratch.name) / "00000000-0000000000000000"
        directory.mkdir(parents=True)
        (directory / "host.json").write_text(json.dumps({
            "supervisor": 1, "nonce": "n" * 32, "log": str(directory / "x.log"),
            "started": self.clock.time() - 400, "wait": 600}))
        watch = automation.Watch(options, threading.Event(), self.clock)
        with mock.patch.object(automation, "host_state", lambda *a, **k: None), \
                mock.patch.object(automation, "recorded_supervisor", lambda r: 1):
            before = self.clock.time()
            watch.settle_last_launch()
        # 600 s of its own, 400 elapsed: about 200 s remain, not the 14 of
        # the watch's own bounds.
        self.assertGreater(self.clock.time() - before, 150)


class ObserverTests(unittest.TestCase):
    """When a host is dead, from what a scripted device shows, on a clock the
    test moves (the real WATCH_ constants; no host process runs)."""

    UDID = "00000000-0000000000000000"

    def setUp(self) -> None:
        self.scratch = __import__("tempfile").TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.clock = ManualClock()
        self.device_clock = 40_000.0   # the device's own clock, hours off the Mac's
        self.beating = True
        self.readable = True
        self.command: float | None = None
        self.nonce = "a" * 32
        self.reads = 0
        self.other: dict | None = None
        self.gone = False
        for name, value in (("host_state", self.read), ("log", lambda message: None)):
            patcher = mock.patch.object(automation, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.options = automation.parse(
            ["--device", self.UDID, "--state-dir", self.scratch.name, "status"])
        process = mock.Mock()
        process.poll = lambda: 1 if self.gone else None
        self.observer = automation.HostObserver(
            self.options, self.nonce, self.state(), process, None, self.clock)

    def state(self) -> dict:
        state = {"nonce": self.nonce, "time": self.device_clock, "started": 39_000.0,
                 "deadline": 99_999.0, "served": 0, "finished": False}
        if self.command is not None:
            state.update(command="wait", commandStarted=self.command)
        return state

    def read(self, options, timeout=60):
        self.reads += 1
        if not self.readable:
            return None
        if self.other is not None:
            return self.other
        return self.state()

    def run_for(self, seconds: float, step: float = 2.0):
        """Runs the watch's loop for `seconds` of the clock; the first verdict
        and when it came, or (None, seconds)."""
        began = self.clock.monotonic()
        while self.clock.monotonic() - began < seconds:
            if self.beating:
                self.device_clock += step
            reason = self.observer.check()
            if reason is not None:
                return reason, self.clock.monotonic() - began
            self.clock.advance(step)
        return None, seconds

    def test_a_beating_host_is_alive_whatever_the_devices_clock_says(self) -> None:
        # The device's clock is hours off the Mac's: only progress counts.
        self.assertEqual(self.run_for(600), (None, 600))

    def test_a_host_that_stops_beating_is_dead_after_the_heartbeat_bound(self) -> None:
        self.run_for(30)
        self.beating = False
        reason, after = self.run_for(120)
        self.assertIn("has not advanced", reason)
        self.assertGreaterEqual(after, automation.HEARTBEAT_STALE - 2)
        self.assertLessEqual(after, automation.HEARTBEAT_STALE
                             + 2 * (automation.WATCH_PULL_EVERY + 2))

    def test_a_host_whose_supervisor_ended_is_dead_after_a_few_seconds(self) -> None:
        self.run_for(30)
        self.beating = False
        self.gone = True
        reason, after = self.run_for(60)
        self.assertIn("supervisor ended", reason)
        self.assertLessEqual(after, automation.WATCH_GONE_AGE + 6)

    def test_a_beating_host_whose_supervisor_ended_is_alive(self) -> None:
        self.gone = True
        self.assertEqual(self.run_for(300), (None, 300))

    def test_reads_that_fail_are_not_a_death(self) -> None:
        # Rule (R3): the host beats on while no read works, for far longer
        # than the bound; the watch says nothing, and says how long it is blind.
        self.run_for(30)
        self.readable = False
        self.assertEqual(self.run_for(1000), (None, 1000))
        self.assertGreater(self.observer.blind_for(), 900)
        self.readable = True
        self.assertEqual(self.run_for(60), (None, 60))
        self.assertEqual(self.observer.blind_for(), 0)

    def test_a_host_that_died_while_unreadable_is_dead_when_read_again(self) -> None:
        # A read that works and shows the beat it showed long ago is evidence.
        self.run_for(30)
        self.readable = False
        self.beating = False
        self.run_for(100)
        self.readable = True
        reason, _ = self.run_for(60)
        self.assertIn("has not advanced", reason)

    def test_a_sleep_of_the_mac_is_not_a_death(self) -> None:
        # Rule (R3): the monotonic clock stops while the Mac sleeps, so the
        # last read looks young to it and the wall clock moves hours. A beating
        # host is not declared dead on waking; and a read is the first thing
        # made on waking.
        self.run_for(30)
        reads = self.reads
        self.clock.suspend(6 * 3600)
        self.device_clock += 6 * 3600
        self.assertIsNone(self.observer.check())
        self.assertEqual(self.reads, reads + 1)
        self.assertEqual(self.run_for(60), (None, 60))

    def test_a_host_that_did_not_survive_the_sleep_is_dead_after_the_bound(self) -> None:
        # The silence seen before the sleep (10 s of it here) does not count
        # towards the bound: the host is measured again from the wake.
        self.run_for(30)
        self.beating = False
        self.run_for(10)
        self.clock.suspend(6 * 3600)
        reason, after = self.run_for(120)
        self.assertIn("has not advanced", reason)
        self.assertGreaterEqual(after, automation.HEARTBEAT_STALE - 2)

    def test_a_command_longer_than_the_busy_limit_is_a_hung_host(self) -> None:
        self.run_for(10)
        self.command = self.device_clock
        reason, after = self.run_for(automation.WATCH_BUSY_LIMIT + 100, 10)
        self.assertIn("hung", reason)
        self.assertGreaterEqual(after, automation.WATCH_BUSY_LIMIT - 10)

    def test_a_command_gets_its_own_longer_deadline(self) -> None:
        # Rule (C): a sender waiting 4000 s for its command, which the host may
        # run that long, is not waiting on a hung host at 1800 s.
        self.run_for(10)
        self.command = self.device_clock
        with automation.Inflight(self.options, "mine", 4000):
            reason, after = self.run_for(automation.WATCH_BUSY_LIMIT + 1000, 10)
            self.assertIsNone(reason)
            reason, after = self.run_for(2000, 10)
        self.assertIn("hung", reason)

    def test_a_sender_that_is_gone_does_not_extend_the_deadline(self) -> None:
        directory = Path(self.scratch.name) / self.UDID / "inflight"
        directory.mkdir(parents=True)
        (directory / "gone.json").write_text(json.dumps(
            {"pid": os.getpid(), "start": "Mon Jan  1 00:00:00 2001", "limit": 9000}))
        (directory / "dead.json").write_text(json.dumps(
            {"pid": 2 ** 22 + 12345, "start": "x", "limit": 9000}))
        self.assertEqual(automation.inflight_limit(self.options), 0)
        with automation.Inflight(self.options, "live", 4000):
            self.assertEqual(automation.inflight_limit(self.options), 4000)
        self.assertEqual(automation.inflight_limit(self.options), 0)

    def test_another_host_is_followed_only_once_it_advances(self) -> None:
        # What an earlier host left behind does not move; a new host does.
        other = {"nonce": "b" * 32, "time": 100.0, "started": 90.0,
                 "deadline": 99_999.0, "served": 0, "finished": False}
        self.other = other
        self.run_for(10)
        self.assertEqual(self.observer.nonce, "a" * 32)
        self.other = dict(other, time=104.0)
        self.run_for(6)
        self.assertEqual(self.observer.nonce, "b" * 32)


class SettleTests(WatchTestCase):
    def test_an_approval_line_that_arrives_during_a_slow_read_is_not_lost(self) -> None:
        # Rule (R1): a read that takes its whole allowed time carries the
        # attempt past the runner's deadline, and XCTest's line arrives in it;
        # the attempt rereads the log before it calls the failure retryable.
        clock = ManualClock()
        self.device.scenarios([{"kind": "up-silent"}])
        real = automation.host_state
        injected = []

        def slow(options, timeout=60):
            record = automation.local_record(options)
            log = Path(record.get("log", "/nonexistent"))
            if (log.exists() and automation.RUNNER_UP_LINE in log.read_text()
                    and not injected):
                injected.append(1)
                with open(log, "a") as handle:
                    handle.write(PROMPT + "\n")
                clock.advance(100)
            return real(options, timeout)
        self.patch(automation, "host_state", slow)
        outcome = automation.Watch(self.watch_options(), threading.Event(),
                                   clock).attempt(False)
        self.assertEqual(outcome[0], "approval", outcome)
        latch = automation.ApprovalLatch(self.options("status")).read()
        self.assertEqual(latch["kind"], "observed")

    def test_an_approval_line_that_arrives_as_the_host_is_ended_is_not_lost(self) -> None:
        # Rule (R1): the host's last words come while it is ended; the attempt
        # that ended it rereads the log after.
        clock = ManualClock()
        self.device.scenarios([{"kind": "up-silent"}])
        real_end = automation.end_host

        def end(record, process=None, wait=None):
            with open(record["log"], "a") as handle:
                handle.write(PROMPT + "\n")
            return real_end(record, process, 2)
        self.patch(automation, "end_host", end)
        self.tune(WATCH_RUNNER_WAIT=1.0)
        outcome = automation.Watch(self.watch_options(), threading.Event(),
                                   clock).attempt(False)
        self.assertEqual(outcome[0], "approval", outcome)

    def test_a_previous_attempts_approval_survives_a_watch_that_ended_mid_attempt(self) -> None:
        # Rule (R1): a watch killed during an attempt leaves its state
        # "starting"; that attempt's own log already says the device asked. The
        # next watch reads it before it launches anything.
        self.device.scenarios([{"kind": "approval", "after": 0.1}])
        launch = automation.launch_host(self.options(
            "start", "--hours", "1", *self.reports()), record=lambda nonce, path: (
            automation.WatchRecord(self.options("status")).update(
                state=automation.STARTING, pending={
                    "nonce": nonce, "log": str(path), "time": time.time(),
                    "replacement": True})))
        self.launches.append(launch)
        self.device.wait(lambda: automation.APPROVAL_TIMEOUT_LINE in
                         launch.log_path.read_text(), 30, "the host's log says it")
        self.device.wait(lambda: launch.process.poll() is not None, 30,
                         "the supervisor ended")
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(1)
        latch = automation.ApprovalLatch(self.options("status")).read()
        self.assertEqual(latch["kind"], "observed")

    def test_a_host_ended_before_it_could_say_whether_the_device_asked_counts(self) -> None:
        # Rule (R1): a watch killed while its runner was up and silent, and its
        # supervisor with it, left no line and no exit status. The device may
        # be asking: the next watch does not start another host.
        self.tune(WATCH_APPROVAL_WINDOW=60)
        self.device.scenarios([{"kind": "up-silent"}])
        launch = automation.launch_host(self.options(
            "start", "--hours", "1", "--wait", "30", *self.reports()))
        self.launches.append(launch)
        self.device.wait(lambda: automation.RUNNER_UP_LINE in
                         launch.log_path.read_text(), 30, "the runner is up")
        os.killpg(launch.process.pid, signal.SIGKILL)
        self.device.wait(lambda: launch.process.poll() is not None, 10,
                         "the supervisor ended")
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(1)
        latch = automation.ApprovalLatch(self.options("status")).read()
        self.assertEqual(latch["kind"], "possible")

    def stopped_attempt(self, wait_for_runner: bool):
        """An attempt whose watch is stopped, by the first read of the host's
        heartbeat that has the runner's line in the log behind it (or by the
        first read at all), as `stop --force` would end it."""
        stop = threading.Event()
        watch = automation.Watch(self.watch_options(), stop, ManualClock())

        def read(options, timeout=60):
            log = Path(automation.local_record(options).get("log", "/nonexistent"))
            if not wait_for_runner or (log.exists() and
                                       automation.RUNNER_UP_LINE in log.read_text()):
                stop.set()
            return None
        self.patch(automation, "host_state", read)
        return watch.attempt(False)

    def test_a_host_stopped_in_the_approval_window_counts(self) -> None:
        # Rule (R1): `stop` during an attempt ends the producer before it could
        # print; once the runner was up that may have been the prompt.
        self.device.scenarios([{"kind": "up-silent"}])
        self.assertEqual(self.stopped_attempt(True), ("stopped",))
        latch = automation.ApprovalLatch(self.options("status")).read()
        self.assertEqual(latch["kind"], "possible")

    def test_a_host_stopped_before_its_runner_came_up_does_not(self) -> None:
        self.device.scenarios([{"kind": "no-runner"}])
        self.assertEqual(self.stopped_attempt(False), ("stopped",))
        self.assertIsNone(automation.ApprovalLatch(self.options("status")).read())

    def test_a_pending_replacement_is_waited_for_not_taken_for_a_ghost(self) -> None:
        # Rule (R1): the host that ended a moment ago still has a recent last
        # beat; a replacement that a watch started and that ended is still
        # coming up. The next watch waits for that replacement, and starts no
        # third host and ends none.
        self.tune(HEARTBEAT_STALE=60)
        old = self.start_host()
        self.device.scenarios([{"kind": "serve", "hold": True}])
        self.device.control(old.nonce, "die")
        self.device.wait(lambda: old.process.poll() is not None, 15,
                         "the old host ended")
        pending = automation.launch_host(self.options(
            "start", "--hours", "1", *self.reports()), record=lambda nonce, path: (
            automation.WatchRecord(self.options("status")).update(
                state=automation.RESTARTING, pending={
                    "nonce": nonce, "log": str(path), "time": time.time(),
                    "replacement": True})))
        self.launches.append(pending)
        self.device.wait(lambda: len(self.device.invocations()) == 2, 15,
                         "the replacement starts")
        watch = self.run_watch()
        self.device.wait(lambda: any("waiting for it" in message
                                     for message in self.messages), 30,
                         "the watch waits for the replacement")
        self.invoked(2)
        self.assertIsNone(pending.process.poll())
        self.device.control(pending.nonce, "go")
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], pending.nonce)
        self.invoked(2)
        self.assertEqual(watch.end(), 0)


class ApprovalLatchTests(WatchTestCase):
    def fail_for_approval(self) -> None:
        self.tune(HEARTBEAT_STALE=2.0)
        self.device.scenarios([{"kind": "serve"}, {"kind": "approval"}])
        watch = self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.assertEqual(watch.join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)

    def test_the_refusal_survives_a_watch_that_is_stopped(self) -> None:
        # Rule (R2): a watch that starts and is stopped at once, before it has
        # read the device, must not clear the refusal for the next one.
        self.fail_for_approval()
        stopped = threading.Event()
        stopped.set()
        again = Running(self.watch_options(), stopped)
        self.watches.append(again)
        self.assertEqual(again.join(), 0)
        self.assertEqual(self.record()["state"], automation.STOPPED)
        third = self.run_watch()
        self.assertEqual(third.join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)

    def test_the_refusal_survives_a_watch_that_fails(self) -> None:
        # Rule (R2): a watch that raises ends in the state "error".
        self.fail_for_approval()
        with mock.patch.object(automation, "host_state",
                               side_effect=RuntimeError("boom")):
            self.assertEqual(self.run_watch().join(),
                             automation.EXIT_FAILED)
        self.assertEqual(self.record()["state"], automation.WATCH_ERROR)
        self.assertEqual(self.run_watch().join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)

    def test_the_refusal_survives_a_watch_that_was_killed(self) -> None:
        # Rule (R2): SIGKILL leaves the record "starting".
        self.fail_for_approval()
        automation.WatchRecord(self.options("status")).update(
            state=automation.STARTING, detail="")
        self.assertEqual(self.run_watch().join(), automation.EXIT_NEEDS_APPROVAL)
        self.invoked(2)

    def test_only_a_host_that_serves_clears_it(self) -> None:
        # A serving host that a watch finds (a `start` by another route) does.
        self.fail_for_approval()
        self.tune(HEARTBEAT_STALE=30)
        self.device.scenarios([{"kind": "serve"}])
        self.device.wait(lambda: self.device.silent_for() > 2.5, 30,
                         "the first host's last beat is old")
        host = self.start_host()
        again = self.run_watch()
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], host.nonce)
        self.assertIsNone(automation.ApprovalLatch(self.options("status")).read())
        self.assertEqual(again.end(), 0)


class LaunchTests(WatchTestCase):
    def test_start_and_watch_never_launch_side_by_side(self) -> None:
        # Rule (R4): a `start` that has checked that no watch runs and is in
        # the report preflight, which can take minutes, holds the launch lock;
        # a watch that begins then cannot launch until the `start` has recorded
        # its host, and then waits for that host instead of starting another.
        self.device.scenarios([{"kind": "serve", "hold": True}])
        inside = threading.Event()
        release = threading.Event()
        real = automation.check_report_slots
        first = []

        def preflight(options, processes):
            if not first:
                first.append(1)
                inside.set()
                release.wait(60)
            return real(options, processes)
        self.patch(automation, "check_report_slots", preflight)
        started: list[int] = []
        thread = threading.Thread(target=lambda: started.append(
            automation.command_start(self.options(
                "start", "--hours", "1", "--wait", "40", *self.reports()))),
            daemon=True)
        thread.start()
        self.assertTrue(inside.wait(30))
        self.run_watch()
        self.device.wait(lambda: any("another launch" in message
                                     for message in self.messages), 30,
                         "the watch waits for the `start`'s launch")
        self.assertEqual(self.device.invocations(), [])
        release.set()
        self.device.wait(lambda: len(self.device.invocations()) == 1, 30,
                         "the `start`'s host runs")
        self.device.wait(lambda: any("waiting for it" in message
                                     for message in self.messages), 30,
                         "the watch waits for the host the `start` made")
        self.invoked(1)
        nonce = self.device.invocations()[0]["nonce"]
        self.device.control(nonce, "go")
        thread.join(60)
        self.assertEqual(started, [0], self.messages)
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], nonce)
        self.invoked(1)

    def test_a_stop_during_the_report_preflight_starts_nothing(self) -> None:
        # Rule (R4): cancellation is checked again after the preflight and
        # before the supervisor is spawned.
        stop = threading.Event()
        real = automation.check_report_slots

        def preflight(options, processes):
            result = real(options, processes)
            stop.set()
            return result
        self.patch(automation, "check_report_slots", preflight)
        watch = Running(self.watch_options(), stop)
        self.watches.append(watch)
        self.assertEqual(watch.join(), 0)
        self.assertEqual(self.device.invocations(), [])
        self.assertEqual(self.record()["state"], automation.STOPPED)
        self.assertFalse(list((self.device.state_dir / self.device.udid).glob("host-*.log")))

    def test_the_host_record_is_whole_before_the_supervisor_starts(self) -> None:
        # Rule (R4): host.json is written atomically, and before the
        # supervisor that a reader would look for exists; the recorded wait is
        # the starter's own.
        seen = []
        real = automation.subprocess.Popen

        def popen(arguments, *positional, **keywords):
            if "_supervise" in arguments:
                seen.append(json.loads((self.device.state_dir / self.device.udid
                                        / "host.json").read_text()))
            return real(arguments, *positional, **keywords)
        self.patch(automation.subprocess, "Popen", popen)
        moved = []
        real_replace = os.replace
        self.patch(automation.os, "replace", lambda source, target: (
            moved.append(Path(target).name), real_replace(source, target))[1])
        launch = automation.launch_host(self.options(
            "start", "--hours", "1", "--wait", "77", *self.reports()))
        self.launches.append(launch)
        # Moved into place both times, never written in place.
        self.assertEqual(moved.count("host.json"), 2)
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0]["nonce"], launch.nonce)
        self.assertIsNone(seen[0]["supervisor"])
        self.assertEqual(seen[0]["wait"], 77)
        final = automation.local_record(self.options("status"))
        self.assertEqual(final["supervisor"], launch.process.pid)
        self.assertFalse(list((self.device.state_dir / self.device.udid).glob("host.json.*")))

    def test_a_record_that_ended_before_it_knew_the_pid_still_finds_the_supervisor(self) -> None:
        launch = self.start_host()
        record = automation.local_record(self.options("status"))
        record["supervisor"] = None
        self.assertEqual(automation.recorded_supervisor(record), launch.process.pid)

    def test_each_collection_of_reports_gets_a_directory_of_its_own(self) -> None:
        # Rule (R8): the collector refuses a destination that exists, and a
        # watch collects again and again. With report checks on, each
        # collection (the launch's and the supervisor's, two hosts over) goes
        # to a new directory beneath the configured one; the real destination
        # checks run, against a report store that is full every time.
        reports = automation.ios_device_reports

        class Store:
            copies = 0
            listings = 0

            def listing(self):
                # A collection counts the store, moves everything, and counts
                # again: the store is full each time it is first counted.
                Store.listings += 1
                if Store.listings % 2 == 0:
                    return []
                return [reports.ListedFile(f"JetsamEvent-2000-01-01-000000.{i}.ips", 1)
                        for i in range(25)]

            def resolve_udid(self):
                return "00000000-0000000000000000"

            def require_mover(self):
                pass

            def copy_verified(self, files, directory):
                Store.copies += 1

            def remove_all(self, udid, directory):
                pass
        spawned = []

        class Process:
            pid = 99999

        real = automation.subprocess.Popen

        def popen(arguments, *positional, **keywords):
            if "_supervise" in arguments:
                spawned.append(arguments)
                return Process()
            return real(arguments, *positional, **keywords)
        base = self.device.root / "kept-reports"
        options = self.options("watch", "--hours", "1", "--device-reports-dir", str(base))
        self.assertFalse(options.leave_device_reports)
        with mock.patch.object(reports, "DeviceStore", lambda device: Store()), \
                mock.patch.object(automation.subprocess, "Popen", popen):
            automation.launch_host(options)
            automation.launch_host(options)
            for arguments in spawned:
                supervised = automation.parse(arguments[arguments.index("--device") :])
                self.assertTrue(automation.check_report_slots(
                    supervised, automation.REPORT_PROCESSES), self.messages)
        self.assertEqual(Store.copies, 4)
        self.assertEqual(len(list(base.iterdir())), 4)
        self.assertFalse(any("already exists" in message for message in self.messages))


class CleanupTests(WatchTestCase):
    def test_a_replacement_ends_the_whole_of_a_host_that_ignores_sigterm(self) -> None:
        # Rule (R5): the supervisor stays until its children are gone. The
        # xcodebuild of this host ignores SIGTERM; the replacement waits for it
        # to be ended, and nothing of the first host is left beside the second.
        self.tune(HEARTBEAT_STALE=2.0)
        self.device.scenarios([{"kind": "serve", "stubborn": True}, {"kind": "serve"}])
        self.run_watch()
        first = self.watching(restarts=1)
        old = self.device.invocations()[0]
        self.device.control(first, "silent")
        second = self.watching(restarts=2)
        self.assertNotEqual(first, second)
        self.assertFalse(self.device.alive(old["pid"]))
        self.assertFalse(self.device.alive(old["ppid"]))
        self.assertEqual(automation.group_members(old["ppid"]), [])

    def test_an_orphaned_xcodebuild_is_ended_by_its_nonce(self) -> None:
        # Rule (R5): the supervisor of an adopted host is gone and its
        # xcodebuild lives on, ignoring SIGTERM. The record's nonce, which the
        # run's result bundle name carries, shows it is the host's: it is ended
        # with SIGKILL, and only then does the replacement start.
        self.tune(HEARTBEAT_STALE=2.0)
        self.device.scenarios([{"kind": "serve", "stubborn": True}, {"kind": "serve"}])
        host = self.start_host()
        old = self.device.invocations()[0]
        os.kill(old["ppid"], signal.SIGKILL)
        self.device.wait(lambda: not self.device.alive(old["ppid"]), 10,
                         "the supervisor ends")
        self.assertTrue(self.device.alive(old["pid"]))
        self.device.control(host.nonce, "silent")
        self.run_watch()
        second = self.watching(restarts=1)
        self.assertNotEqual(second, host.nonce)
        self.assertFalse(self.device.alive(old["pid"]))

    def test_an_unrelated_group_under_a_reused_pid_is_left_alone(self) -> None:
        # Rule (R5): no member of the group shows the record's nonce.
        decoy = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        self.addCleanup(lambda: (decoy.kill(), decoy.wait()))
        self.assertEqual(
            automation.end_host({"supervisor": decoy.pid, "nonce": "f" * 32}),
            automation.FOREIGN)
        self.assertIsNone(decoy.poll())

    def test_a_host_that_will_not_end_stops_the_watch(self) -> None:
        # Rule (R5): fail closed. Nothing starts beside a host that cannot be
        # ended.
        self.tune(HEARTBEAT_STALE=2.0)
        self.run_watch()
        first = self.watching(restarts=1)
        self.patch(automation, "end_host", lambda *a, **k: automation.STUCK)
        self.device.control(first, "silent")
        watch = self.watches[0]
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(self.record()["state"], automation.CANNOT_START)
        self.assertIn("would not end", self.record()["detail"])
        self.invoked(1)


class CountingTests(WatchTestCase):
    def test_a_replacement_made_across_a_watch_crash_still_counts(self) -> None:
        # Rule (R7): the watch that launched the replacement ended before the
        # host served; the next watch adopts it, and it counts, once. With a
        # bound of one, the next end is a restart storm, not another host.
        self.tune(HEARTBEAT_STALE=2.0)
        host = automation.launch_host(self.options(
            "start", "--hours", "1", *self.reports()), record=lambda nonce, path: (
            automation.WatchRecord(self.options("status")).update(
                state=automation.RESTARTING, pending={
                    "nonce": nonce, "log": str(path), "time": time.time(),
                    "replacement": True})))
        self.launches.append(host)
        self.device.wait(lambda: self.device.serving(host.nonce), 30, "it serves")
        watch = self.run_watch("--max-restarts", "1")
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], host.nonce)
        self.assertEqual(record["restarts"], 1)
        self.assertEqual(len(record["restart_times"]), 1)
        self.assertIsNone(record["pending"])
        self.device.control(host.nonce, "die")
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(self.record()["state"], automation.RESTART_STORM)
        self.invoked(1)

    def test_the_replacement_is_recorded_before_its_host_starts(self) -> None:
        # Rule (R7): a watch killed after the launch but before the host serves
        # must leave the replacement on record: the watch's own record names
        # the nonce already when the supervisor is spawned.
        seen = []
        real = automation.subprocess.Popen

        def popen(arguments, *positional, **keywords):
            if "_supervise" in arguments:
                seen.append((arguments[arguments.index("--nonce") + 1],
                             self.record().get("pending")))
            return real(arguments, *positional, **keywords)
        self.patch(automation.subprocess, "Popen", popen)
        self.run_watch()
        first = self.watching(restarts=1)
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0][0], first)
        self.assertEqual(seen[0][1]["nonce"], first)
        self.assertEqual(seen[0][1]["replacement"], False)
        self.assertIsNone(self.record()["pending"])

    def test_a_pending_replacement_of_another_host_is_not_counted(self) -> None:
        self.tune(HEARTBEAT_STALE=30)
        automation.WatchRecord(self.options("status")).update(
            pending={"nonce": "c" * 32, "log": "x", "time": time.time(),
                     "replacement": True})
        host = self.start_host()
        self.run_watch()
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], host.nonce)
        self.assertEqual(record["restarts"], 0)
        self.assertIsNone(record["pending"])


class LockOwnerTests(WatchTestCase):
    """`stop` signals the process that holds this device's lock, as the lock
    file names it, and no other."""

    def hold(self, **identity) -> int:
        directory = self.device.state_dir / self.device.udid
        directory.mkdir(parents=True, exist_ok=True)
        fd = os.open(directory / "watch.lock", os.O_RDWR | os.O_CREAT, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX)
        self.addCleanup(os.close, fd)
        os.ftruncate(fd, 0)
        os.pwrite(fd, json.dumps(identity).encode(), 0)
        return fd

    def decoy(self) -> subprocess.Popen:
        # Looks like a watch of this script to a check of the command line.
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(60)", "watch",
             "ipad-automation.py"], stdin=subprocess.DEVNULL)
        self.addCleanup(lambda: (process.kill(), process.wait()))
        deadline = time.monotonic() + 10
        while True:
            instance = automation.process_instance(process.pid)
            if instance is not None and "time.sleep" in instance[1]:
                return process
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)

    def stop_with(self, signals: list) -> bool:
        self.tune(WATCH_END_WAIT=0.5)
        self.patch(automation, "signal_watcher", signals.append)
        return automation.end_watch(self.options("stop", "--force"))

    def identity(self, process, **changes) -> dict:
        identity = {"pid": process.pid,
                    "start": automation.process_instance(process.pid)[0],
                    "device": self.device.udid,
                    "state": os.path.realpath(self.device.state_dir / self.device.udid)}
        identity.update(changes)
        return identity

    def test_a_pid_that_was_reused_is_not_signalled(self) -> None:
        # Rule (R6): the lock is held by a new watch that has not written its
        # pid yet; the file still names a watch that ended, whose pid is now
        # another process: a watch for another device here.
        process = self.decoy()
        self.hold(**self.identity(process, start="Mon Jan  1 00:00:00 2001"))
        signals: list = []
        self.assertFalse(self.stop_with(signals))
        self.assertEqual(signals, [])
        self.assertIsNone(process.poll())

    def test_a_watch_of_another_device_is_not_signalled(self) -> None:
        process = self.decoy()
        self.hold(**self.identity(process, device="another-device"))
        signals: list = []
        self.assertFalse(self.stop_with(signals))
        self.assertEqual(signals, [])

    def test_a_watch_of_another_state_directory_is_not_signalled(self) -> None:
        process = self.decoy()
        self.hold(**self.identity(process, state="/somewhere/else"))
        signals: list = []
        self.assertFalse(self.stop_with(signals))
        self.assertEqual(signals, [])

    def test_the_holder_the_file_names_is_signalled(self) -> None:
        process = self.decoy()
        self.hold(**self.identity(process))
        signals: list = []
        self.assertFalse(self.stop_with(signals))   # the lock stays held
        self.assertEqual(signals, [process.pid])


class StopTests(WatchTestCase):
    def end_by_event(self, watch: Running):
        """Stands for the SIGTERM `stop --force` sends: in this process the
        watch is a thread, so the signal sets its stop event."""
        self.patch(automation, "signal_watcher", lambda pid: watch.stop.set())
        self.patch(automation, "watcher_is_a_watch", lambda pid: True)

    def test_a_deliberate_stop_is_not_undone(self) -> None:
        # Rule: `stop --force` ends the watch before the host, so the watch
        # does not start the host again: the host is told to stop only once the
        # watch has been signalled and has ended.
        self.run_watch()
        first = self.watching(restarts=1)
        watch = self.watches[0]
        self.end_by_event(watch)
        order = []
        real = automation.send

        def send(options, command, timeout):
            order.append((command["op"], watch.stop.is_set(),
                          automation.watcher_pid(options) is None))
            return real(options, command, timeout)
        self.patch(automation, "send", send)
        self.assertEqual(automation.command_stop(
            self.options("stop", "--force")), 0)
        self.assertEqual(order, [("stop", True, True)])
        self.assertEqual(watch.join(), 0)
        self.assertEqual(self.record()["state"], automation.STOPPED)
        self.device.wait(lambda: not self.device.alive(
            self.device.invocations()[0]["pid"]), 15, "the host ended")
        self.assertFalse(self.device.serving())
        self.assertEqual(self.nonces(), [first])

    def test_a_stop_without_force_is_refused_and_the_watch_runs_on(self) -> None:
        self.tune(HEARTBEAT_STALE=30)
        host = self.start_host()
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        self.assertEqual(automation.command_stop(self.options("stop")),
                         automation.EXIT_FAILED)
        self.progress()
        self.assertTrue(self.device.serving(host.nonce))
        self.assertTrue(automation.watch_summary(self.options("status"))["running"])

    def test_a_watch_that_will_not_end_keeps_the_host(self) -> None:
        # Rule: if the watch cannot be ended the host stays, as stopping it
        # would only have the watch start it again.
        self.tune(WATCH_END_WAIT=1)
        host = self.start_host()
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        self.patch(automation, "signal_watcher", lambda pid: None)
        self.patch(automation, "watcher_is_a_watch", lambda pid: True)
        self.assertEqual(automation.command_stop(self.options("stop", "--force")),
                         automation.EXIT_FAILED)
        self.assertTrue(self.device.serving(host.nonce))

    def test_start_is_refused_while_a_watch_runs(self) -> None:
        # Rule: a second host beside the watch's would ask for approval.
        self.tune(HEARTBEAT_STALE=30)
        self.start_host()
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        before = len(self.device.invocations())
        self.assertEqual(automation.command_start(self.options(
            "start", *self.reports())), automation.EXIT_FAILED)
        self.assertEqual(len(self.device.invocations()), before)
        self.assertTrue(any("watch is running" in message for message in self.messages))


class StatusTests(WatchTestCase):
    def test_status_shows_the_watch_and_its_last_end(self) -> None:
        self.run_watch()
        first = self.watching(restarts=1)
        self.device.control(first, "die")
        self.watching(restarts=2)
        status = automation.watch_summary(self.options("status"))
        self.assertTrue(status["running"])
        self.assertEqual(status["pid"], os.getpid())
        self.assertEqual(status["state"], automation.WATCHING)
        self.assertEqual(status["restarts"], 2)
        self.assertIn("reason", status["last_end"])
        self.assertGreater(status["last_end"]["time"], 0)

    def test_a_watch_that_ended_without_a_word_is_shown_as_not_running(self) -> None:
        directory = self.device.state_dir / self.device.udid
        directory.mkdir(parents=True)
        (directory / "watch.json").write_text(json.dumps(
            {"state": automation.WATCHING, "pid": 1, "restarts": 3}))
        status = automation.watch_summary(self.options("status"))
        self.assertFalse(status["running"])
        self.assertIn("without recording why", status["meaning"])


class LogSignalTests(unittest.TestCase):
    """The lines of xcodebuild's own log the watch reads, as it prints them."""

    PROMPT = PROMPT
    RUNNER = ("2026-11-13 22:04:30.522108+0900 KernariumAutomation-Runner[11111:"
              "2222222] [Default] Running tests...")

    def scan(self, *lines: str):
        with mock.patch("builtins.open", mock.mock_open(
                read_data=("\n".join(lines) + "\n").encode())):
            scan = automation.LogScan("log")
            scan.read()
        return scan

    def test_the_prompt_timeout_is_told_from_other_initialization_failures(self) -> None:
        self.assertTrue(self.scan(self.RUNNER, self.PROMPT).approval_failure)
        other = self.PROMPT.replace("Timed out while enabling automation mode",
                                    "Timed out while loading Accessibility")
        scan = self.scan(self.RUNNER, other)
        self.assertFalse(scan.approval_failure)
        self.assertIn("Failed to initialize for UI testing", scan.init_failure)
        self.assertFalse(self.scan(self.RUNNER, "Test Suite started").approval_failure)

    def test_a_cancelled_authentication_is_an_approval_signal(self) -> None:
        # The device's failure when the owner's authentication was cancelled:
        # Domain and Code are what is matched, the description is localized.
        template = (
            "2026-11-14 00:05:00.059296+0900 KernariumAutomation-Runner[11111:"
            "2222222] [Default] Failed to initialize for UI testing: Error Domain="
            '{domain} Code={code} "{text}" UserInfo={{NSLocalizedDescription={text}}}')
        for text in ("UI canceled by system.", "认证已取消。", "Authentication was cancelled."):
            for code in ("-4", "-2"):
                with self.subTest(text=text, code=code):
                    scan = self.scan(self.RUNNER, template.format(
                        domain="com.apple.LocalAuthentication", code=code, text=text))
                    self.assertTrue(scan.approval_failure)
                    self.assertIn("LocalAuthentication", scan.describe())
        for domain, code in (("com.apple.LocalAuthentication", "-1"),
                             ("com.apple.LocalAuthentication", "-44"),
                             ("com.apple.dt.XCTest.XCTFuture", "-4"),
                             ("com.example.LocalAuthentication", "-4")):
            with self.subTest(domain=domain, code=code):
                scan = self.scan(self.RUNNER, template.format(
                    domain=domain, code=code, text="Something else."))
                self.assertFalse(scan.approval_failure)
                self.assertIn("Failed to initialize", scan.init_failure)
        # The same words outside the initialization failure are not it.
        self.assertFalse(self.scan(
            "Error Domain=com.apple.LocalAuthentication Code=-4").approval_failure)

    def test_a_runner_started_again_is_counted(self) -> None:
        self.assertEqual(self.scan(self.RUNNER).runner_up, 1)
        self.assertEqual(self.scan(self.RUNNER, "x", self.RUNNER).runner_up, 2)

    def test_the_supervisors_exit_line_is_read(self) -> None:
        scan = self.scan("ipad-automation: xcodebuild exited with status 65")
        self.assertIn("status 65", scan.describe())

    def test_a_line_is_taken_once_it_is_complete(self) -> None:
        with mock.patch("builtins.open", mock.mock_open(
                read_data=(self.RUNNER + "\n" + self.RUNNER[:30]).encode())):
            scan = automation.LogScan("log")
            scan.read()
        self.assertEqual(scan.runner_up, 1)

    def test_a_line_that_is_completed_by_a_later_read_is_taken_then(self) -> None:
        # The prompt's line arrives in two pieces, as a log written while it is
        # read can show it: the first read keeps the piece, the second has it.
        with __import__("tempfile").TemporaryDirectory() as scratch:
            path = Path(scratch) / "log"
            path.write_text(self.PROMPT[:80])
            scan = automation.LogScan(str(path))
            scan.read()
            self.assertFalse(scan.approval_failure)
            with open(path, "a") as handle:
                handle.write(self.PROMPT[80:] + "\n")
            scan.read()
            self.assertTrue(scan.approval_failure)


if __name__ == "__main__":
    unittest.main()
