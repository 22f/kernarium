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
  mkdir -p STATE_DIR/UDID
  ipad-automation.py --device UDID watch --print-launchd-job LABEL > job.plist

The same `watch` runs in a terminal's foreground, detached by `--detach` (its
log is watch-<time>.log in the state directory), or as a job manager's job
(`--print-launchd-job` prints a launchd job with absolute paths and its own
PATH, which runs once and comes back only after a crash; printing creates
nothing, so the `mkdir` above, for the job's log directory, comes before the
job loads, and the job's registration is the owner's). It adopts a host that
serves (its heartbeat advances between two reads) and never stops or restarts
one that does. A host stops serving when the watch has read the same heartbeat
for 20 s (a busy host keeps beating: its heartbeat comes from a queue no
command blocks), or for 5 s once its supervisor ended or xcodebuild started
the runner again, when its lifetime ended, or when one command has run longer
than 30 minutes and than the process that sent it waits for. A read that fails
or comes late, and a sleep of the Mac, are not a host's end: only reads that
worked count, and a host that cannot be seen is left alone. The next host
starts at once with the watch's own options, after ending the whole of the old
one (its supervisor, xcodebuild and what else is in their process group; one
that will not end stops the watch with the state cannot-start); detection takes
25 s at most, plus the time of the device reads.

