#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive UI automation on a connected iPad through one long-lived host.

Each XCTest session that turns UI automation on can make the device ask for
its owner's passcode or biometrics. The KernariumAutomation UI-test target
keeps one session alive for hours and executes commands this script places in
its runner's data container over `devicectl`, so one approval at the device
serves every later command. Build the host once, start it, approve the prompt
at the device, and then send commands:

  ipad-automation.py --device UDID build
  ipad-automation.py --device UDID start --hours 8
  ipad-automation.py --device UDID launch --env KERNARIUM_ROOTFS=none.erofs
  ipad-automation.py --device UDID tap --id displayModeButton
  ipad-automation.py --device UDID orientation landscapeLeft
  ipad-automation.py --device UDID --bundle com.apple.springboard drag 500 700 400 600 1
  ipad-automation.py --device UDID screenshot -o shot.png
  ipad-automation.py --device UDID stop --force

Every command has a bounded wait. A command fails with exit status 3 when the
host is not running. UI automation stays approved only while some XCTest
session keeps it on, so a new host normally needs a new approval at the
device: never stop a working host without someone to approve its successor.
Run `start` only while someone can approve, because XCTest gives up 60 s after
the runner starts and leaves the prompt on screen. The host targets
com.imxieyi.Kernarium unless --bundle names another application, such as
com.apple.springboard.

A coordinate tap or drag names points as offsets from the origin of that
application's frame, which `tree` prints; springboard's frame is the whole
screen. A drag presses its start for the given seconds before moving, which is
what picks up a window's resize handle. `orientation` rotates the device and
reports the orientation it reached; without a name it only reports.

No command ends the host: a failed command reports its error, and the host
prints it to its log. If every command starts taking about 60 s, a system
keyboard animation has left XCTest's wait for an idle application stuck;
relaunch the application. The host is not in the Kernarium scheme, so
reinstalling Kernarium or running other tests leaves it serving, and its log
records each such termination as a Kernarium crash.

A host ends on its own now and then: its runner dies under memory pressure
on a small device, its lifetime runs out, or the process that started it goes
away. A new host started within minutes of the old one's end needs no approval;
one started hours later does. `watch` keeps one device's host serving:

  ipad-automation.py --device UDID watch --hours 8
  ipad-automation.py --device UDID watch --detach
  ipad-automation.py --device UDID watch --print-launchd-job LABEL > job.plist

The same `watch` runs in a terminal's foreground, detached by `--detach` (its
log is watch-<time>.log in the state directory), or as a job manager's job
(`--print-launchd-job` prints a launchd job that sets up its own PATH, runs
once and comes back only after a crash; the job's registration is the owner's).
It adopts a host that already serves and never stops or restarts one that
does. A host stops serving when its heartbeat is older than 20 s (a busy host
keeps beating: its heartbeat comes from a queue no command blocks), or older
than 5 s once its supervisor ended or xcodebuild started the runner again, or
when its lifetime ended, or when one command has run for 30 minutes. The next
host starts at once with the watch's own options, after ending what is left of
the old one; detection takes a few seconds, 25 at most.

