#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""`ipad-automation.py watch` keeps a device's UI automation host serving.

A host that ended minutes ago is replaced by a new one without the device's
approval prompt; one that ended hours ago is not. The watch therefore starts
the next host the moment the old one stops serving, and fails, once, when the
device asks for approval. Every test runs the real script (its supervisor
included) against `automation_fakes.FakeDevice`, an `xcrun` and an `xcodebuild`
that are files in a scratch folder, so no call reaches a device or Xcode. The
watch's waits are scaled down; each test names the rule it pins.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import json
import os
from pathlib import Path
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
    "WATCH_COMMAND_WAIT": 30, "WATCH_FRESH": 1.5, "WATCH_END_WAIT": 20,
    "SUPERVISOR_TERM_WAIT": 5, "SEND_LIVENESS_EVERY": 0.5,
}
SECONDS = 3 * 3600


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
            automation.end_process_group(launch.process, 2)

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

    def quiet(self, seconds: float, count: int) -> None:
        """No host starts in the next `seconds`: the number of starts stays."""
        time.sleep(seconds)
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
        self.quiet(1.5, 1)
        self.assertIsNone(host.process.poll())
        self.assertTrue(self.device.serving(host.nonce))
        self.assertEqual(watch.end(), 0)
        self.assertEqual(self.record()["state"], automation.STOPPED)
        # Ending the watch alone leaves the host as it was.
        self.assertIsNone(host.process.poll())
        self.assertTrue(self.device.serving(host.nonce))

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
        self.quiet(1.0, 1)
        self.assertEqual(self.record()["state"], automation.STARTING)
        self.assertIsNone(launch.process.poll())
        self.device.control(launch.nonce, "go")
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], launch.nonce)
        self.quiet(0.5, 1)
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
        ended = time.monotonic()
        new = self.watching(restarts=1)
        self.assertLess(time.monotonic() - ended, 15)
        invocations = self.device.invocations()
        self.assertEqual([item["nonce"] for item in invocations], [host.nonce, new])
        replacement = invocations[1]
        self.assertEqual(replacement["seconds"], SECONDS)
        supervisor = replacement["supervisor"]
        self.assertIn(f"--seconds {SECONDS}", supervisor)
        self.assertIn(f"--device-reports-dir {self.device.reports}", supervisor)
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
        # new one serves nothing) is read from its log and replaced within
        # WATCH_GONE_AGE, and the live xcodebuild is ended. The heartbeat's own
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
        self.quiet(2.5, 1)
        self.assertTrue(self.device.serving(first))
        self.assertEqual(self.record()["restarts"], 1)

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
        self.quiet(3.5, 1)
        self.assertTrue(self.device.serving(first))

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
        self.quiet(1.0, 2)
        status = automation.watch_summary(self.options("status"))
        self.assertFalse(status["running"])
        self.assertEqual(status["meaning"], "needs approval at the device")
        # A second watch does not start a host either.
        self.assertEqual(self.run_watch().join(), automation.EXIT_NEEDS_APPROVAL)
        self.assertEqual(len(self.device.invocations()), 2)
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
        again = self.run_watch()
        record = self.wait_record(state=automation.WATCHING)
        self.assertEqual(record["host"], third["nonce"])
        self.assertEqual(again.end(), 0)

    def test_a_runner_that_never_serves_is_not_the_approval_prompt(self) -> None:
        # Rule: a runner that is up and silent is an ordinary failure, retried
        # a bounded number of times: only XCTest's line means approval.
        self.tune(WATCH_ATTEMPTS=2, WATCH_RUNNER_WAIT=1.0)
        self.device.scenarios([{"kind": "up-silent"}])
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(len(self.device.invocations()), 2)
        record = self.record()
        self.assertEqual(record["state"], automation.START_FAILED)
        self.assertIn("did not serve", record["detail"])
        for item in self.device.invocations():
            self.assertFalse(self.device.alive(item["ppid"]))

    def test_a_missing_device_is_retried_a_bounded_number_of_times(self) -> None:
        # Rule: other failures get a few attempts with a short backoff, then a
        # state of their own; never an endless loop.
        self.device.scenarios([{"kind": "exit", "status": 70}])
        watch = self.run_watch()
        self.assertEqual(watch.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(len(self.device.invocations()), automation.WATCH_ATTEMPTS)
        record = self.record()
        self.assertEqual(record["state"], automation.START_FAILED)
        self.assertIn("status 70", record["detail"])
        self.quiet(0.5, automation.WATCH_ATTEMPTS)

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
        self.assertEqual(len(self.device.invocations()), 2)
        again = self.run_watch("--max-restarts", "2")
        self.assertEqual(again.join(), automation.EXIT_WATCH_GAVE_UP)
        self.assertEqual(len(self.device.invocations()), 2)
        self.assertEqual(self.record()["state"], automation.RESTART_STORM)

    def test_restarts_older_than_an_hour_do_not_count(self) -> None:
        self.tune(WATCH_STORM_WINDOW=1)
        watch = self.run_watch("--max-restarts", "1")
        first = self.watching(restarts=1)
        time.sleep(1.2)
        self.device.control(first, "die")
        self.watching(restarts=2)
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
    def ping(self, results: list) -> threading.Thread:
        def send() -> None:
            try:
                results.append(automation.send(
                    self.options("ping"), {"op": "ping"}, 20))
            except automation.NoHost as error:
                results.append(error)
        thread = threading.Thread(target=send, daemon=True)
        thread.start()
        return thread

    def test_a_command_waits_through_a_replacement(self) -> None:
        # Rule: a command sent while the watch is starting a host waits for it
        # and is answered by it. The heartbeat is made to look fresh, so that
        # only the watch's own state can hold the command back.
        self.tune(WATCH_FRESH=1000)
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
        # yet; a command does not go to it, and is answered by the successor.
        self.tune(HEARTBEAT_STALE=3.0, WATCH_FRESH=1.0)
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
        self.tune(WATCH_COMMAND_WAIT=2, WATCH_RUNNER_WAIT=30, WATCH_FRESH=1000)
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


class StopTests(WatchTestCase):
    def end_by_event(self, watch: Running):
        """Stands for the SIGTERM `stop --force` sends: in this process the
        watch is a thread, so the signal sets its stop event."""
        self.patch(automation, "signal_watcher", lambda pid: watch.stop.set())
        self.patch(automation, "watcher_is_a_watch", lambda pid: True)

    def test_a_deliberate_stop_is_not_undone(self) -> None:
        # Rule: `stop --force` ends the watch before the host, so the watch
        # does not start the host again.
        self.run_watch()
        first = self.watching(restarts=1)
        watch = self.watches[0]
        self.end_by_event(watch)
        self.assertEqual(automation.command_stop(
            self.options("stop", "--force")), 0)
        self.assertEqual(watch.join(), 0)
        self.assertEqual(self.record()["state"], automation.STOPPED)
        self.device.wait(lambda: not self.device.alive(
            self.device.invocations()[0]["pid"]), 15, "the host ended")
        self.quiet(2.5, 1)
        self.assertFalse(self.device.serving())
        self.assertEqual(self.nonces(), [first])

    def test_a_stop_without_force_is_refused_and_the_watch_runs_on(self) -> None:
        self.tune(HEARTBEAT_STALE=30)
        host = self.start_host()
        self.run_watch()
        self.wait_record(state=automation.WATCHING)
        self.assertEqual(automation.command_stop(self.options("stop")),
                         automation.EXIT_FAILED)
        self.quiet(0.5, 1)
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

    PROMPT = (
        "2026-11-13 16:04:14.424136+0900 KernariumAutomation-Runner[11111:2222222] "
        "[Default] Failed to initialize for UI testing: Error Domain="
        'com.apple.dt.XCTest.XCTFuture Code=1000 "Timed out while enabling '
        'automation mode." UserInfo={NSLocalizedDescription=Timed out while '
        "enabling automation mode.}")
    RUNNER = ("2026-11-13 22:04:30.522108+0900 KernariumAutomation-Runner[11111:"
              "2222222] [Default] Running tests...")

    def scan(self, *lines: str):
        with mock.patch("builtins.open", mock.mock_open(
                read_data=("\n".join(lines) + "\n").encode())):
            scan = automation.LogScan("log")
            scan.read()
        return scan

    def test_the_prompt_timeout_is_told_from_other_initialization_failures(self) -> None:
        self.assertTrue(self.scan(self.RUNNER, self.PROMPT).approval_timeout)
        other = self.PROMPT.replace("Timed out while enabling automation mode",
                                    "Timed out while loading Accessibility")
        scan = self.scan(self.RUNNER, other)
        self.assertFalse(scan.approval_timeout)
        self.assertIn("Failed to initialize for UI testing", scan.init_failure)
        self.assertFalse(self.scan(self.RUNNER, "Test Suite started").approval_timeout)

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


if __name__ == "__main__":
    unittest.main()