A replacement that the device wants approved is not retried: xcodebuild's line
"Timed out while enabling automation mode" (XCTest's own 60 s give-up), or the
same "Failed to initialize for UI testing" line with a LocalAuthentication
error of code -4 or -2 (the owner's authentication cancelled, by the system or
the owner), ends the watch with status 4 and the state needs-approval, which
`status` reports. So does that line from the runner xcodebuild starts again
after a host's runner died, which the watch waits for before it replaces the
host. A host's heartbeat is written by the test that runs only after XCTest
initialized the runner for UI testing, so a serving host has its approval; the
next runner after it dies may not.
A host the watch ended before its log could say whether the device asked, in
the 60 s after the runner came up, counts the same. The refusal is the file
approval.json in the state directory, which another watch starting, stopping
or failing leaves as it is: a new `watch` starts nothing until a host serves,
as after a `start` with the owner at the device. Any other failure to start
(device not connected or locked, report slots short, a runner that does not
serve within 90 s of coming up) is retried three times after 5, 10 and 20 s,
then ends the watch with status 5 and the state start-failed. No built host, or
a run file that lets Xcode record the screen, ends it at once with the state
cannot-start; more than 6 replacements (--max-restarts) in an hour, counted
across a watch that ended mid-replacement too, end it with restart-storm.
Status 6: another watch holds this device's lock. `status` shows the watch and
its last end. A watch also waits for a host that a `start` is still bringing up,
for as long as that `start` waits, and `start` and `watch` launch one at a time.

A command sent while the watch is starting a host, or has not seen the host
beat for 15 s, waits for a host that beats, up to 180 s (a hard bound), and
fails with status 3 as before when no watch runs or the watch has ended. A
command that is running or waiting when its host ends is not repeated for the
replacement, which starts with an empty inbox: it may have run, and its
sender gets the same status 3. `stop --force` ends the watch first (only the
process that holds this device's watch lock), so that it does not start the host
again, and then the host; `start` refuses while a watch runs. Ending the watch
alone (SIGTERM, or the job manager's stop) leaves a serving host running.

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
import re
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
# The host beats every 2 s from a queue its commands never block. A host is
# dead when the watch has read the same beat for this long: WATCH_GONE_AGE once
# its supervisor ended or xcodebuild started the runner again, HEARTBEAT_STALE
# otherwise. Only reads that succeed count: a read that fails says nothing.
WATCH_GONE_AGE = 5
# One command that has run longer than this, and longer than the process that
# sent it waits for it, is a host whose main thread is hung.
WATCH_BUSY_LIMIT = 1800
# The wall clock gaining this much on the monotonic one (which stops while the
# Mac sleeps) is a sleep or a clock change: what the watch saw before it says
# nothing about the host now.
WATCH_CLOCK_GAP = 10
# A host is adopted when its beat advances between two of this many reads,
# this far apart: the last beat of a host that just ended stands still.
WATCH_ADOPT_PULLS = 3
WATCH_ADOPT_GAP = 2.5
# After "Running tests..." XCTest gives the device's approval prompt 60 s; a
# runner that has neither served nor timed out by then is not waiting for one.
WATCH_RUNNER_WAIT = 90
# How long after the runner came up a host that ends before it says anything
# may still have been waiting for an approval: XCTest's 60 s and a margin.
WATCH_APPROVAL_WINDOW = 75
# Attempts at one replacement, and the pauses after each failed one. The
# approval prompt is skipped when the previous host ended minutes earlier, so
# the pauses stay short.
WATCH_ATTEMPTS = 4
WATCH_BACKOFF = (5, 10, 20)
# More replacements than this in WATCH_STORM_WINDOW end the watch.
WATCH_MAX_RESTARTS = 6
WATCH_STORM_WINDOW = 3600
# How long a command waits for a replacement the watch is making (a hard
# bound), how recently the watch must have seen the host's beat advance for a
# command to be sent, and how long `stop --force` lets the watch end and
# `watch --detach` lets the new watch take its lock.
WATCH_COMMAND_WAIT = 180
WATCH_ALIVE_FRESH = 15
WATCH_END_WAIT = 90
WATCH_DETACH_WAIT = 20
# How often a command waiting for its result looks at the host's heartbeat.
SEND_LIVENESS_EVERY = 10
# How long a launch waits for another launch of the same device (`start` or
# the watch) to finish its transition.
LAUNCH_LOCK_WAIT = 300
# How long a supervisor lets xcodebuild, and then what else is in its process
# group, obey SIGTERM before SIGKILL; and how long what ends a supervisor
# waits for it to be done (its two waits, with room).
CHILD_TERM_WAIT = 10
SUPERVISOR_TERM_WAIT = 30
# xcodebuild's own lines: the runner is up, and XCTest gave up on the prompt.
RUNNER_UP_LINE = "Running tests..."
APPROVAL_TIMEOUT_LINE = "Timed out while enabling automation mode"
INIT_FAILURE_LINE = "Failed to initialize for UI testing"
# The same line when the owner's authentication for it was cancelled: by the
# owner (LAError -2) or by the system (-4, "UI canceled by system", as when the
# screen locks during the prompt). The description is localized; the domain
# and the code are not.
AUTHENTICATION_CANCELLED = re.compile(
    r"Error Domain=com\.apple\.LocalAuthentication Code=-[24]\b")
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


class Clock:
    """The time the watch measures with; a test steps its own by hand."""

    def monotonic(self):
        return time.monotonic()

    def time(self):
        return time.time()

    def sleep(self, seconds):
        time.sleep(seconds)

    def wait(self, event, seconds):
        """True when `event` is set within `seconds`."""
        return event.wait(seconds)


CLOCK = Clock()


def write_json(path, data):
    """Writes `data` whole and moves it into place: a reader never sees half
    of it."""
    temporary = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}")
    temporary.write_text(json.dumps(data))
    os.replace(temporary, path)


def read_json(path):
    try:
        value = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    return value if isinstance(value, dict) else {}


def host_state(options, timeout=60):
    """The device-side heartbeat, or None when there is none (or it cannot be
    read: the two are not told apart)."""
    with tempfile.TemporaryDirectory() as scratch:
        local = pathlib.Path(scratch) / "state.json"
        if not pull(options, f"{CHANNEL}/state.json", local, timeout):
            return None
        try:
            state = json.loads(local.read_text())
        except ValueError:
            return None
        return state if isinstance(state, dict) else None


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
    return read_json(state_dir(options) / "host.json")


def process_table():
    """Every process as (pid, pgid, stat, command); None when `ps` fails."""
    try:
        done = subprocess.run(
            ["ps", "-ax", "-o", "pid=,pgid=,stat=,command="],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, errors="replace", timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if done.returncode != 0:
        return None
    rows = []
    for line in done.stdout.splitlines():
        parts = line.split(None, 3)
        if len(parts) >= 3 and parts[0].isdigit() and parts[1].isdigit():
            rows.append((int(parts[0]), int(parts[1]), parts[2],
                         parts[3] if len(parts) > 3 else ""))
    return rows


def group_members(pgid):
    """The live processes of a process group as (pid, command), without the
    `ps` that lists them; None when the processes cannot be listed."""
    table = process_table()
    if table is None:
        return None
    members = []
    for pid, group, stat, command in table:
        words = command.split()
        if group != pgid or stat.startswith("Z"):
            continue
        if len(words) > 1 and os.path.basename(words[0]) == "ps" and words[1] == "-ax":
            continue
        members.append((pid, command))
    return members


def process_instance(pid):
    """(start time, command) of a process: the start time tells it from a
    later process that has the same pid. None when there is none."""
    try:
        done = subprocess.run(
            ["ps", "-o", "lstart=", "-o", "command=", "-p", str(pid)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, errors="replace",
            env=dict(os.environ, LC_ALL="C"), timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    line = done.stdout.rstrip("\n")
    if done.returncode != 0 or len(line) < 25:
        return None
    return line[:24], line[25:].strip()


def find_supervisor(nonce):
    """The pid of the supervisor that was started with this nonce, or None."""
    for pid, group, stat, command in process_table() or []:
        words = command.split()
        if stat.startswith("Z") or "_supervise" not in words:
            continue
        for index, word in enumerate(words[:-1]):
            if word == "--nonce" and words[index + 1] == nonce:
                return pid
    return None


def recorded_supervisor(record):
    """The pid `start` recorded, while that process is still its supervisor.

    Nothing clears the record when the host ends, and by then the pid can
    name any process, including another agent's process-group leader. The
    pid is trusted only while its command line still carries `_supervise`
    and the record's nonce."""
    pid = record.get("supervisor")
    nonce = record.get("nonce")
    if not nonce:
        return None
    if pid is None:
        # The launch that wrote the record ended before it knew the pid.
        return find_supervisor(nonce)
    if not isinstance(pid, int) or pid <= 0:
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


# What end_host found: nothing of the host remains; a process group the
# record names holds nothing that shows it is the host's, and is left alone;
# or something that is the host's would not end.
ENDED, FOREIGN, STUCK = "ended", "foreign", "stuck"


def end_host(record, process=None, wait=None):
    """Ends the whole of a host: its supervisor, xcodebuild and what else is
    in their process group, with SIGTERM and then SIGKILL, and returns once
    the group is empty.

    The group is the supervisor's: `start_new_session` made its pid the group
    id, and no pid is reused while a group of that id has members. A group is
    signalled only while `process`, the supervisor this process started and
    has not reaped, holds the pid, or while a member's command line carries
    the record's nonce (the supervisor's `--nonce` and xcodebuild's result
    bundle name): that is ownership an orphaned remainder can still show. A
    group with no such member is left alone (FOREIGN)."""
    nonce = record.get("nonce")
    pid = (process.pid if process is not None
           else record.get("supervisor") or (nonce and find_supervisor(nonce)))
    if not isinstance(pid, int) or pid <= 1:
        return ENDED
    signalled = False
    for sig, limit in ((signal.SIGTERM, wait or SUPERVISOR_TERM_WAIT),
                       (signal.SIGKILL, 10)):
        if process is not None:
            process.poll()
        members = group_members(pid)
        if members is None:
            return STUCK
        if not members:
            return ENDED
        ours = (process is not None and process.returncode is None) or (
            bool(nonce) and any(nonce in command for _, command in members))
        if not ours:
            return ENDED if signalled else FOREIGN
        try:
            os.killpg(pid, sig)
        except ProcessLookupError:
            pass
        except PermissionError:
            return FOREIGN
        signalled = True
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            if process is not None:
                process.poll()
            if not group_members(pid):
                return ENDED
            time.sleep(0.2)
    if process is not None:
        process.poll()
    return ENDED if not group_members(pid) else STUCK


class WatchLock:
    """The one-watch-per-device rule: an exclusive lock on `watch.lock` in the
    state directory, held while the watch runs and released by the system
    when its process ends, however it ends. The file then says who holds it:
    pid, the process's start time (which tells it from a later process with
    the pid), device and state directory; the lock itself is the proof that
    some watch runs, and these say which."""

    def __init__(self, options):
        self.options = options
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
        instance = process_instance(os.getpid())
        identity = {"pid": os.getpid(), "start": instance[0] if instance else "",
                    "device": self.options.device,
                    "state": os.path.realpath(state_path(self.options))}
        os.ftruncate(fd, 0)
        os.pwrite(fd, (json.dumps(identity) + "\n").encode(), 0)
        self.fd = fd
        return True

    def release(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


def lock_owner(fd, options):
    """The pid of the process the lock file names, when that process is the
    one that wrote it for this device and state directory; else None. Until
    the holder has written, the file still names a watch that ended, whose pid
    may belong to any process now."""
    try:
        identity = json.loads(os.pread(fd, 512, 0).decode(errors="replace"))
    except ValueError:
        return None
    if not isinstance(identity, dict):
        return None
    pid = identity.get("pid")
    if (not isinstance(pid, int) or pid <= 0 or not identity.get("start")
            or identity.get("device") != options.device
            or identity.get("state") != os.path.realpath(state_path(options))):
        return None
    instance = process_instance(pid)
    if instance is None or instance[0] != identity["start"]:
        return None
    return pid


def watcher_pid(options):
    """The pid of the watch running for this device, or None; 0 when one runs
    and cannot be identified (it may not have written yet). The held lock is
    the proof of a running watch, never a pid in a file."""
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
        for _ in range(20):
            pid = lock_owner(fd, options)
            if pid:
                return pid
            time.sleep(0.05)
        return 0
    finally:
        os.close(fd)


def read_watch_record(options):
    return read_json(state_path(options) / "watch.json")


class WatchRecord:
    """`watch.json` in the state directory: what `status` and a later `watch`
    read."""

    def __init__(self, options):
        self.path = state_dir(options) / "watch.json"
        self.data = read_watch_record(options)

    def update(self, **fields):
        if fields.get("state", self.data.get("state")) != self.data.get("state"):
            fields["since"] = time.time()
        self.data.update(fields, updated=time.time())
        write_json(self.path, self.data)


class ApprovalLatch:
    """`approval.json` in the state directory: the device may be asking for
    an approval nobody gave, so no watch starts a host until one serves. It
    is set when an attempt shows XCTest's timeout ("observed") or ended where
    that could not be told ("possible"), and cleared only by a host that
    serves: a stopped, failed or restarted watch leaves it as it is."""

    def __init__(self, options):
        self.options = options

    def path(self):
        return state_path(self.options) / "approval.json"

    def read(self):
        return read_json(self.path()) or None

    def set(self, kind, nonce, log_path, detail):
        existing = self.read() or {}
        if existing.get("kind") == "observed":
            kind = "observed"
        write_json(state_dir(self.options) / "approval.json", {
            "kind": kind, "since": existing.get("since", time.time()),
            "nonce": nonce, "log": str(log_path), "detail": detail})

    def clear(self):
        try:
            self.path().unlink()
        except FileNotFoundError:
            pass


def forget_approval_failure(options):
    """A host started with the owner at the device serves: the watch that
    failed for approval may run again."""
    ApprovalLatch(options).clear()
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
    latch = ApprovalLatch(options).read()
    if latch:
        summary["approval"] = (
            "the device may be waiting for an approval nobody gave "
            f"({latch.get('kind')}): no watch starts a host until `start` "
            "serves with the owner at the device")
    return summary


def wait_for_replacement(options, clock=None):
    """With a watch running, a command waits while the watch says it is
    starting a host, or has not seen the host's heartbeat advance lately (the
    watch may not have noticed a death yet), until it has, or the watch ends;
    it returns at once, and fails as it always did, when no watch runs. The
    wait is a hard bound, and makes no device read: it reads only the watch's
    own record, whose times are the Mac's."""
    clock = clock or CLOCK
    if watcher_pid(options) is None:
        return
    deadline = clock.monotonic() + WATCH_COMMAND_WAIT
    announced = False
    while True:
        record = read_watch_record(options)
        if (watcher_pid(options) is None
                or record.get("state") not in ACTIVE_STATES):
            return
        if (record.get("state") == WATCHING
                and clock.time() - record.get("alive_at", 0) <= WATCH_ALIVE_FRESH):
            return
        if clock.monotonic() >= deadline:
            raise NoHost(f"the host was still being replaced after "
                         f"{WATCH_COMMAND_WAIT} s")
        if not announced:
            log("the host is not serving and its watch is starting another; "
                f"waiting up to {WATCH_COMMAND_WAIT} s")
            announced = True
        clock.sleep(1)


def inflight_directory(options):
    return state_path(options) / "inflight"


class Inflight:
    """What a sender tells the watch while it waits for a command's result:
    how long it waits, so that a command the host has run for as long as that
    is hung and one it has not is not. A sender that ended leaves a file that
    its process's start time shows is stale."""

    def __init__(self, options, ident, limit):
        self.options = options
        self.path = inflight_directory(options) / f"{ident}.json"
        instance = process_instance(os.getpid())
        self.data = {"pid": os.getpid(), "limit": limit,
                     "start": instance[0] if instance else ""}

    def __enter__(self):
        state_dir(self.options)
        self.path.parent.mkdir(exist_ok=True)
        write_json(self.path, self.data)
        return self

    def __exit__(self, *exception):
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def inflight_limit(options):
    """The longest wait, in seconds, of the senders that still wait for a
    command's result; 0 when none does."""
    longest = 0
    try:
        paths = list(inflight_directory(options).iterdir())
    except OSError:
        return 0
    for path in paths:
        entry = read_json(path)
        pid, limit = entry.get("pid"), entry.get("limit")
        if (not isinstance(pid, int) or not entry.get("start")
                or not isinstance(limit, (int, float))):
            continue
        instance = process_instance(pid)
        if instance is not None and instance[0] == entry["start"]:
            longest = max(longest, limit)
    return longest


def send(options, command, timeout, clock=None):
    """Sends one command and waits at most `timeout` s for its result.

    A command already sent to a host that ends is not repeated for the
    replacement: it may have run, and the replacement clears its inbox, so
    the sender is told the host is gone (status 3), as for any dead host."""
    clock = clock or CLOCK
    wait_for_replacement(options, clock)
    ident = f"{time.time_ns():020d}-{uuid.uuid4().hex[:8]}"
    command = dict(command, bundle=command.get("bundle") or options.bundle)
    deadline = clock.monotonic() + timeout + 30

    def remaining():
        return max(1, min(60, deadline - clock.monotonic()))

    sent_to = host_state(options, remaining())
    host = sent_to.get("nonce") if sent_to else None
    with Inflight(options, ident, timeout + 30), \
            tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        body = scratch / f"{ident}.json"
        marker = scratch / f"{ident}.ready"
        body.write_text(json.dumps(command))
        marker.write_text("")
        push(options, [body, marker], f"{CHANNEL}/inbox")
        result = scratch / "result.json"
        next_check = clock.monotonic() + SEND_LIVENESS_EVERY
        while clock.monotonic() < deadline:
            if pull(options, f"{CHANNEL}/outbox/{ident}.json", result, remaining()):
                reply = json.loads(result.read_text())
                if "file" in reply:
                    attachment = scratch / reply["file"]
                    if not pull(options, f"{CHANNEL}/outbox/{reply['file']}", attachment, 120):
                        raise NoHost("the host wrote an attachment that cannot be copied")
                    reply["data"] = attachment.read_bytes()
                return reply
            if clock.monotonic() >= next_check:
                state = host_state(options, remaining())
                reason = liveness(state)
                if reason is not None:
                    raise NoHost(f"the host is not running: {reason}" + (
                        "; its watch replaces it, but this command is not "
                        "repeated because it may have run"
                        if watcher_pid(options) is not None else ""))
                if host and state.get("nonce") != host:
                    raise NoHost("the host this command was sent to has been "
                                 "replaced, and the replacement started with an "
                                 "empty inbox; the command is not repeated "
                                 "because it may have run")
                next_check = clock.monotonic() + SEND_LIVENESS_EVERY
            clock.sleep(0.5)
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


def clear_group(wait):
    """Ends what else is in the supervisor's process group (it leads one of its
    own), so that the supervisor ends last and nothing of the host outlives
    it."""
    group = os.getpgrp()
    if group != os.getpid():
        return

    def others():
        return [pid for pid, _ in (group_members(group) or []) if pid != os.getpid()]

    if not others():
        return
    try:
        os.killpg(group, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + wait
    while others() and time.monotonic() < deadline:
        time.sleep(0.2)
    for pid in others():
        try:
            os.kill(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


def command_supervise(options):
    """Runs xcodebuild for the host's lifetime; started detached by `start`.

    The result bundle goes where `start` says, under the state directory,
    never to the default under the internal disk's DerivedData. This is where
    xcodebuild starts, so the run file and the report store are checked here,
    whoever called.

    The supervisor stays responsible for the whole host until nothing of it is
    left: SIGTERM (what ends a host) ends xcodebuild, with SIGKILL for one
    that ignores it, and then whatever else xcodebuild left in the group; it
    exits last."""
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
    stopping = []
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, lambda *_: stopping.append(number))
    # xcodebuild stays in this process group, which `start`, `stop` and the
    # watch signal as a whole.
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, env=environment)
    deadline = time.monotonic() + options.seconds + 900
    while (process.poll() is None and not stopping
           and time.monotonic() < deadline):
        time.sleep(0.2)
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=options.term_wait)
        except subprocess.TimeoutExpired:
            process.kill()
    status = process.wait()
    clear_group(options.term_wait)
    print(f"ipad-automation: xcodebuild exited with status {status}", flush=True)
    return status


class LaunchRefused(Exception):
    """Nothing was started. A permanent refusal is one that trying again
    cannot change; `cancelled` says the caller asked to end first; `settle`
    says a host another launch started is still coming up."""

    def __init__(self, message, permanent=False, cancelled=False, settle=False):
        super().__init__(message)
        self.permanent = permanent
        self.cancelled = cancelled
        self.settle = settle


class Launch:
    """One host that `launch_host` started."""

    def __init__(self, process, log_path, nonce):
        self.process = process
        self.log_path = log_path
        self.nonce = nonce


class LaunchLock:
    """The launch transition of one device: from the check that no host is
    coming up to the host's record on disk. `start` and the watch each make it
    under this lock, so that two of them never launch side by side. It is an
    exclusive `flock` on `launch.lock` in the state directory, and waiting for
    it is bounded and ends when `cancelled()` says so."""

    def __init__(self, options, cancelled=None):
        self.path = state_dir(options) / "launch.lock"
        self.cancelled = cancelled
        self.fd = None

    def __enter__(self):
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
        deadline = time.monotonic() + LAUNCH_LOCK_WAIT
        waiting = False
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if not waiting:
                    log("another launch of this device is in progress; "
                        "waiting for it")
                    waiting = True
            if self.cancelled is not None and self.cancelled():
                os.close(fd)
                raise LaunchRefused("the launch was cancelled while it waited "
                                    "for another launch", cancelled=True)
            if time.monotonic() >= deadline:
                os.close(fd)
                raise LaunchRefused("another launch of this device did not "
                                    f"finish within {LAUNCH_LOCK_WAIT} s")
            time.sleep(0.1)
        self.fd = fd
        return self

    def __exit__(self, *exception):
        os.close(self.fd)


def host_unsettled(record, default_wait):
    """Whether the host the record names is still coming up: its supervisor
    runs, and the wait its starter recorded (plus the runner's own) has not
    run out."""
    if recorded_supervisor(record) is None:
        return False
    wait = record.get("wait") or default_wait
    return time.time() < record.get("started", 0) + wait + WATCH_RUNNER_WAIT


def launch_conflict(options):
    """Why `start` must not launch a host now, or None."""
    if watcher_pid(options) is not None:
        return ("a watch is running for this device: it replaces a host that "
                "ends, and a second host would ask for approval; `stop "
                "--force` ends the watch and the host")
    if liveness(host_state(options)) is None:
        return "a host is already serving this device; use it or stop it first"
    if host_unsettled(local_record(options), options.wait):
        return ("a host started a moment ago is still coming up; wait for it, "
                "or `stop --force`")
    return None


def start_guard(options):
    conflict = launch_conflict(options)
    return LaunchRefused(conflict) if conflict else None


def collection_options(options, label):
    """The options for one collection of the device's diagnostic reports. The
    collector refuses a destination that exists, so each collection of a
    watch, which collects again and again, gets a directory of its own
    beneath the one configured."""
    if not options.device_reports_dir:
        return options
    fresh = argparse.Namespace(**vars(options))
    fresh.device_reports_dir = pathlib.Path(options.device_reports_dir) / label
    return fresh


def launch_host(options, cancelled=None, guard=None, record=None):
    """Starts a supervisor, and with it xcodebuild and a new host, and records
    it in `host.json`. Raises LaunchRefused, before anything starts, when the
    host cannot be started.

    The transition is made under the device's launch lock: `guard()` there
    returns a LaunchRefused to raise (or None), `cancelled()` is checked
    again after the report preflight, which can take minutes, and `record`
    is told the host's nonce and log before the host starts. A record that
    exists before the supervisor does is written whole, and a crash between
    the two leaves it findable by its nonce."""
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
    # The nonce keeps two starts in one second apart: xcodebuild refuses a
    # result bundle that exists.
    name = f"host-{time.strftime('%Y%m%d-%H%M%S')}-{nonce}"
    log_path = directory / f"{name}.log"
    result_bundle = directory / f"{name}.xcresult"
    with LaunchLock(options, cancelled):
        refusal = guard() if guard is not None else None
        if refusal is not None:
            raise refusal
        # Before anything starts: a kill during the session must leave a report.
        if not check_report_slots(collection_options(options, f"{name}-launch"),
                                  REPORT_PROCESSES):
            raise LaunchRefused("the device's report store has too few free slots "
                                "or cannot be read")
        if cancelled is not None and cancelled():
            raise LaunchRefused("the launch was cancelled before the host started",
                                cancelled=True)
        if record is not None:
            record(nonce, log_path)
        reports = []
        supervised = collection_options(options, f"{name}-supervise")
        if supervised.device_reports_dir:
            reports = ["--device-reports-dir", str(supervised.device_reports_dir)]
        if options.leave_device_reports:
            reports.append("--leave-device-reports")
        host = {"supervisor": None, "log": str(log_path),
                "result_bundle": str(result_bundle), "nonce": nonce,
                "started": time.time(), "seconds": seconds, "wait": options.wait}
        write_json(directory / "host.json", host)
        with open(log_path, "w") as output:
            supervisor = subprocess.Popen(
                [sys.executable, str(SCRIPT), "--device", options.device,
                 "--runner", options.runner, "--state-dir", options.state_dir,
                 "_supervise", "--xctestrun", str(runs[-1]),
                 "--result-bundle", str(result_bundle),
                 "--seconds", str(seconds), "--nonce", nonce,
                 "--term-wait", str(CHILD_TERM_WAIT), *reports],
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                start_new_session=True)
        write_json(directory / "host.json", dict(host, supervisor=supervisor.pid))
    return Launch(supervisor, log_path, nonce)


def command_start(options):
    conflict = launch_conflict(options)
    if conflict:
        log(conflict)
        return EXIT_FAILED
    try:
        launch = launch_host(options, guard=lambda: start_guard(options))
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
    scan = LogScan(str(log_path))
    scan.read()
    if scan.approval_failure:
        log("the device did not enable UI automation: the approval it asked for "
            "was not given (the prompt can stay on the screen until someone "
            f"dismisses it); {scan.approval_failure}")
    elif supervisor.poll() is not None:
        log("xcodebuild ended before the host served; see the log")
    else:
        log(f"the host did not serve within {options.wait} s; see the log")
    if supervisor.poll() is None:
        # A supervisor that ended on its own cleared its group first.
        end_host(local_record(options), supervisor)
    return EXIT_NO_HOST


def signal_watcher(pid):
    os.kill(pid, signal.SIGTERM)


def watcher_is_a_watch(pid):
    """Whether `pid` is a `watch` of this script, the last check before a
    signal."""
    instance = process_instance(pid)
    if instance is None:
        return False
    words = instance[1].split()
    return "watch" in words and any(
        word.endswith(SCRIPT.name) for word in words)


def end_watch(options):
    """Ends the watch running for this device, before its host is stopped:
    left running it would start the host again. False, after saying why,
    when it cannot be ended, and then the host stays.

    Only the process that holds this device's lock for this state directory,
    as the lock file names it with its start time, is signalled: a watch for
    another device, or any other process that has the pid of one that ended,
    is not."""
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
    if recorded_supervisor(record) is not None:
        deadline = time.monotonic() + SUPERVISOR_EXIT_WAIT
        while time.monotonic() < deadline:
            if recorded_supervisor(record) is None:
                break
            time.sleep(1)
        if recorded_supervisor(record) is not None:
            log(f"terminating supervisor {record.get('supervisor')}")
    if end_host(record) == STUCK:
        log("part of the host would not end; see `ps` for its process group")
        return EXIT_FAILED
    return 0


class LogScan:
    """Reads a host's xcodebuild log as it grows and keeps what the watch
    needs of it. A line is taken once it is complete."""

    def __init__(self, path):
        self.path = path
        self.offset = 0
        self.runner_up = 0
        self.approval_failure = None
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
        if INIT_FAILURE_LINE in line:
            failure = line[line.index(INIT_FAILURE_LINE):][:160]
            if APPROVAL_TIMEOUT_LINE in line or AUTHENTICATION_CANCELLED.search(line):
                # The device asked for an approval and did not get it.
                self.approval_failure = failure
            else:
                self.init_failure = failure
        if line.startswith(EXIT_STATUS_LINE):
            self.exit_status = line[len(EXIT_STATUS_LINE):].strip()
        elif line.startswith("ipad-automation:"):
            self.note = line[:200]

    def describe(self):
        """Why the host's run ended, as far as its log says."""
        parts = []
        if self.exit_status:
            parts.append(f"xcodebuild exited with status {self.exit_status}")
        if self.approval_failure:
            parts.append(self.approval_failure)
        elif self.init_failure:
            parts.append(self.init_failure)
        elif self.note and not self.exit_status:
            parts.append(self.note)
        return "; ".join(parts) or "its log says nothing"


class HostObserver:
    """Tells, from what a host and its processes show, whether it still serves.

    The host writes its heartbeat every 2 s from a queue that no command
    blocks, so a busy host keeps beating, and only a beat that stops advancing
    ends a host. The watch measures that silence itself: the time between the
    read that first showed a beat and the last read that still showed it,
    counting only reads that worked, on its own monotonic clock. It never
    subtracts the device's clock from the Mac's, a read that fails or is late
    is no evidence (the host may beat on, unseen), and a gap between the wall
    clock and the monotonic one (the Mac slept) starts the measuring again.
    The processes only hurry the verdict: a runner that still beats serves,
    whatever happened to the process that started it."""

    def __init__(self, options, nonce, state, process=None, scan=None,
                 clock=CLOCK, on_progress=None):
        self.options = options
        self.nonce = nonce
        self.state = state
        self.process = process
        self.scan = scan or LogScan(None)
        self.runners = self.scan.runner_up
        self.clock = clock
        self.on_progress = on_progress
        now = clock.monotonic()
        self.beat = state.get("time", 0)
        self.changed = self.observed = self.last_pull = now
        self.last_mono, self.last_wall = now, clock.time()
        self.blind_since = None
        self.candidate = None
        self.relaunched = False

    def supervisor_gone(self):
        if self.process is not None:
            return self.process.poll() is not None
        record = local_record(self.options)
        return bool(self.nonce) and record.get("nonce") == self.nonce and (
            recorded_supervisor(record) is None)

    def progressed(self):
        if self.on_progress is not None:
            self.on_progress()

    def pull(self):
        """One read of the heartbeat. A read that fails observes nothing."""
        self.last_pull = self.clock.monotonic()
        state = host_state(self.options)
        now = self.clock.monotonic()
        if state is None:
            if self.blind_since is None:
                self.blind_since = now
            return
        self.blind_since = None
        if state.get("nonce") != self.nonce:
            self.consider(state, now)
            return
        self.state = state
        self.observed = now
        if state.get("time", 0) != self.beat:
            self.beat = state.get("time", 0)
            self.changed = now
            self.progressed()

    def consider(self, state, now):
        """The heartbeat of another host: it is followed once its beat
        advances between two reads. What an earlier host left behind does not."""
        nonce = state.get("nonce")
        if not nonce or state.get("finished"):
            self.candidate = None
            return
        seen = self.candidate
        self.candidate = (nonce, state.get("time", 0))
        if seen is None or seen[0] != nonce or seen[1] == state.get("time", 0):
            return
        record = local_record(self.options)
        self.nonce, self.process = nonce, None
        self.scan = LogScan(record.get("log") if record.get("nonce") == nonce else None)
        self.scan.read()
        self.runners = self.scan.runner_up
        self.state = state
        self.beat = state.get("time", 0)
        self.changed = self.observed = now
        self.candidate = None
        self.progressed()

    def check(self):
        """None while the host serves, else why it does not."""
        clock = self.clock
        now = clock.monotonic()
        wall = clock.time()
        slept = abs((wall - self.last_wall) - (now - self.last_mono)) > WATCH_CLOCK_GAP
        self.last_mono, self.last_wall = now, wall
        if slept:
            # The silence measured so far is not the host's: measure again,
            # from a fresh read.
            self.changed = self.observed = now
        gone = self.supervisor_gone()
        self.scan.read()
        relaunched = self.scan.runner_up > self.runners
        self.relaunched = relaunched
        quick = gone or relaunched
        if slept or quick or now - self.last_pull >= WATCH_PULL_EVERY:
            self.pull()
        state = self.state
        if state.get("finished"):
            if state.get("time", 0) >= state.get("deadline", 0) - WATCH_GONE_AGE:
                return "the host reached its lifetime"
            return "the host reported that it finished"
        silent = self.observed - self.changed
        if silent >= (WATCH_GONE_AGE if quick else HEARTBEAT_STALE):
            if gone:
                return (f"the supervisor ended ({self.scan.describe()}) and "
                        f"the heartbeat has not advanced for {silent:.0f} s")
            if relaunched:
                return ("xcodebuild started the runner again, so the host's "
                        f"runner ended; the heartbeat has not advanced for {silent:.0f} s")
            return f"the heartbeat has not advanced for {silent:.0f} s"
        if state.get("command"):
            # The device's own clock, both times: how long it has run.
            running = state.get("time", 0) - state.get("commandStarted", state.get("time", 0))
            if running > WATCH_BUSY_LIMIT and running > inflight_limit(self.options):
                return f"one command has run for {running:.0f} s: the host is hung"
        return None

    def blind_for(self):
        """How long the heartbeat could not be read, in seconds."""
        return 0 if self.blind_since is None else self.clock.monotonic() - self.blind_since


class Watch:
    """Keeps one device's host serving; see `watch` in the module's help.

    `stop` is set to end it; the watch then leaves a serving host as it is."""

    def __init__(self, options, stop=None, clock=CLOCK):
        self.options = options
        self.stop = stop or threading.Event()
        self.clock = clock
        self.record = WatchRecord(options)
        self.latch = ApprovalLatch(options)
        self.observer = None
        self.pending = None
        self.blind_said = False
        self.restarts = 0
        self.restart_times = [t for t in self.record.data.get("restart_times", [])
                              if isinstance(t, (int, float))]

    def say(self, message):
        log(f"watch {time.strftime('%H:%M:%S')}: {message}")

    def progress(self):
        """The observer saw the heartbeat advance: commands may be sent."""
        self.record.update(alive_at=self.clock.time())

    def run(self):
        self.record.update(state=STARTING, detail="", pid=os.getpid(),
                           started=self.clock.time(), restarts=0,
                           restart_times=self.restart_times)
        # What the last launch left (a `start`'s host still coming up, or the
        # host a watch that ended was starting) comes before anything else: it
        # may have been the one the device asked to approve, and the beat of
        # the host before it can still look alive.
        self.settle_last_launch()
        state = None
        for _ in range(WATCH_ADOPT_PULLS):
            if self.stop.is_set():
                break
            state, observed = self.survey()
            if (state is not None or observed
                    or recorded_supervisor(local_record(self.options)) is None):
                break
            self.say("the host's heartbeat cannot be read; looking again before "
                     "taking its place")
        if self.stop.is_set():
            return self.stopped()
        if state is not None:
            self.adopt(state)
        elif self.latch.read():
            return self.refuse_after_approval_failure()
        else:
            code = self.replace("no host was serving when the watch started",
                                ended=False)
            if code is not None:
                return code
        while not self.stop.is_set():
            reason = self.observer.check()
            if reason is None:
                self.report_blind()
                self.clock.wait(self.stop, WATCH_POLL)
                continue
            code = self.replace(reason)
            if code is not None:
                return code
        return self.stopped()

    def report_blind(self):
        blind = self.observer.blind_for()
        if blind >= 60 and not self.blind_said:
            self.say(f"the host's heartbeat cannot be read for {blind:.0f} s; "
                     "a host that cannot be seen is left alone")
            self.blind_said = True
        elif blind == 0:
            self.blind_said = False

    def survey(self):
        """(state, observed): the heartbeat of a host that serves, which a
        beat that advances between two reads shows (the last beat of a host
        that just ended stands still), and whether any read worked."""
        previous = None
        observed = False
        for number in range(WATCH_ADOPT_PULLS):
            if number and self.clock.wait(self.stop, WATCH_ADOPT_GAP):
                break
            state = host_state(self.options)
            if state is None:
                continue
            observed = True
            if state.get("finished"):
                break
            if (previous is not None and state.get("nonce") == previous.get("nonce")
                    and state.get("time", 0) > previous.get("time", 0)):
                return state, True
            previous = state
        return None, observed

    def settle_last_launch(self):
        """Before the watch adopts or launches anything: what the last launch
        left. A host still coming up is its starter's to finish, the owner
        may be approving it now, and it is waited for as long as the wait its
        starter recorded; its own log says whether the device asked for an
        approval nobody gave, which is then recorded for good."""
        options = self.options
        record = local_record(options)
        nonce = record.get("nonce")
        if not nonce:
            return
        coming = record.get("started", 0) + (record.get("wait") or options.wait) \
            + WATCH_RUNNER_WAIT
        scan = LogScan(record.get("log"))
        announced = False
        while not self.stop.is_set():
            scan.read()
            state = host_state(options)
            scan.read()
            now = self.clock.time()
            served = bool(state and state.get("nonce") == nonce)
            if served and not state.get("finished") and liveness(state) is None:
                return
            if scan.approval_failure and not served:
                self.latch.set("observed", nonce, record.get("log"), scan.describe())
                end_host(record)
                return
            if recorded_supervisor(record) is None:
                if (not served and not scan.exit_status and scan.runner_up
                        and now < coming + WATCH_APPROVAL_WINDOW):
                    # It ended without saying why, while it could have been
                    # waiting for an approval.
                    self.latch.set("possible", nonce, record.get("log"),
                                   "a host ended before it served and before "
                                   "its log could say whether the device asked "
                                   "for approval")
                return
            if now >= coming:
                return
            if not announced:
                self.say(f"a host that started {now - record.get('started', now):.0f} s "
                         "ago does not serve yet; waiting for it")
                announced = True
            self.clock.wait(self.stop, WATCH_POLL)

    def adopt(self, state):
        """Takes over watching a host that already serves, touching nothing."""
        nonce = state.get("nonce")
        record = local_record(self.options)
        scan = LogScan(record.get("log") if record.get("nonce") == nonce else None)
        scan.read()
        self.observer = HostObserver(self.options, nonce, state, None, scan,
                                     self.clock, self.progress)
        now = self.clock.time()
        self.say(f"adopting the host that serves (up {now - state.get('started', now):.0f} s, "
                 f"{state.get('deadline', now) - now:.0f} s left); it is not touched")
        counted = {}
        pending = self.record.data.get("pending")
        if (isinstance(pending, dict) and pending.get("nonce") == nonce
                and isinstance(pending.get("time"), (int, float))):
            # The host a watch that ended began to start: its replacement
            # counts, once, now that it is known to serve.
            self.restart_times = [t for t in self.restart_times
                                  if now - t < WATCH_STORM_WINDOW] + [pending["time"]]
            self.restarts += 1
            counted = {"restart_times": self.restart_times, "restarts": self.restarts}
        # A host serves, so whatever the device was asked is answered.
        self.latch.clear()
        self.pending = None
        self.record.update(state=WATCHING, detail="adopted a serving host",
                           host=nonce, pending=None, alive_at=now, **counted)

    def await_relaunched_runner(self):
        """xcodebuild started the runner again after the host's runner ended.
        That runner runs no test (the one that was running is not run again),
        but the device can ask for an approval for it, and its log says within
        a minute whether it did. The host started next would put the same
        prompt up again, and ending this runner now would end it before it
        could say: wait, for as long as XCTest gives the prompt. True when its
        log shows the approval was not given."""
        observer = self.observer
        deadline = self.clock.monotonic() + WATCH_APPROVAL_WINDOW
        self.say("xcodebuild started the runner again; waiting for it to say "
                 "whether the device asks for an approval")
        while not self.stop.is_set():
            observer.scan.read()
            if observer.scan.approval_failure:
                return True
            if observer.supervisor_gone():
                observer.scan.read()
                return bool(observer.scan.approval_failure)
            if self.clock.monotonic() >= deadline:
                return False
            self.clock.wait(self.stop, WATCH_POLL)
        return False

    def guard(self):
        """Under the launch lock: a host another launch is bringing up is not
        raced."""
        if host_unsettled(local_record(self.options), self.options.wait):
            return LaunchRefused(
                "a host that another launch started is still coming up",
                settle=True)
        return None

    def note_pending(self, nonce, log_path, replacement):
        """Recorded before the host starts: a watch that ends in between
        leaves the replacement to be counted when its host is adopted."""
        self.pending = {"nonce": nonce, "log": str(log_path),
                        "time": self.clock.time(), "replacement": replacement}
        self.record.update(pending=self.pending)

    def retire(self):
        """Ends what is left of the host that stopped serving, so that no
        second session or prompt piles up beside its successor. Why that
        cannot be done, or None."""
        process = self.observer.process if self.observer is not None else None
        if end_host(local_record(self.options), process) == STUCK:
            return ("part of the host that stopped serving would not end, even "
                    "after SIGKILL: no second host starts beside it")
        return None

    def replace(self, reason, ended=True):
        """Starts the host that replaces one that stopped serving (`ended`
        is False for the first host, when none served). Returns the status
        the watch ends with, or None once the new host serves."""
        options = self.options
        now = self.clock.time()
        if ended:
            self.say(f"the host stopped serving: {reason}")
            self.record.update(state=RESTARTING, detail=reason,
                               last_end={"reason": reason, "time": now})
        else:
            self.say(reason)
            self.record.update(state=STARTING, detail=reason)
        if ended and self.observer is not None and self.observer.relaunched:
            asked = self.await_relaunched_runner()
            if self.stop.is_set():
                return self.stopped()
            if asked:
                self.retire()
                self.latch.set("observed", self.observer.nonce, self.observer.scan.path,
                               self.observer.scan.describe())
                return self.give_up(
                    NEEDS_APPROVAL,
                    "the host's runner ended and the one xcodebuild started in "
                    "its place could not be approved at the device "
                    f"({self.observer.scan.approval_failure}); run `start` with "
                    "the owner at the device, then `watch` again",
                    EXIT_NEEDS_APPROVAL)
        problem = self.retire()
        if problem:
            return self.give_up(CANNOT_START, problem, EXIT_WATCH_GAVE_UP)
        recent = [t for t in self.restart_times if now - t < WATCH_STORM_WINDOW]
        if len(recent) >= options.max_restarts:
            return self.give_up(
                RESTART_STORM,
                f"{len(recent)} replacements in the last {WATCH_STORM_WINDOW // 60} "
                f"minutes, and the bound is {options.max_restarts}; the host ended "
                f"again: {reason}", EXIT_WATCH_GAVE_UP)
        detail = ""
        for number in range(1, WATCH_ATTEMPTS + 1):
            outcome = self.attempt(ended)
            kind = outcome[0]
            if kind == "served":
                self.observer = outcome[1]
                self.restarts += 1
                launched = (self.pending or {}).get("time", self.clock.time())
                self.restart_times = recent + [launched]
                self.pending = None
                self.say(f"the replacement serves (restart {self.restarts})")
                self.record.update(
                    state=WATCHING, detail="", host=self.observer.nonce,
                    restarts=self.restarts, restart_times=self.restart_times,
                    pending=None, alive_at=self.clock.time())
                return None
            self.pending = None
            self.record.update(pending=None)
            if kind == "stopped":
                return self.stopped()
            if kind == "approval":
                return self.give_up(
                    NEEDS_APPROVAL,
                    "the device asked for approval and XCTest gave up after its "
                    "60 s: nobody answered; run `start` with the owner at the "
                    "device, then `watch` again", EXIT_NEEDS_APPROVAL)
            if kind == "stuck":
                return self.give_up(CANNOT_START, outcome[1], EXIT_WATCH_GAVE_UP)
            if kind == "settle":
                self.settle_last_launch()
                if self.stop.is_set():
                    return self.stopped()
                state, _ = self.survey()
                if state is not None:
                    self.adopt(state)
                    return None
                if self.latch.read():
                    return self.refuse_after_approval_failure()
                detail = "a host that another launch started did not serve"
            else:
                _, detail, permanent = outcome
                if permanent:
                    return self.give_up(CANNOT_START, detail, EXIT_WATCH_GAVE_UP)
            self.say(f"attempt {number} of {WATCH_ATTEMPTS} failed: {detail}")
            self.record.update(detail=f"attempt {number} failed: {detail}")
            if number < WATCH_ATTEMPTS and self.clock.wait(
                    self.stop, WATCH_BACKOFF[number - 1]):
                return self.stopped()
        return self.give_up(
            START_FAILED, f"{WATCH_ATTEMPTS} attempts failed; the last: {detail}",
            EXIT_WATCH_GAVE_UP)

    def attempt(self, replacement):
        """One start of a host: ("served", observer), ("approval",),
        ("stopped",), ("settle",), ("stuck", why) or ("failed", why,
        permanent).

        The approval prompt is told from every other failure by xcodebuild's
        own line, "Timed out while enabling automation mode": XCTest prints it
        when the prompt stayed unanswered for its 60 s, so it means the device
        asked. The log is read again after every wait that can take long and
        after the host's processes are ended, so that a line that arrives
        meanwhile is not lost; a host ended before it could print one, within
        the 60 s, may have been waiting for an approval, and is not started
        again (the approval latch). A runner that is up and has neither served
        nor printed the line within WATCH_RUNNER_WAIT is not waiting for a
        prompt; that, a runner that never comes up and an xcodebuild that ends
        first are ordinary failures, which are retried."""
        options = self.options
        try:
            launch = launch_host(
                options, cancelled=self.stop.is_set, guard=self.guard,
                record=lambda nonce, path: self.note_pending(nonce, path, replacement))
        except LaunchRefused as refusal:
            if refusal.cancelled:
                return ("stopped",)
            if refusal.settle:
                return ("settle",)
            return ("failed", str(refusal), refusal.permanent)
        self.say(f"started host {launch.nonce[:8]} under supervisor "
                 f"{launch.process.pid}; log {launch.log_path}")
        scan = LogScan(launch.log_path)
        deadline = self.clock.monotonic() + options.wait
        runner_up_at = None
        while True:
            if self.stop.is_set():
                return self.finish(launch, scan, runner_up_at, "stopped")
            scan.read()
            state = host_state(options)
            scan.read()
            if scan.approval_failure:
                return self.finish(launch, scan, runner_up_at, "approval")
            if (state and state.get("nonce") == launch.nonce
                    and liveness(state) is None):
                return ("served", HostObserver(
                    options, launch.nonce, state, launch.process, scan,
                    self.clock, self.progress))
            if launch.process.poll() is not None:
                return self.finish(launch, scan, runner_up_at, "exited")
            now = self.clock.monotonic()
            if scan.runner_up and runner_up_at is None:
                runner_up_at = now
                deadline = now + WATCH_RUNNER_WAIT
            if now > deadline:
                return self.finish(launch, scan, runner_up_at, "overdue")
            self.clock.wait(self.stop, WATCH_POLL)

    def finish(self, launch, scan, runner_up_at, why):
        """An attempt ends without a serving host: ends whatever is left of
        it, then reads its log once more and says what kind of end it was."""
        stuck = end_host(local_record(self.options), launch.process) == STUCK
        scan.read()
        if scan.approval_failure:
            self.latch.set("observed", launch.nonce, launch.log_path, scan.describe())
            return ("stopped",) if why == "stopped" else ("approval",)
        if why == "stopped":
            if scan.runner_up and (runner_up_at is None or self.clock.monotonic()
                                   - runner_up_at < WATCH_APPROVAL_WINDOW):
                self.latch.set("possible", launch.nonce, launch.log_path,
                               "the host was ended before its log could say "
                               "whether the device asked for approval")
            return ("stopped",)
        if stuck:
            return ("stuck", "part of the host that was starting would not end, "
                    "even after SIGKILL: no second host starts beside it")
        if why == "exited":
            return ("failed", f"xcodebuild ended before the host served "
                    f"({scan.describe()}); log {launch.log_path}", False)
        explanation = (f"the runner was up but did not serve within "
                       f"{WATCH_RUNNER_WAIT} s" if scan.runner_up else
                       f"the runner did not come up within {self.options.wait} s")
        return ("failed", f"{explanation}; log {launch.log_path}", False)

    def give_up(self, state, detail, code):
        self.say(f"{STATE_TEXT[state]}: {detail}")
        self.record.update(state=state, detail=detail)
        return code

    def refuse_after_approval_failure(self):
        """A watch is not run again after an approval failure by whatever
        starts it again (a job manager, a loop): each try would put another
        prompt on the device. The latch on disk, not this watch's memory, is
        what refuses, and only a host that serves clears it."""
        latch = self.latch.read() or {}
        detail = latch.get("detail", "")
        self.say("a host started for this device may have needed an approval "
                 f"nobody gave ({latch.get('kind')}), and no host serves: run "
                 "`start` with the owner at the device, then `watch` again; "
                 f"starting nothing. {detail}")
        self.record.update(state=NEEDS_APPROVAL, detail=detail,
                           since=latch.get("since", self.clock.time()))
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
            # A relative PATH entry names a directory of this shell's
            # working directory, which the job does not have.
            directories.append(str(pathlib.Path(found).absolute().parent))
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
        # Absolute without resolving: the report collector refuses a path
        # with a symlink in it, and a resolved one would hide that.
        arguments += ["--device-reports-dir",
                      str(pathlib.Path(options.device_reports_dir).absolute())]
    if options.leave_device_reports:
        arguments.append("--leave-device-reports")
    environment = {"PATH": launchd_path()}
    if os.environ.get("DEVELOPER_DIR"):
        environment["DEVELOPER_DIR"] = str(
            pathlib.Path(os.environ["DEVELOPER_DIR"]).absolute())
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
        job = launchd_job(options, options.print_launchd_job)
        # Printing creates nothing, and launchd will not create the directory
        # its log goes to.
        log(f"the job logs to {job['StandardOutPath']}: before loading it, run "
            f"`mkdir -p {pathlib.Path(job['StandardOutPath']).parent}`")
        sys.stdout.write(plistlib.dumps(job).decode())
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


def collections_help(parser):
    """`start` and the watch collect the device's reports once or more per
    host, each into a new directory beneath the one named."""
    for action in parser._actions:
        if "--device-reports-dir" in action.option_strings:
            action.help = ("where the reports moved off the device go, outside "
                           "every checkout and with no symlink in the path: "
                           "each collection makes a new directory of its own "
                           "in it (default: a per-user folder)")


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
    collections_help(start)
    supervise = commands.add_parser("_supervise")
    supervise.add_argument("--xctestrun", required=True)
    supervise.add_argument("--result-bundle", required=True)
    supervise.add_argument("--seconds", type=int, required=True)
    supervise.add_argument("--nonce", required=True)
    supervise.add_argument("--term-wait", type=float, default=CHILD_TERM_WAIT,
                           help=argparse.SUPPRESS)
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
                       help="print the launchd job that runs this watch, and "
                            "exit; it creates nothing, and the job's log "
                            "directory, STATE_DIR/UDID, must exist when it "
                            "loads: `mkdir -p` it first")
    ios_device_reports.add_arguments(watch)
    collections_help(watch)
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