A replacement that the device wants approved is not retried: xcodebuild's line
"Timed out while enabling automation mode" (XCTest's own 60 s give-up) ends
the watch with status 4 and the state needs-approval, which `status` reports,
and a new `watch` starts nothing until a `start` with the owner at the device
has served. Any other failure to start (device not connected or locked, report
slots short, a runner that does not serve within 90 s of coming up) is retried
three times after 5, 10 and 20 s, then ends the watch with status 5 and the
state start-failed. No built host, or a run file that lets Xcode record the
screen, ends it at once with the state cannot-start; more than 6 replacements
(--max-restarts) in an hour end it with restart-storm. Status 6: another watch
holds this device's lock. `status` shows the watch and its last end.

A command sent while the watch is starting a host, or to a host that has not
beaten for 4 s, waits for a host that beats, up to 180 s, and fails with
status 3 as before when no watch runs or the watch has ended. A command sent to
a host that ends while it runs is not repeated: it may have run. `stop --force` ends the watch first, so that it
does not start the host again, and then the host; `start` refuses while a watch
runs. Ending the watch alone (SIGTERM, or the job manager's stop) leaves a
serving host running.

The host's test run records no video of the screen and keeps no attachments: a
host runs for hours, and Xcode's defaults would store a recording of whatever
the device shows. The KernariumAutomation scheme turns both off, `start`
refuses a built test run file that does not say so, and the run's result
bundle lives next to the host log in --state-dir. `start`, `launch` and
`activate` first make room for diagnostic reports on the device
(tests/integration/support/ios_device_reports.py), so a kill during the run
leaves a report.
"""

import argparse
import fcntl
import json
import os
import pathlib
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import uuid
from xml.parsers import expat

RUNNER = "com.imxieyi.KernariumAutomation.xctrunner"
KERNARIUM = "com.imxieyi.Kernarium"
CHANNEL = "Documents/automation"
TEST = "KernariumAutomation/AutomationHost/testServe"
HEARTBEAT_STALE = 20
EXIT_FAILED = 1
EXIT_NO_HOST = 3
# `watch` ends with one of these when it gave up: the owner is needed at the
# device, or it stopped trying for a reason its record names; or when another
# watch for the same device and state directory already runs.
EXIT_NEEDS_APPROVAL = 4
EXIT_WATCH_GAVE_UP = 5
EXIT_WATCH_LOCKED = 6
# How long `stop` lets the supervisor end on its own before signalling it.
SUPERVISOR_EXIT_WAIT = 60

# The watch (see `watch` in the module's help). Every wait it makes is bounded
# by these, in seconds.
#
# How often it looks at the host's processes and log, and how often it reads
# the heartbeat when those show nothing wrong.
WATCH_POLL = 2
WATCH_PULL_EVERY = 5
# The host beats every 2 s from a queue its commands never block. A host whose
# supervisor or runner is gone is replaced once its last beat is older than
# this; any other silent host after HEARTBEAT_STALE.
WATCH_GONE_AGE = 5
# One command running longer than this is a host whose main thread is hung
# (every command has a bound of minutes), and the host is replaced.
WATCH_BUSY_LIMIT = 1800
# After "Running tests..." XCTest gives the device's approval prompt 60 s; a
# runner that has neither served nor timed out by then is not waiting for one.
WATCH_RUNNER_WAIT = 90
# Attempts at one replacement, and the pauses after each failed one. The
# approval prompt is skipped when the previous host ended minutes earlier, so
# the pauses stay short.
WATCH_ATTEMPTS = 4
WATCH_BACKOFF = (5, 10, 20)
# More replacements than this in WATCH_STORM_WINDOW end the watch.
WATCH_MAX_RESTARTS = 6
WATCH_STORM_WINDOW = 3600
# How long a command waits for a replacement the watch is making, how recent
# a beat it needs before it sends, and how long `stop --force` lets the watch
# end and `watch --detach` lets the new watch take its lock.
WATCH_COMMAND_WAIT = 180
WATCH_FRESH = 4
WATCH_END_WAIT = 90
WATCH_DETACH_WAIT = 20
# How often a command waiting for its result looks at the host's heartbeat.
SEND_LIVENESS_EVERY = 10
# How long what is left of an ended host has to obey SIGTERM before SIGKILL:
# its successor waits for it.
SUPERVISOR_TERM_WAIT = 5
# xcodebuild's own lines: the runner is up, and XCTest gave up on the prompt.
RUNNER_UP_LINE = "Running tests..."
APPROVAL_TIMEOUT_LINE = "Timed out while enabling automation mode"
INIT_FAILURE_LINE = "Failed to initialize for UI testing"
EXIT_STATUS_LINE = "ipad-automation: xcodebuild exited with status"
# The watch record's states: the first three while the watch runs, the others
# when it has ended.
WATCHING, RESTARTING, STARTING = "watching", "restarting", "starting"
NEEDS_APPROVAL, START_FAILED = "needs-approval", "start-failed"
RESTART_STORM, CANNOT_START = "restart-storm", "cannot-start"
STOPPED, WATCH_ERROR = "stopped", "error"
ACTIVE_STATES = (WATCHING, RESTARTING, STARTING)
# What the watch's record says for each ended state.
STATE_TEXT = {
    NEEDS_APPROVAL: "needs approval at the device",
    START_FAILED: "could not start a host",
    RESTART_STORM: "gave up: too many restarts",
    CANNOT_START: "cannot start a host here",
    STOPPED: "ended on request",
    WATCH_ERROR: "ended on an error",
}
# What the host's test run file must say (the KernariumAutomation scheme sets
# it). Xcode's defaults record the device's screen for the whole session and
# keep every attachment, and a host runs for hours: that filled the internal
# disk with a video of whatever the device showed.
RECORDING_SETTINGS = {
    "PreferredScreenCaptureFormat": "screenshots",
    "SystemAttachmentLifetime": "keepNever",
    "UserAttachmentLifetime": "keepNever",
}
SCRIPT = pathlib.Path(__file__).resolve()
CHECKOUT = SCRIPT.parents[2]
sys.path.insert(0, str(CHECKOUT / "tests" / "integration" / "support"))
import ios_device_reports  # noqa: E402

# The processes whose diagnostic reports a host session needs free slots for:
# the applications it drives and its own runner.
REPORT_PROCESSES = (*ios_device_reports.APPLICATION_PROCESSES,
                    "KernariumAutomation-Runner")
# The process an application's reports are named for, by bundle identifier;
# any other bundle is taken to be named like the last part of its identifier.
BUNDLE_PROCESSES = {"com.imxieyi.Kernarium": "Kernarium",
                    "com.imxieyi.andarium": "Andarium"}
# The host operations that start an application on the device.
STARTING_COMMANDS = ("launch", "activate")

# XCTest types a key from the character it produces. On a device it delivers
# nothing for the characters of XCUIKeyboardKey.return ("\r"), .delete
# ("\x7f") and .escape ("\x1b"), or for a backspace ("\b"): no event reaches
# any responder of the application, a plain UITextField included, and the
# command still succeeds. "\n" produces the real Return key (HID usage 0x28),
# modifiers included. Key names therefore resolve to characters that arrive,
# and the characters that would be dropped are refused with a working route.
# The arrows are the iOS values of XCUIKeyboardKey, the arrow symbols; the
# macOS values, U+F700 to U+F703, press nothing on a device.
KEYS = {
    "return": "\n",
    "tab": "\t",
    "space": " ",
    "up": "\u2191",
    "down": "\u2193",
    "left": "\u2190",
    "right": "\u2192",
}
UNDELIVERABLE = {
    "delete": "XCTest drops a synthesized Delete key; `type $'\\x7f'` deletes "
              "through the text input system instead",
    "escape": "XCTest drops a synthesized Escape key; the terminal's key bar "
              "sends one with `tap --label Escape`",
}
# The key each dropped character stands for.
DROPPED = {"\r": "return", "\x7f": "delete", "\b": "delete", "\x1b": "escape"}
# The UIDeviceOrientation cases XCUIDevice can be set to.
ORIENTATIONS = ("portrait", "portraitUpsideDown", "landscapeLeft", "landscapeRight")


class NoHost(Exception):
    pass


def log(message):
    print(f"ipad-automation: {message}", file=sys.stderr, flush=True)


def devicectl(args, device, timeout):
    """Runs one devicectl device subcommand; returns (status, output)."""
    command = ["xcrun", "devicectl", "device", *args, "--device", device]
    try:
        done = subprocess.run(
            command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, f"devicectl timed out after {timeout} s"
    except OSError as error:
        return 127, f"devicectl cannot run: {error}"
    return done.returncode, done.stdout


def container_args(options):
    return ["--domain-type", "appDataContainer",
            "--domain-identifier", options.runner]


def pull(options, source, destination, timeout=60):
    status, _ = devicectl(
        ["copy", "from", "--source", source, "--destination", str(destination),
         *container_args(options)], options.device, timeout)
    return status == 0 and pathlib.Path(destination).exists()


def push(options, sources, destination, timeout=60):
    args = ["copy", "to"]
    for source in sources:
        args += ["--source", str(source)]
    status, output = devicectl(
        [*args, "--destination", destination, *container_args(options)],
        options.device, timeout)
    if status != 0:
        raise NoHost(f"cannot copy into the runner's container: {output.strip()[-400:]}")


def host_state(options):
    """The device-side heartbeat, or None when there is none."""
    with tempfile.TemporaryDirectory() as scratch:
        local = pathlib.Path(scratch) / "state.json"
        if not pull(options, f"{CHANNEL}/state.json", local):
            return None
        try:
            return json.loads(local.read_text())
        except ValueError:
            return None


def liveness(state):
    """Why the host is not serving, or None when it is."""
    if state is None:
        return "no heartbeat in the runner's container"
    if state.get("finished"):
        return "the host has finished"
    age = time.time() - state.get("time", 0)
    if age > HEARTBEAT_STALE:
        return f"the heartbeat is {age:.0f} s old"
    return None


def state_dir(options):
    directory = pathlib.Path(options.state_dir) / options.device
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def state_path(options):
    """The device's state directory, without creating it."""
    return pathlib.Path(options.state_dir) / options.device


def local_record(options):
    try:
        return json.loads((state_dir(options) / "host.json").read_text())
    except (OSError, ValueError):
        return {}


def recorded_supervisor(record):
    """The pid `start` recorded, while that process is still its supervisor.

    Nothing clears the record when the host ends, and by then the pid can
    name any process, including another agent's process-group leader. The
    pid is trusted only while its command line still carries `_supervise`
    and the record's nonce."""
    pid = record.get("supervisor")
    nonce = record.get("nonce")
    if not isinstance(pid, int) or pid <= 0 or not nonce:
        return None
    try:
        done = subprocess.run(
            ["ps", "-o", "command=", "-p", str(pid)], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    words = done.stdout.split()
    if done.returncode != 0 or "_supervise" not in words:
        return None
    for index, word in enumerate(words[:-1]):
        if word == "--nonce" and words[index + 1] == nonce:
            return pid
    return None


def end_process_group(process, wait=SUPERVISOR_TERM_WAIT):
    """Ends a supervisor this process started, and xcodebuild with it."""
    if process.poll() is not None:
        return
    for sig, limit in ((signal.SIGTERM, wait), (signal.SIGKILL, 10)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        try:
            process.wait(timeout=limit)
            return
        except subprocess.TimeoutExpired:
            continue
    process.poll()


def end_recorded_supervisor(record, wait=SUPERVISOR_TERM_WAIT):
    """Ends the supervisor `start` or `watch` recorded, if it still is one;
    the pid is checked again before each signal."""
    pid = recorded_supervisor(record)
    for sig, limit in ((signal.SIGTERM, wait), (signal.SIGKILL, 10)):
        if pid is None or recorded_supervisor(record) != pid:
            return
        try:
            os.killpg(pid, sig)
        except (ProcessLookupError, PermissionError):
            return
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            if recorded_supervisor(record) is None:
                return
            time.sleep(0.2)


class WatchLock:
    """The one-watch-per-device rule: an exclusive lock on `watch.lock` in the
    state directory, held while the watch runs and released by the system
    when its process ends, however it ends."""

    def __init__(self, options):
        self.path = state_dir(options) / "watch.lock"
        self.fd = None

    def acquire(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        # A command looking for the watch holds the lock for an instant.
        for _ in range(5):
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                time.sleep(0.05)
        else:
            os.close(fd)
            return False
        os.ftruncate(fd, 0)
        os.write(fd, f"{os.getpid()}\n".encode())
        self.fd = fd
        return True

    def release(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def watcher_pid(options):
    """The pid of the watch running for this device, or None; 0 when one runs
    and has not written its pid yet. The held lock is the proof of a running
    watch, never a pid in a file."""
    try:
        fd = os.open(state_path(options) / "watch.lock", os.O_RDONLY | os.O_CLOEXEC)
    except OSError:
        return None
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            return None
        for _ in range(10):
            text = os.pread(fd, 32, 0).decode(errors="replace").strip()
            if text.isdigit():
                return int(text)
            time.sleep(0.05)
        return 0
    finally:
        os.close(fd)


def read_watch_record(options):
    try:
        record = json.loads((state_path(options) / "watch.json").read_text())
    except (OSError, ValueError):
        return {}
    return record if isinstance(record, dict) else {}


class WatchRecord:
    """`watch.json` in the state directory: what `status` and a later `watch`
    read. Written whole and moved into place, so a reader never sees half."""

    def __init__(self, options):
        self.path = state_dir(options) / "watch.json"
        self.data = read_watch_record(options)

    def update(self, **fields):
        if fields.get("state", self.data.get("state")) != self.data.get("state"):
            fields["since"] = time.time()
        self.data.update(fields, updated=time.time())
        temporary = self.path.with_name(f"watch.json.{os.getpid()}")
        temporary.write_text(json.dumps(self.data))
        os.replace(temporary, self.path)


def forget_approval_failure(options):
    """A host started by hand with the owner at the device serves: the watch
    that failed for approval may run again."""
    if (read_watch_record(options).get("state") == NEEDS_APPROVAL
            and watcher_pid(options) is None):
        WatchRecord(options).update(
            state=STOPPED, detail="cleared by a `start` that served")


def watch_summary(options):
    """What `status` says of the watch."""
    pid = watcher_pid(options)
    record = read_watch_record(options)
    summary = {"running": pid is not None}
    if pid:
        summary["pid"] = pid
    state = record.get("state")
    if state:
        summary["state"] = state
        if state not in ACTIVE_STATES:
            summary["meaning"] = STATE_TEXT.get(state, state)
        elif pid is None:
            summary["meaning"] = "the watch ended without recording why"
        for key in ("detail", "restarts", "last_end"):
            if record.get(key) not in (None, ""):
                summary[key] = record[key]
        if record.get("since"):
            summary["since"] = time.strftime(
                "%Y-%m-%d %H:%M:%S", time.localtime(record["since"]))
    return summary


def wait_for_replacement(options):
    """With a watch running, a command waits while the watch says it is
    starting a host, or the host has not beaten for WATCH_FRESH s (the watch
    may not have noticed yet), until a host beats again or the watch ends; it
    returns at once, and fails as it always did, when no watch runs. The wait
    is bounded."""
    if watcher_pid(options) is None:
        return
    deadline = time.monotonic() + WATCH_COMMAND_WAIT
    announced = False
    while True:
        if (watcher_pid(options) is None
                or read_watch_record(options).get("state") not in ACTIVE_STATES):
            return
        starting = read_watch_record(options).get("state") != WATCHING
        state = host_state(options)
        if (not starting and state is not None and not state.get("finished")
                and time.time() - state.get("time", 0) <= WATCH_FRESH):
            return
        if time.monotonic() >= deadline:
            raise NoHost(f"the host was still being replaced after "
                         f"{WATCH_COMMAND_WAIT} s")
        if not announced:
            log("the host is not serving and its watch is starting another; "
                f"waiting up to {WATCH_COMMAND_WAIT} s")
            announced = True
        time.sleep(1)


def send(options, command, timeout):
    """Sends one command and waits at most `timeout` s for its result.

    A command already sent to a host that ends is not repeated for the
    replacement: it may have run."""
    wait_for_replacement(options)
    ident = f"{time.time_ns():020d}-{uuid.uuid4().hex[:8]}"
    command = dict(command, bundle=command.get("bundle") or options.bundle)
    deadline = time.monotonic() + timeout + 30
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        body = scratch / f"{ident}.json"
        marker = scratch / f"{ident}.ready"
        body.write_text(json.dumps(command))
        marker.write_text("")
        push(options, [body, marker], f"{CHANNEL}/inbox")
        result = scratch / "result.json"
        next_check = time.monotonic() + SEND_LIVENESS_EVERY
        while time.monotonic() < deadline:
            if pull(options, f"{CHANNEL}/outbox/{ident}.json", result):
                reply = json.loads(result.read_text())
                if "file" in reply:
                    attachment = scratch / reply["file"]
                    if not pull(options, f"{CHANNEL}/outbox/{reply['file']}", attachment, 120):
                        raise NoHost("the host wrote an attachment that cannot be copied")
                    reply["data"] = attachment.read_bytes()
                return reply
            if time.monotonic() >= next_check:
                reason = liveness(host_state(options))
                if reason is not None:
                    raise NoHost(f"the host is not running: {reason}" + (
                        "; its watch replaces it, but this command is not "
                        "repeated because it may have run"
                        if watcher_pid(options) is not None else ""))
                next_check = time.monotonic() + SEND_LIVENESS_EVERY
            time.sleep(0.5)
    raise NoHost(f"no result for {command['op']} within {timeout} s")


def command_status(options):
    state = host_state(options)
    reason = liveness(state)
    record = local_record(options)
    summary = {"serving": reason is None}
    if reason is not None:
        summary["reason"] = reason
    if state:
        now = time.time()
        summary.update({
            "heartbeat_age_s": round(now - state.get("time", 0), 1),
            "uptime_s": round(now - state.get("started", now)),
            "remaining_s": round(state.get("deadline", now) - now),
            "served": state.get("served"),
            "busy": state.get("command"),
        })
    if record:
        summary["xcodebuild_log"] = record.get("log")
        summary["result_bundle"] = record.get("result_bundle")
        summary["supervisor_running"] = recorded_supervisor(record) is not None
    summary["watch"] = watch_summary(options)
    print(json.dumps(summary, indent=2))
    return 0 if reason is None else EXIT_NO_HOST


def derived_data(options):
    return pathlib.Path(options.derived_data)


def command_build(options):
    command = [
        "xcodebuild", "build-for-testing",
        "-project", str(CHECKOUT / "Kernarium/Kernarium.xcodeproj"),
        "-scheme", "KernariumAutomation",
        "-destination", "generic/platform=iOS",
        "-derivedDataPath", str(derived_data(options)),
        "-allowProvisioningUpdates", *options.xcodebuild_arg]
    log("building the automation host")
    try:
        return subprocess.run(command, stdin=subprocess.DEVNULL,
                              timeout=options.timeout).returncode
    except subprocess.TimeoutExpired:
        log(f"the build took longer than {options.timeout} s")
        return EXIT_FAILED


def recording_problems(xctestrun):
    """What in the test run file lets Xcode record the session, as messages.

    Both run file formats are enumerated target by target, and every target
    must have a bundle path of its own and state each setting. A target of any
    other shape, such as one that runs the destination's installed bundle
    (UseDestinationArtifacts), is refused: this check cannot read its
    settings."""
    try:
        with open(xctestrun, "rb") as handle:
            plan = plistlib.load(handle)
    except (OSError, ValueError, expat.ExpatError) as error:
        return [f"{xctestrun} cannot be read: {error}"]
    if not isinstance(plan, dict):
        return [f"{xctestrun} is not a test run file"]
    metadata = plan.get("__xctestrun_metadata__")
    version = metadata.get("FormatVersion") if isinstance(metadata, dict) else None
    problems = set()
    targets = []
    if version == 2:
        configurations = plan.get("TestConfigurations")
        if not isinstance(configurations, list):
            return [f"{xctestrun} has no test configurations"]
        for configuration in configurations:
            entries = (configuration.get("TestTargets")
                       if isinstance(configuration, dict) else None)
            if not isinstance(entries, list):
                problems.add("a test configuration lists no targets")
                continue
            targets.extend(entries)
    elif version in (None, 1):
        targets = [value for key, value in plan.items()
                   if key != "__xctestrun_metadata__"]
    else:
        return [f"{xctestrun} is in a test run file format this check "
                "does not know"]
    if not targets:
        return [f"{xctestrun} names no test target"]
    for target in targets:
        if not isinstance(target, dict) or not isinstance(
                target.get("TestBundlePath"), str):
            problems.add("a test target has no bundle path of its own "
                         "(a destination-artifact or unknown target shape)")
            continue
        if target.get("UseDestinationArtifacts"):
            problems.add("a test target uses the destination's artifacts")
        for key, wanted in RECORDING_SETTINGS.items():
            if target.get(key) != wanted:
                problems.add(f"{key} is {target.get(key)!r}, not {wanted!r}")
    return sorted(problems)


def refuse_recording(xctestrun):
    """True, after saying why, when Xcode would record this run file's
    session."""
    problems = recording_problems(xctestrun)
    if problems:
        log(f"{xctestrun} does not turn Xcode's recording off "
            f"({'; '.join(problems)}): it would record the device's screen "
            "for the whole session and keep attachments; run `build` again "
            "with the current KernariumAutomation scheme")
    return bool(problems)


def check_report_slots(options, processes):
    """Makes room for diagnostic reports before something starts an
    application; False, after saying why, when it cannot."""
    try:
        ios_device_reports.preflight(options, [], extra_processes=processes,
                                     emit=log)
    except ios_device_reports.DeviceError as error:
        log(str(error))
        return False
    return True


def command_supervise(options):
    """Runs xcodebuild for the host's lifetime; started detached by `start`.

    The result bundle goes where `start` says, under the state directory,
    never to the default under the internal disk's DerivedData. This is where
    xcodebuild starts, so the run file and the report store are checked here,
    whoever called."""
    if refuse_recording(options.xctestrun):
        return EXIT_FAILED
    if not check_report_slots(options, REPORT_PROCESSES):
        return EXIT_FAILED
    environment = dict(os.environ,
                       TEST_RUNNER_KERNARIUM_AUTOMATION_HOST="1",
                       TEST_RUNNER_KERNARIUM_AUTOMATION_HOST_SECONDS=str(options.seconds),
                       TEST_RUNNER_KERNARIUM_AUTOMATION_NONCE=options.nonce)
    command = ["xcodebuild", "test-without-building",
               "-xctestrun", options.xctestrun,
               "-destination", f"platform=iOS,id={options.device}",
               "-resultBundlePath", options.result_bundle,
               f"-only-testing:{TEST}"]
    # xcodebuild stays in this process group, which `start` and `stop`
    # signal as a whole.
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, env=environment)
    try:
        status = process.wait(timeout=options.seconds + 900)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            status = process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            process.kill()
            status = process.wait(timeout=60)
    print(f"ipad-automation: xcodebuild exited with status {status}", flush=True)
    return status


class LaunchRefused(Exception):
    """Nothing was started. A permanent refusal is one that trying again
    cannot change."""

    def __init__(self, message, permanent=False):
        super().__init__(message)
        self.permanent = permanent


class Launch:
    """One host that `launch_host` started."""

    def __init__(self, process, log_path, nonce):
        self.process = process
        self.log_path = log_path
        self.nonce = nonce


def launch_host(options):
    """Starts a supervisor, and with it xcodebuild and a new host, and records
    it in `host.json`. Raises LaunchRefused, before anything starts, when the
    host cannot be started."""
    runs = sorted(derived_data(options).glob("Build/Products/KernariumAutomation_*.xctestrun"))
    if not runs:
        raise LaunchRefused(
            f"no built host under {derived_data(options)}; run `build` first",
            permanent=True)
    # The supervisor checks both again where xcodebuild starts; here the
    # caller sees a refusal before anything is started.
    if refuse_recording(runs[-1]):
        raise LaunchRefused("the built host's run file lets Xcode record the "
                            "device's screen", permanent=True)
    seconds = int(options.hours * 3600)
    nonce = uuid.uuid4().hex
    directory = state_dir(options)
    # Before anything starts: a kill during the session must leave a report.
    if not check_report_slots(options, REPORT_PROCESSES):
        raise LaunchRefused("the device's report store has too few free slots "
                            "or cannot be read")
    # The nonce keeps two starts in one second apart: xcodebuild refuses a
    # result bundle that exists.
    name = f"host-{time.strftime('%Y%m%d-%H%M%S')}-{nonce}"
    log_path = directory / f"{name}.log"
    result_bundle = directory / f"{name}.xcresult"
    reports = (["--device-reports-dir", str(options.device_reports_dir)]
               if options.device_reports_dir else [])
    if options.leave_device_reports:
        reports.append("--leave-device-reports")
    with open(log_path, "w") as output:
        supervisor = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--device", options.device,
             "--runner", options.runner, "--state-dir", options.state_dir,
             "_supervise", "--xctestrun", str(runs[-1]),
             "--result-bundle", str(result_bundle),
             "--seconds", str(seconds), "--nonce", nonce, *reports],
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
            start_new_session=True)
    (directory / "host.json").write_text(json.dumps({
        "supervisor": supervisor.pid, "log": str(log_path),
        "result_bundle": str(result_bundle), "nonce": nonce,
        "started": time.time(), "seconds": seconds}))
    return Launch(supervisor, log_path, nonce)


def command_start(options):
    if watcher_pid(options) is not None:
        log("a watch is running for this device: it replaces a host that ends, "
            "and a second host would ask for approval; `stop --force` ends "
            "the watch and the host")
        return EXIT_FAILED
    state = host_state(options)
    if liveness(state) is None:
        log("a host is already serving this device; use it or stop it first")
        return EXIT_FAILED
    try:
        launch = launch_host(options)
    except LaunchRefused as refusal:
        log(str(refusal))
        return EXIT_FAILED
    supervisor, log_path, nonce = launch.process, launch.log_path, launch.nonce
    log(f"started xcodebuild under supervisor {supervisor.pid}; log {log_path}")
    deadline = time.monotonic() + options.wait
    announced = False
    while time.monotonic() < deadline:
        if supervisor.poll() is not None:
            break
        if not announced and RUNNER_UP_LINE in log_path.read_text(errors="replace"):
            # XCTest gives up on enabling automation 60 s after this line.
            log("the runner is up: approve Enable UI Automation at the device within 60 s")
            announced = True
        state = host_state(options)
        if state and state.get("nonce") == nonce and liveness(state) is None:
            log("the host is serving")
            forget_approval_failure(options)
            return 0
        time.sleep(3)
    text = log_path.read_text(errors="replace")
    if APPROVAL_TIMEOUT_LINE in text:
        log("the device did not enable UI automation: the prompt was not approved "
            "within 60 s, and it stays on the screen until someone dismisses it")
    elif supervisor.poll() is not None:
        log("xcodebuild ended before the host served; see the log")
    else:
        log(f"the host did not serve within {options.wait} s; see the log")
    if supervisor.poll() is None:
        os.killpg(supervisor.pid, signal.SIGTERM)
    return EXIT_NO_HOST


def signal_watcher(pid):
    os.kill(pid, signal.SIGTERM)


def watcher_is_a_watch(pid):
    """Whether `pid` is a `watch` of this script, the check made before a
    signal: a lock holder's pid comes from a file."""
    try:
        done = subprocess.run(
            ["ps", "-o", "command=", "-p", str(pid)], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return False
    words = done.stdout.split()
    return done.returncode == 0 and "watch" in words and any(
        word.endswith(SCRIPT.name) for word in words)


def end_watch(options):
    """Ends the watch running for this device, before its host is stopped:
    left running it would start the host again. False, after saying why,
    when it cannot be ended, and then the host stays."""
    pid = watcher_pid(options)
    if pid is None:
        return True
    if pid <= 0 or not watcher_is_a_watch(pid):
        log(f"a watch holds the lock but pid {pid} is not one: not stopping the "
            "host, which that watch would start again")
        return False
    log(f"ending the watch (pid {pid}) first, so that it does not restart the host")
    try:
        signal_watcher(pid)
    except ProcessLookupError:
        pass
    except PermissionError:
        log(f"cannot signal the watch (pid {pid}): not stopping the host, "
            "which it would start again")
        return False
    deadline = time.monotonic() + WATCH_END_WAIT
    while time.monotonic() < deadline:
        if watcher_pid(options) is None:
            return True
        time.sleep(0.2)
    log(f"the watch (pid {pid}) did not end within {WATCH_END_WAIT} s: not "
        "stopping the host, which it would start again")
    return False


def command_stop(options):
    if not options.force:
        log("refusing to stop: automation stays approved only while a session runs, "
            "so a new host will ask for approval at the device again; pass --force "
            "when someone can approve it")
        return EXIT_FAILED
    if not end_watch(options):
        return EXIT_FAILED
    record = local_record(options)
    try:
        send(options, {"op": "stop"}, 30)
        log("the host stopped")
    except NoHost as error:
        log(str(error))
    supervisor = recorded_supervisor(record)
    if supervisor is None:
        return 0
    deadline = time.monotonic() + SUPERVISOR_EXIT_WAIT
    while time.monotonic() < deadline:
        if recorded_supervisor(record) is None:
            return 0
        time.sleep(1)
    if recorded_supervisor(record) is not None:
        log(f"terminating supervisor {supervisor}")
        try:
            os.killpg(supervisor, signal.SIGTERM)
        except ProcessLookupError:
            pass
    return 0


class LogScan:
    """Reads a host's xcodebuild log as it grows and keeps what the watch
    needs of it. A line is taken once it is complete."""

    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.runner_up = 0
        self.approval_timeout = False
        self.init_failure = None
        self.exit_status = None
        self.note = None

    def read(self):
        if not self.path:
            return
        try:
            with open(self.path, "rb") as handle:
                handle.seek(self.offset)
                data = handle.read()
        except OSError:
            return
        complete = data.rfind(b"\n") + 1
        self.offset += complete
        for line in data[:complete].decode(errors="replace").splitlines():
            self.take(line.rstrip())

    def take(self, line):
        if line.endswith(RUNNER_UP_LINE):
            self.runner_up += 1
        if APPROVAL_TIMEOUT_LINE in line:
            self.approval_timeout = True
        elif INIT_FAILURE_LINE in line:
            self.init_failure = line[line.index(INIT_FAILURE_LINE):][:160]
        if line.startswith(EXIT_STATUS_LINE):
            self.exit_status = line[len(EXIT_STATUS_LINE):].strip()
        elif line.startswith("ipad-automation:"):
            self.note = line[:200]

    def describe(self):
        """Why the host's run ended, as far as its log says."""
        parts = []
        if self.exit_status:
            parts.append(f"xcodebuild exited with status {self.exit_status}")
        if self.init_failure:
            parts.append(self.init_failure)
        elif self.note and not self.exit_status:
            parts.append(self.note)
        return "; ".join(parts) or "its log says nothing"


class HostObserver:
    """Tells, from what a host and its processes show, whether it still serves.

    A busy host is not a dead one: the host writes its heartbeat every 2 s from
    a queue that no command blocks, so a command running for minutes leaves it
    fresh, and only a heartbeat older than HEARTBEAT_STALE (or WATCH_GONE_AGE
    once the supervisor is gone, or xcodebuild started the runner again) ends a
    host. The processes only hurry that verdict: a runner that still beats
    serves, whatever happened to the process that started it."""

    def __init__(self, options, nonce, state, process=None, scan=None):
        self.options = options
        self.nonce = nonce
        self.state = state
        self.beat = state.get("time", 0)
        self.process = process
        self.scan = scan or LogScan(None)
        self.runners = self.scan.runner_up
        self.last_pull = time.monotonic()

    def supervisor_gone(self):
        if self.process is not None:
            return self.process.poll() is not None
        record = local_record(self.options)
        return bool(self.nonce) and record.get("nonce") == self.nonce and (
            recorded_supervisor(record) is None)

    def pull(self):
        self.last_pull = time.monotonic()
        state = host_state(self.options)
        if state is None:
            return
        nonce = state.get("nonce")
        if nonce != self.nonce:
            if not nonce or liveness(state) is not None:
                return  # what an earlier host left behind
            # Another host serves now: follow that one.
            record = local_record(self.options)
            self.nonce, self.process = nonce, None
            self.scan = LogScan(record.get("log") if record.get("nonce") == nonce else None)
            self.scan.read()
            self.runners = self.scan.runner_up
        self.state = state
        self.beat = state.get("time", 0)

    def check(self):
        """None while the host serves, else why it does not."""
        gone = self.supervisor_gone()
        self.scan.read()
        relaunched = self.scan.runner_up > self.runners
        quick = gone or relaunched
        if quick or time.monotonic() - self.last_pull >= WATCH_PULL_EVERY:
            self.pull()
        state = self.state
        if state.get("finished"):
            if state.get("time", 0) >= state.get("deadline", 0) - WATCH_GONE_AGE:
                return "the host reached its lifetime"
            return "the host reported that it finished"
        age = time.time() - self.beat
        if age > (WATCH_GONE_AGE if quick else HEARTBEAT_STALE):
            if gone:
                return (f"the supervisor ended ({self.scan.describe()}) and "
                        f"the heartbeat is {age:.0f} s old")
            if relaunched:
                return ("xcodebuild started the runner again, so the host's "
                        f"runner ended; the heartbeat is {age:.0f} s old")
            return f"the heartbeat is {age:.0f} s old"
        running = time.time() - state.get("commandStarted", time.time())
        if state.get("command") and running > WATCH_BUSY_LIMIT:
            return f"one command has run for {running:.0f} s: the host is hung"
        return None


class Watch:
    """Keeps one device's host serving; see `watch` in the module's help.

    `stop` is set to end it; the watch then leaves a serving host as it is."""

    def __init__(self, options, stop=None):
        self.options = options
        self.stop = stop or threading.Event()
        self.record = WatchRecord(options)
        self.previous = dict(self.record.data)
        self.observer = None
        self.restarts = 0
        self.restart_times = [t for t in self.previous.get("restart_times", [])
                              if isinstance(t, (int, float))]

    def say(self, message):
        log(f"watch {time.strftime('%H:%M:%S')}: {message}")

    def run(self):
        options = self.options
        self.record.update(state=STARTING, detail="", pid=os.getpid(),
                           started=time.time(), restarts=0,
                           restart_times=self.restart_times)
        state = host_state(options)
        refused = self.previous.get("state") == NEEDS_APPROVAL
        # The heartbeat of a host that ended a moment before the failure still
        # looks alive for HEARTBEAT_STALE: only a host started since counts.
        if liveness(state) is None and not (
                refused and state.get("started", 0) <= self.previous.get("since", 0)):
            self.adopt(state)
        else:
            self.await_starting_host()
        if self.observer is None and not self.stop.is_set():
            if refused:
                return self.refuse_after_approval_failure()
            code = self.replace("no host was serving when the watch started",
                                ended=False)
            if code is not None:
                return code
        while not self.stop.is_set():
            reason = self.observer.check()
            if reason is None:
                self.stop.wait(WATCH_POLL)
                continue
            code = self.replace(reason)
            if code is not None:
                return code
        return self.stopped()

    def adopt(self, state):
        """Takes over watching a host that already serves, touching nothing."""
        nonce = state.get("nonce")
        record = local_record(self.options)
        scan = LogScan(record.get("log") if record.get("nonce") == nonce else None)
        scan.read()
        self.observer = HostObserver(self.options, nonce, state, None, scan)
        now = time.time()
        self.say(f"adopting the host that serves (up {now - state.get('started', now):.0f} s, "
                 f"{state.get('deadline', now) - now:.0f} s left); it is not touched")
        self.record.update(state=WATCHING, detail="adopted a serving host",
                           host=nonce)

    def await_starting_host(self):
        """A host that a `start` began and that does not serve yet is that
        start's own to finish, and the owner may be approving it now: wait
        for it as `start` does before taking its place."""
        options = self.options
        record = local_record(options)
        started = record.get("started", 0)
        if recorded_supervisor(record) is None:
            return
        deadline = started + options.wait + WATCH_RUNNER_WAIT
        if time.time() > deadline:
            return
        self.say(f"a host that started {time.time() - started:.0f} s ago does not "
                 "serve yet; waiting for it")
        while not self.stop.is_set() and time.time() < deadline:
            state = host_state(options)
            if (liveness(state) is None and state.get("nonce") == record.get("nonce")):
                self.adopt(state)
                return
            if recorded_supervisor(record) is None:
                return
            self.stop.wait(WATCH_POLL)

    def retire(self):
        """Ends what is left of the host that stopped serving, so that no
        second session or prompt piles up beside its successor."""
        if self.observer is not None and self.observer.process is not None:
            end_process_group(self.observer.process)
        end_recorded_supervisor(local_record(self.options))

    def replace(self, reason, ended=True):
        """Starts the host that replaces one that stopped serving (`ended`
        is False for the first host, when none served). Returns the status
        the watch ends with, or None once the new host serves."""
        options = self.options
        now = time.time()
        if ended:
            self.say(f"the host stopped serving: {reason}")
            self.record.update(state=RESTARTING, detail=reason,
                               last_end={"reason": reason, "time": now})
        else:
            self.say(reason)
            self.record.update(state=STARTING, detail=reason)
        self.retire()
        recent = [t for t in self.restart_times if now - t < WATCH_STORM_WINDOW]
        if len(recent) >= options.max_restarts:
            return self.give_up(
                RESTART_STORM,
                f"{len(recent)} replacements in the last {WATCH_STORM_WINDOW // 60} "
                f"minutes, and the bound is {options.max_restarts}; the host ended "
                f"again: {reason}", EXIT_WATCH_GAVE_UP)
        detail = ""
        for number in range(1, WATCH_ATTEMPTS + 1):
            outcome = self.attempt()
            if outcome[0] == "served":
                self.observer = outcome[1]
                self.restarts += 1
                self.restart_times = recent + [time.time()]
                self.say(f"the replacement serves (restart {self.restarts})")
                self.record.update(state=WATCHING, detail="", host=self.observer.nonce,
                                   restarts=self.restarts,
                                   restart_times=self.restart_times)
                return None
            if outcome[0] == "stopped":
                return self.stopped()
            if outcome[0] == "approval":
                return self.give_up(
                    NEEDS_APPROVAL,
                    "the device asked for approval and XCTest gave up after its "
                    "60 s: nobody answered; run `start` with the owner at the "
                    "device, then `watch` again", EXIT_NEEDS_APPROVAL)
            _, detail, permanent = outcome
            if permanent:
                return self.give_up(CANNOT_START, detail, EXIT_WATCH_GAVE_UP)
            self.say(f"attempt {number} of {WATCH_ATTEMPTS} failed: {detail}")
            self.record.update(detail=f"attempt {number} failed: {detail}")
            if number < WATCH_ATTEMPTS and self.stop.wait(WATCH_BACKOFF[number - 1]):
                return self.stopped()
        return self.give_up(
            START_FAILED, f"{WATCH_ATTEMPTS} attempts failed; the last: {detail}",
            EXIT_WATCH_GAVE_UP)

    def attempt(self):
        """One start of a host: ("served", observer), ("approval",),
        ("stopped",) or ("failed", why, permanent).

        The approval prompt is told from every other failure by xcodebuild's
        own line, "Timed out while enabling automation mode": XCTest prints it
        when the prompt stayed unanswered for its 60 s, so it means the device
        asked. A runner that is up and has neither served nor printed it
        within WATCH_RUNNER_WAIT is not waiting for a prompt; that, a runner
        that never comes up and an xcodebuild that ends first are ordinary
        failures, which are retried."""
        options = self.options
        try:
            launch = launch_host(options)
        except LaunchRefused as refusal:
            return ("failed", str(refusal), refusal.permanent)
        self.say(f"started host {launch.nonce[:8]} under supervisor "
                 f"{launch.process.pid}; log {launch.log_path}")
        scan = LogScan(launch.log_path)
        deadline = time.monotonic() + options.wait
        runner_up = False
        while True:
            if self.stop.is_set():
                end_process_group(launch.process)
                return ("stopped",)
            scan.read()
            if scan.approval_timeout:
                end_process_group(launch.process)
                return ("approval",)
            state = host_state(options)
            if (state and state.get("nonce") == launch.nonce
                    and liveness(state) is None):
                # The runner's line came before its first beat: read it now,
                # so that it is not counted as a second runner later.
                scan.read()
                return ("served", HostObserver(
                    options, launch.nonce, state, launch.process, scan))
            if launch.process.poll() is not None:
                scan.read()
                if scan.approval_timeout:
                    return ("approval",)
                return ("failed", f"xcodebuild ended before the host served "
                        f"({scan.describe()}); log {launch.log_path}", False)
            if scan.runner_up and not runner_up:
                runner_up = True
                deadline = time.monotonic() + WATCH_RUNNER_WAIT
            if time.monotonic() > deadline:
                end_process_group(launch.process)
                why = (f"the runner was up but did not serve within "
                       f"{WATCH_RUNNER_WAIT} s" if runner_up else
                       f"the runner did not come up within {options.wait} s")
                return ("failed", f"{why}; log {launch.log_path}", False)
            self.stop.wait(WATCH_POLL)

    def give_up(self, state, detail, code):
        self.say(f"{STATE_TEXT[state]}: {detail}")
        self.record.update(state=state, detail=detail)
        return code

    def refuse_after_approval_failure(self):
        """A watch that failed for approval is not run again by whatever
        starts it again (a job manager, a loop): each try puts another prompt
        on the device. A host that serves ends the refusal."""
        detail = self.previous.get("detail", "")
        self.say("the last watch failed because the device needs approval, and "
                 "no host serves: run `start` with the owner at the device, "
                 f"then `watch` again; starting nothing. {detail}")
        self.record.update(state=NEEDS_APPROVAL, detail=detail,
                           since=self.previous.get("since", time.time()))
        return EXIT_NEEDS_APPROVAL

    def stopped(self):
        self.say("ending on request; a serving host is left as it is")
        self.record.update(state=STOPPED, detail="ended on request")
        return 0


def launchd_path():
    """The PATH a job manager's job needs: where this shell finds the device
    tools, then the system's. A job inherits no login shell's."""
    directories = []
    for tool in ("xcrun", "xcodebuild", ios_device_reports.MOVE_TOOL):
        found = shutil.which(tool)
        if found:
            directories.append(str(pathlib.Path(found).parent))
    directories += ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]
    return ":".join(dict.fromkeys(directories))


def launchd_job(options, label):
    """The launchd job description that runs this watch.

    The job runs the same foreground `watch` and relies on nothing of the
    environment it is made in: absolute interpreter, script, state directory
    and derived data, and a PATH built for it. launchd starts it once and
    restarts it only after a crash, a signal the process did not handle:
    every failure the watch itself reports (approval, giving up) exits with a
    status, and a job that came back after those would put a new prompt on the
    device. Processes the watch started are left running when it ends."""
    state = pathlib.Path(options.state_dir).resolve()
    arguments = [sys.executable, str(SCRIPT), "--device", options.device,
                 "--state-dir", str(state),
                 "--derived-data", str(pathlib.Path(options.derived_data).resolve())]
    if options.runner != RUNNER:
        arguments += ["--runner", options.runner]
    arguments += ["watch", "--hours", f"{options.hours:g}",
                  "--wait", str(options.wait),
                  "--max-restarts", str(options.max_restarts)]
    if options.device_reports_dir:
        arguments += ["--device-reports-dir", str(options.device_reports_dir)]
    if options.leave_device_reports:
        arguments.append("--leave-device-reports")
    environment = {"PATH": launchd_path()}
    if os.environ.get("DEVELOPER_DIR"):
        environment["DEVELOPER_DIR"] = os.environ["DEVELOPER_DIR"]
    log_path = str(state / options.device / "watch-launchd.log")
    return {
        "Label": label,
        "ProgramArguments": arguments,
        "EnvironmentVariables": environment,
        "RunAtLoad": True,
        "KeepAlive": {"Crashed": True},
        "ThrottleInterval": 30,
        "AbandonProcessGroup": True,
        "LimitLoadToSessionType": "Aqua",
        "StandardOutPath": log_path,
        "StandardErrorPath": log_path,
    }


def detach_watch(options):
    """Runs the same `watch` in a session of its own, with its output in the
    state directory, and returns once it holds the watch lock."""
    if watcher_pid(options) is not None:
        log("a watch already runs for this device and state directory")
        return EXIT_WATCH_LOCKED
    arguments = [word for word in options.argv if word != "--detach"]
    directory = state_dir(options)
    log_path = directory / f"watch-{time.strftime('%Y%m%d-%H%M%S')}.log"
    with open(log_path, "ab") as output:
        child = subprocess.Popen(
            [sys.executable, str(SCRIPT), *arguments], stdin=subprocess.DEVNULL,
            stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic() + WATCH_DETACH_WAIT
    while time.monotonic() < deadline:
        status = child.poll()
        if status is not None:
            log(f"the watch ended at once with status {status}; see {log_path}")
            return status
        if watcher_pid(options) == child.pid:
            log(f"watching under pid {child.pid}; log {log_path}")
            return 0
        time.sleep(0.2)
    log(f"the watch (pid {child.pid}) did not take its lock within "
        f"{WATCH_DETACH_WAIT} s; see {log_path}")
    return EXIT_FAILED


def command_watch(options, stop=None):
    if options.print_launchd_job:
        sys.stdout.write(plistlib.dumps(
            launchd_job(options, options.print_launchd_job)).decode())
        return 0
    if options.detach:
        return detach_watch(options)
    lock = WatchLock(options)
    if not lock.acquire():
        log("another watch already runs for this device and state directory")
        return EXIT_WATCH_LOCKED
    stop = stop or threading.Event()
    saved = {}
    if threading.current_thread() is threading.main_thread():
        for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            saved[number] = signal.signal(number, lambda *_: stop.set())
    watch = None
    try:
        watch = Watch(options, stop)
        return watch.run()
    except Exception as error:  # the watch must say why it ended
        log(traceback.format_exc().rstrip())
        if watch is not None:
            watch.record.update(state=WATCH_ERROR,
                                detail=f"{type(error).__name__}: {error}")
        return EXIT_FAILED
    finally:
        for number, handler in saved.items():
            signal.signal(number, handler)
        lock.release()


def element_fields(options):
    fields = {}
    if options.id:
        fields["id"] = options.id
    if options.label:
        fields["label"] = options.label
    return fields


def key_character(key):
    """The character the host types for a `key` argument: a key name or one
    character. Raises ValueError for a key XCTest would silently drop."""
    key = DROPPED.get(key, key)
    if key in UNDELIVERABLE:
        raise ValueError(UNDELIVERABLE[key])
    if key in KEYS:
        return KEYS[key]
    # XCTest's typeKey raises for anything but one UTF-16 code unit, so a
    # character outside the Basic Multilingual Plane is refused as well.
    if len(key.encode("utf-16-le")) != 2:
        raise ValueError(f"unknown key {key!r}: name one of "
                         f"{', '.join(sorted(KEYS))} or give one character")
    return key


def build_command(options):
    """The host command for an application operation."""
    op = options.command
    command = {"op": op}
    if op == "launch":
        command["environment"] = dict(item.split("=", 1) for item in options.env)
        command["arguments"] = options.arg
    elif op in ("tap", "wait", "exists"):
        command.update(element_fields(options))
        if op == "tap" and options.x is not None:
            command.update(x=options.x, y=options.y)
        if op == "wait":
            command["gone"] = options.gone
        command["timeout"] = options.element_timeout
    elif op in ("type", "siri"):
        command["text"] = options.text
    elif op == "key":
        command["key"] = key_character(options.key)
        command["modifiers"] = [item for item in options.modifiers.split(",") if item]
    elif op == "orientation":
        if options.orientation is not None:
            command["orientation"] = options.orientation
    elif op == "drag":
        if options.seconds < 0 or options.hold < 0:
            raise ValueError("a drag's press and hold durations cannot be negative")
        command.update(x1=options.x1, y1=options.y1, x2=options.x2, y2=options.y2,
                       press=options.seconds, hold=options.hold)
        if options.velocity is not None:
            if options.velocity <= 0:
                raise ValueError("a drag's velocity is a positive number of points per second")
            command["velocity"] = options.velocity
    return command


def bundle_process(bundle):
    """The process name the reports of an application are filed under."""
    return BUNDLE_PROCESSES.get(bundle) or bundle.rsplit(".", 1)[-1]


def run_op(options):
    op = options.command
    try:
        command = build_command(options)
    except ValueError as error:
        log(str(error))
        return EXIT_FAILED
    if op in STARTING_COMMANDS and not check_report_slots(
            options, [bundle_process(command.get("bundle") or options.bundle)]):
        return EXIT_FAILED
    reply = send(options, command, options.timeout)
    data = reply.pop("data", None)
    if op == "screenshot" and data is not None:
        pathlib.Path(options.output).write_bytes(data)
        reply["file"] = options.output
    if op == "tree" and reply.get("ok"):
        print(reply.pop("tree"))
    print(json.dumps(reply))
    return 0 if reply.get("ok") else EXIT_FAILED


def parse(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default=os.environ.get("KERNARIUM_AUTOMATION_DEVICE"),
                        help="device UDID (default: $KERNARIUM_AUTOMATION_DEVICE)")
    parser.add_argument("--runner", default=RUNNER, help=argparse.SUPPRESS)
    parser.add_argument("--bundle", default=KERNARIUM,
                        help=f"application a command acts on (default: {KERNARIUM})")
    parser.add_argument("--state-dir", default=os.environ.get(
        "KERNARIUM_AUTOMATION_STATE_DIR", str(CHECKOUT / ".build/kernarium-automation")),
        help="where `start` keeps the host's xcodebuild log, result bundle "
                             "and supervisor pid")
    parser.add_argument("--derived-data", default=str(CHECKOUT / ".build/kernarium-automation/derived"),
                        help="derived data of the host build")
    parser.add_argument("--timeout", type=int, default=120,
                        help="seconds to wait for a command's result (default: 120)")
    commands = parser.add_subparsers(dest="command", required=True)

    build = commands.add_parser("build", help="build the host for iOS devices")
    build.add_argument("--xcodebuild-arg", action="append", default=[],
                       help="extra xcodebuild argument, such as a package cache path")
    start = commands.add_parser("start", help="start the host; the device may ask for approval")
    start.add_argument("--hours", type=float, default=8, help="host lifetime (default: 8)")
    start.add_argument("--wait", type=int, default=240,
                       help="seconds to wait for the host to serve (default: 240)")
    ios_device_reports.add_arguments(start)
    supervise = commands.add_parser("_supervise")
    supervise.add_argument("--xctestrun", required=True)
    supervise.add_argument("--result-bundle", required=True)
    supervise.add_argument("--seconds", type=int, required=True)
    supervise.add_argument("--nonce", required=True)
    ios_device_reports.add_arguments(supervise)
    watch = commands.add_parser(
        "watch", help="keep the host serving: start the next one the moment it ends")
    watch.add_argument("--hours", type=float, default=8,
                       help="lifetime of each host the watch starts (default: 8)")
    watch.add_argument("--wait", type=int, default=240,
                       help="seconds to wait for a new host's runner to come up "
                            "(default: 240)")
    watch.add_argument("--max-restarts", type=int, default=WATCH_MAX_RESTARTS,
                       help="replacements allowed per hour (default: "
                            f"{WATCH_MAX_RESTARTS})")
    watch.add_argument("--detach", action="store_true",
                       help="run the watch in a session of its own and return")
    watch.add_argument("--print-launchd-job", metavar="LABEL",
                       help="print the launchd job that runs this watch, and exit")
    ios_device_reports.add_arguments(watch)
    commands.add_parser("status", help="report whether the host is serving")
    stop = commands.add_parser(
        "stop", help="end the host session; starting another needs a new approval")
    stop.add_argument("--force", action="store_true",
                      help="stop even though a new host must be approved at the device")

    commands.add_parser("ping", help="round-trip an empty command")
    launch = commands.add_parser("launch", help="launch (or relaunch) the application")
    launch.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    launch.add_argument("--arg", action="append", default=[])
    ios_device_reports.add_arguments(launch)
    for name, text in (("activate", "bring the application to the foreground"),
                       ("terminate", "terminate the application"),
                       ("state", "report the application's run state"),
                       ("tree", "print the application's element tree")):
        sub = commands.add_parser(name, help=text)
        if name == "activate":
            ios_device_reports.add_arguments(sub)
    for name, text in (("tap", "tap an element or a point in the application"),
                       ("wait", "wait for an element to appear or disappear"),
                       ("exists", "report whether an element exists")):
        sub = commands.add_parser(name, help=text)
        sub.add_argument("--id", help="accessibility identifier")
        sub.add_argument("--label", help="accessibility label")
        sub.add_argument("--element-timeout", type=float, default=10)
        if name == "tap":
            sub.add_argument("--x", type=float, help="point offset from the application's origin")
            sub.add_argument("--y", type=float)
        if name == "wait":
            sub.add_argument("--gone", action="store_true", help="wait for disappearance")
    drag = commands.add_parser(
        "drag", help="press a point, drag it to another and lift (points from the "
                     "application's origin)")
    for name in ("x1", "y1", "x2", "y2"):
        drag.add_argument(name, type=float)
    drag.add_argument("seconds", type=float, nargs="?", default=0.5,
                      help="how long to press before dragging (default: 0.5)")
    drag.add_argument("--velocity", type=float,
                      help="drag speed in points per second (default: XCTest's)")
    drag.add_argument("--hold", type=float, default=0.0,
                      help="seconds to hold at the end before lifting (default: 0)")
    orientation = commands.add_parser(
        "orientation", help="rotate the device, or report its orientation")
    orientation.add_argument("orientation", nargs="?", choices=ORIENTATIONS)
    type_ = commands.add_parser("type", help="type text into the focused element")
    type_.add_argument("text")
    siri = commands.add_parser(
        "siri", help="start a Siri request with typed text (interrupts the app's audio)")
    siri.add_argument("text")
    key = commands.add_parser("key", help="press one hardware-keyboard key")
    key.add_argument("key", help="one character, or return, tab, space, up, down, left, "
                                 "right; XCTest cannot press delete or escape")
    key.add_argument("--modifiers", default="", help="comma list of shift, control, option, command")
    shot = commands.add_parser("screenshot", help="save a screenshot of the device")
    shot.add_argument("-o", "--output", required=True)

    options = parser.parse_args(argv)
    options.argv = list(argv)
    if not options.device:
        parser.error("name the device with --device or $KERNARIUM_AUTOMATION_DEVICE")
    if options.command == "tap" and (options.x is None) != (options.y is None):
        parser.error("tap needs both --x and --y")
    return options


def main(argv):
    options = parse(argv)
    handlers = {"build": command_build, "start": command_start, "stop": command_stop,
                "status": command_status, "watch": command_watch,
                "_supervise": command_supervise}
    try:
        handler = handlers.get(options.command, run_op)
        if options.command == "build":
            options.timeout = max(options.timeout, 1800)
        return handler(options)
    except NoHost as error:
        log(str(error))
        return EXIT_NO_HOST


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
