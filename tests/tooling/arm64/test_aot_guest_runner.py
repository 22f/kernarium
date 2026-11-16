#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AOT guest runner against scripted guests.

tests/integration/aot/run_aot_guest.py compares an Android guest with the
ahead-of-time component off and on. Here a fake launcher and a fake adb stand
in for the guest, and each row scripts what the guest printed and which exit
statuses it returned. The runner must pass a complete, successful, identical
pair of arms and fail every incomplete or failed one.
"""

__import__("sys").dont_write_bytecode = True

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

RUNNER_DIRECTORY = (pathlib.Path(__file__).resolve().parents[2] /
                    "integration" / "aot")
sys.path.insert(0, str(RUNNER_DIRECTORY))
sys.path.insert(0, str(RUNNER_DIRECTORY.parent / "support"))
import run_aot_guest as runner  # noqa: E402

FAKE_LAUNCHER = """\
import os, pathlib, signal, sys, time
state = pathlib.Path(os.environ["FAKE_GUEST_STATE"])
arm = "on" if os.environ.get("DARWIN_UML_AOT") == "1" else "off"
(state / "arm").write_text(arm)
scenario = json.loads((state / "scenario.json").read_text())
report = os.environ.get("DARWIN_UML_AOT_REPORT")
if report and scenario.get("report", {}).get(arm) is not None:
    pathlib.Path(report).write_text(scenario["report"][arm])
# The emulator: a process of that name below the launcher.
import subprocess
link = state / "uml-darwin-android17"
if not link.exists():
    link.symlink_to("/bin/sleep")
child = subprocess.Popen([str(link), "120"])
def stop(*_):
    child.kill()
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
deadline = time.monotonic() + 120
while time.monotonic() < deadline:
    time.sleep(0.05)
child.kill()
"""

FAKE_VMMAP = """\
import json, os, pathlib, sys
state = pathlib.Path(os.environ["FAKE_GUEST_STATE"])
scenario = json.loads((state / "scenario.json").read_text())
arm = (state / "arm").read_text() if (state / "arm").exists() else "off"
sys.stdout.write(scenario["vmmap"][arm])
sys.exit(scenario.get("vmmap_status", 0))
"""

def vmmap_text(resident: str, framework: str = "UmlArm64Aot") -> str:
    return ("REGION TYPE                    START - END         [ VSIZE  RSDNT"
            "  DIRTY   SWAP] PRT/MAX SHRMOD PURGE    REGION DETAIL\n"
            "__TEXT                      114450000-123808000    [243.7M"
            f"  {resident}     0K     0K] r-x/rwx SM=COW          "
            f"/x/bin/Frameworks/{framework}.framework/{framework}\n"
            "__LINKEDIT                  123808000-123884000    [  496K"
            "    16K     0K     0K] r--/rwx SM=COW          "
            f"/x/bin/Frameworks/{framework}.framework/{framework}\n")


FAKE_ADB = """\
import json, os, pathlib, sys
state = pathlib.Path(os.environ["FAKE_GUEST_STATE"])
scenario = json.loads((state / "scenario.json").read_text())
arm = (state / "arm").read_text() if (state / "arm").exists() else "off"
arguments = sys.argv[1:]
if arguments[:1] == ["-s"]:
    arguments = arguments[2:]
verb = arguments[0] if arguments else ""
if verb == "get-state":
    print("device")
elif verb == "shell" and arguments[1].startswith("getprop"):
    print("1")
elif verb == "shell" and "LD_PRELOAD" in arguments[1]:
    counter = state / ("probe-" + arm)
    number = int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(number + 1))
    texts = scenario["probe"][arm]
    sys.stdout.write(texts[min(number, len(texts) - 1)])
    sys.exit(scenario.get("probe_status", 0))
elif verb == "shell":
    sys.stdout.write(scenario["workloads"][arm])
    sys.exit(scenario.get("workloads_status", 0))
elif verb == "push":
    sys.exit(scenario.get("push_status", 0))
sys.exit(0)
"""

WRAPPER = """\
import sys
sys.path.insert(0, {runner_directory!r})
sys.path.insert(0, {support_directory!r})
import run_aot_guest as runner
runner.LAUNCHER = {launcher!r}
runner.SETTLE_SECONDS = 0
runner.REPORT_SECONDS = 0
sys.exit(runner.main())
"""


def digest(name: str) -> str:
    return hashlib.md5(name.encode()).hexdigest()


def good_workloads() -> list[str]:
    return [f"{name} {number} 0.25 {digest(name)} 0"
            for name in runner.WORKLOAD_NAMES
            for number in runner.REPETITIONS]


def workload_text(rows: list[str], finished: bool = True) -> str:
    return "\n".join(rows + ([runner.WORKLOADS_FINISHED] if finished else [])) + "\n"


def good_probe() -> str:
    lines = []
    for case in runner.PROBE_CASES:
        lines.append(f"case {case}")
        lines.append("  fault signo=11 code=2 addr=R+0x1000 pc=libz.so+0x40 "
                     "sp=S+0x10 pstate=0x0")
        lines.append("  regs x00=0x1 x01=0x2 x02=R+0x20")
    lines += ["done faults=%d" % len(runner.PROBE_CASES), "rc=0"]
    return "\n".join(lines) + "\n"


GOOD_AUDIT = ("count_audit runs 5 skipped 1 records 40 mismatches 0 "
              "last_mismatches 0 anomalies 0 nanos_total 9000000 nanos_max 3200000\n")
GOOD_STACK = ("stack_free 59200 stack_size 65536 stack_kind 0 "
              "stack_task_bytes 65536 stack_engine_free 62000 stack_growth 2800\n")


def good_scenario() -> dict:
    text = workload_text(good_workloads())
    return {"workloads": {"off": text, "on": text},
            "probe": {"off": [good_probe()], "on": [good_probe()]},
            "report": {"on": "binds 3 enters 9 steps 2 poisoned 0 refills 0\n" +
                       GOOD_AUDIT + GOOD_STACK,
                       "off": None},
            "vmmap": {"off": vmmap_text("16K"), "on": vmmap_text("19.3M")}}


class RunnerTest(unittest.TestCase):
    def run_runner(self, scenario: dict, *extra: str):
        with tempfile.TemporaryDirectory(prefix="aot-runner-test-") as name:
            root = pathlib.Path(name)
            state = root / "state"
            state.mkdir()
            (state / "scenario.json").write_text(json.dumps(scenario))
            launcher = root / "launcher.py"
            launcher.write_text("import json\n" + FAKE_LAUNCHER)
            adb = root / "adb"
            adb.write_text(f"#!{sys.executable}\n" + FAKE_ADB)
            adb.chmod(0o755)
            wrapper = root / "wrapper.py"
            wrapper.write_text(WRAPPER.format(
                runner_directory=str(RUNNER_DIRECTORY),
                support_directory=str(RUNNER_DIRECTORY.parent / "support"),
                launcher=str(launcher)))
            vmmap = root / "vmmap"
            vmmap.write_text(f"#!{sys.executable}\n" + FAKE_VMMAP)
            vmmap.chmod(0o755)
            probe = root / "libaotprobe.so"
            probe.write_bytes(b"probe")
            environment = {**os.environ, "FAKE_GUEST_STATE": str(state),
                           "PYTHONDONTWRITEBYTECODE": "1"}
            return subprocess.run(
                [sys.executable, "-B", str(wrapper), "--adb", str(adb),
                 "--probe", str(probe), "--vmmap", str(vmmap),
                 "--boot-timeout", "30", *extra],
                stdin=subprocess.DEVNULL, capture_output=True, text=True,
                timeout=90, env=environment, check=False)

    def passes(self, scenario: dict, *extra: str):
        result = self.run_runner(scenario, *extra)
        self.assertEqual(result.returncode, 0,
                         result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)
        return result

    def fails(self, scenario: dict, expected: str, *extra: str):
        result = self.run_runner(scenario, *extra)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("PASS", result.stdout)
        self.assertIn(expected, result.stderr)

    def both_arms(self, scenario: dict, rows: list[str], arm: str = "on",
                  finished: bool = True) -> dict:
        scenario["workloads"][arm] = workload_text(rows, finished)
        return scenario

    # The scripted guest the other rows start from.
    def test_complete_identical_arms_pass(self):
        result = self.passes(good_scenario())
        self.assertIn("registers compared, identical", result.stdout)

    def test_script_has_seven_workloads_and_three_repetitions(self):
        self.assertEqual(len(runner.WORKLOAD_NAMES), 7)
        self.assertEqual(runner.REPETITIONS, (1, 2, 3))

    def test_empty_output_digest_fails(self):
        rows = good_workloads()
        rows[4] = f"{runner.WORKLOAD_NAMES[1]} 2 0.25 {runner.EMPTY_DIGEST} 0"
        self.fails(self.both_arms(good_scenario(), rows), "printed nothing")
        self.fails(self.both_arms(good_scenario(), rows, "off"),
                   "printed nothing")

    def test_workload_exit_status_fails(self):
        rows = good_workloads()
        rows[9] = rows[9][:-1] + "1"
        self.fails(self.both_arms(good_scenario(), rows), "status 1")
        rows[9] = rows[9][:-1] + "missing"
        self.fails(self.both_arms(good_scenario(), rows, "off"),
                   "status missing")

    def test_missing_repetition_fails(self):
        rows = good_workloads()
        del rows[7]
        self.fails(self.both_arms(good_scenario(), rows),
                   "workload rows missing")

    def test_missing_workload_fails(self):
        rows = [row for row in good_workloads()
                if not row.startswith(runner.WORKLOAD_NAMES[-1] + " ")]
        self.fails(self.both_arms(good_scenario(), rows, "off"),
                   "workload rows missing")
        self.fails(self.both_arms(good_scenario(), []), "workload rows missing")

    def test_unfinished_workloads_fail(self):
        self.fails(self.both_arms(good_scenario(), good_workloads(),
                                  finished=False), "did not run to their end")

    def test_workload_command_status_fails(self):
        scenario = good_scenario()
        scenario["workloads_status"] = 1
        self.fails(scenario, "the workload command exited 1")

    def test_output_that_differs_between_arms_fails(self):
        rows = good_workloads()
        rows[0] = f"{runner.WORKLOAD_NAMES[0]} 1 0.25 {digest('other')} 0"
        rows[1] = rows[1].replace(digest(runner.WORKLOAD_NAMES[0]),
                                  digest("other"))
        rows[2] = rows[2].replace(digest(runner.WORKLOAD_NAMES[0]),
                                  digest("other"))
        scenario = self.both_arms(good_scenario(), rows)
        self.fails(scenario, "output differs with the component on")

    def test_failed_push_fails(self):
        scenario = good_scenario()
        scenario["push_status"] = 1
        self.fails(scenario, "pushing the probe exited 1")

    def test_failed_probe_command_fails(self):
        scenario = good_scenario()
        scenario["probe_status"] = 1
        self.fails(scenario, "the probe command exited 1")

    # A probe that fails the same way in both arms is no difference between
    # them, and still no result.
    def bad_probe(self, text: str) -> dict:
        scenario = good_scenario()
        scenario["probe"] = {"off": [text], "on": [text]}
        return scenario

    def test_probe_ending_in_failure_fails(self):
        self.fails(self.bad_probe(good_probe().replace("rc=0", "rc=1")),
                   "not rc=0")

    def test_probe_missing_a_scenario_fails(self):
        self.fails(self.bad_probe(good_probe().replace(
            "case patch-change\n", "case other\n")),
            "did not take: patch-change")

    def test_probe_that_skipped_patching_fails(self):
        self.fails(self.bad_probe(good_probe().replace(
            "case patch-before\n",
            "case patch-before\n  patch cases skipped\n")),
            "patch cases skipped")

    def test_probe_without_faults_fails(self):
        text = "\n".join(line for line in good_probe().splitlines()
                         if not line.startswith("  regs")) + "\n"
        self.fails(self.bad_probe(text), "no fault registers")
        self.fails(self.bad_probe(good_probe().replace(
            "done faults=%d" % len(runner.PROBE_CASES), "done faults=0")),
            "closing record")

    def test_probe_that_differs_between_arms_fails(self):
        scenario = good_scenario()
        scenario["probe"]["on"] = [good_probe().replace(
            "x01=0x2", "x01=0x3")]
        self.fails(scenario, "without the component")

    def test_probe_that_does_not_run_to_its_end_fails(self):
        self.fails(self.bad_probe(good_probe().replace(
            "done faults=%d\n" % len(runner.PROBE_CASES), "")),
            "closing record")

    def test_product_arm_without_residency_evidence_fails(self):
        scenario = good_scenario()
        scenario["vmmap"]["on"] = vmmap_text("16K")
        self.fails(scenario, "no residency evidence")
        scenario = good_scenario()
        scenario["vmmap"]["on"] = vmmap_text("512K")
        self.fails(scenario, "no residency evidence")

    def test_framework_resident_with_the_component_off_fails(self):
        scenario = good_scenario()
        scenario["vmmap"]["off"] = vmmap_text("19.3M")
        self.fails(scenario, "resident with the component off")

    def test_product_mode_without_evidence_fails(self):
        scenario = good_scenario()
        scenario["vmmap_status"] = 1
        self.fails(scenario, "vmmap exited 1")
        scenario = good_scenario()
        scenario["vmmap"] = {"off": "nothing\n", "on": "nothing\n"}
        self.fails(scenario, "no text of the AOT framework")
        scenario = good_scenario()
        scenario["vmmap"] = {"off": vmmap_text("16K", "Other"),
                             "on": vmmap_text("19.3M", "Other")}
        self.fails(scenario, "no text of the AOT framework")

    def test_diagnostics_mode_does_not_need_the_framework_regions(self):
        scenario = good_scenario()
        scenario["vmmap"] = {"off": "nothing\n", "on": "nothing\n"}
        self.passes(scenario, "--diagnostics")

    def test_diagnostics_report_is_judged(self):
        self.passes(good_scenario(), "--diagnostics")
        scenario = good_scenario()
        scenario["report"]["on"] = "binds 3 enters 0 steps 0 poisoned 0\n" + GOOD_AUDIT
        self.fails(scenario, "never entered rewritten code", "--diagnostics")
        scenario = good_scenario()
        scenario["report"]["off"] = "binds 3 enters 4 steps 0 poisoned 0\n"
        self.fails(scenario, "ran with the component off", "--diagnostics")

    def test_the_duration_of_the_exact_audit_is_reported(self):
        """The exact audit holds every address space still for the length of
        its scan, so the timing of a diagnostics run is read with it."""
        result = self.passes(good_scenario(), "--diagnostics")
        self.assertIn("longest 3.2 ms", result.stdout)
        self.assertIn("total 9.0 ms", result.stdout)
        scenario = good_scenario()
        scenario["report"]["on"] = scenario["report"]["on"].replace(
            " nanos_total 9000000 nanos_max 3200000", "")
        self.fails(scenario, "no duration of the count audit", "--diagnostics")

    def test_the_count_audit_is_judged(self):
        """The run recounts the executable-mapping counts and fails on a
        mismatch, on an anomaly, or when nothing was recounted."""
        for report, expected in (
                ("binds 3 enters 9 steps 2 poisoned 0 refills 0\n",
                 "no count audit"),
                ("binds 3 enters 9 steps 2 poisoned 0 refills 0\n"
                 "count_audit runs 1 skipped 0 records 40 mismatches 0 "
                 "last_mismatches 0 anomalies 0\n", "needs at least two"),
                ("binds 3 enters 9 steps 2 poisoned 0 refills 0\n"
                 "count_audit runs 6 skipped 0 records 40 mismatches 2 "
                 "last_mismatches 0 anomalies 0\n", "2 records whose"),
                # What the last mismatching audit saw is in the failure.
                ("binds 3 enters 9 steps 2 poisoned 0 refills 0\n"
                 "count_audit runs 6 skipped 0 records 40 mismatches 2 "
                 "last_mismatches 2 anomalies 0\n"
                 "count_audit_detail generations 7 unnamed 0 c0 3 e0 2 c1 1 e1 0 "
                 "c2 0 e2 0 c3 0 e3 0\n", "c0 3, c1 1, c2 0, c3 0, e0 2"),
                ("binds 3 enters 9 steps 2 poisoned 0 refills 0\n"
                 "count_audit runs 6 skipped 0 records 40 mismatches 0 "
                 "last_mismatches 0 anomalies 3\n", "3 accounting anomalies"),
                ("binds 3 enters 9 steps 2 poisoned 0 refills 0\n"
                 "count_audit runs 6 skipped 0 records 0 mismatches 0 "
                 "last_mismatches 0 anomalies 0\n", "compared no record")):
            scenario = good_scenario()
            scenario["report"]["on"] = report
            self.fails(scenario, expected, "--diagnostics")

    def with_stack(self, stack: str) -> dict:
        scenario = good_scenario()
        scenario["report"]["on"] = scenario["report"]["on"].replace(GOOD_STACK, stack)
        return scenario

    def test_the_stack_the_runtime_left_is_judged(self):
        self.passes(good_scenario(), "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("59200", "4096")),
                   "bytes of stack left", "--diagnostics")

    def test_a_report_without_a_stack_sample_is_not_a_run_that_had_room(self):
        sentinel = (1 << 64) - 1
        self.fails(self.with_stack(""), "no stack sample", "--diagnostics")
        self.fails(self.with_stack("stack_free 59200 stack_size 65536\n"),
                   "no stack sample", "--diagnostics")
        # What the sampler holds when no thread sampled at all.
        self.fails(self.with_stack(
            f"stack_free {sentinel} stack_size 0 stack_kind 0 "
            f"stack_task_bytes 65536 stack_engine_free {sentinel} stack_growth 0\n"),
                   "holds the sentinel", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("stack_size 65536", "stack_size 0")),
                   "zero bytes", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("stack_free 59200", "stack_free 70000")),
                   "bytes free of a stack of", "--diagnostics")

    def test_the_stack_size_is_the_kernels(self):
        # The runtime's assumption must be the kernel's configuration, and a
        # sample on a task stack must name that size.
        self.fails(self.with_stack(GOOD_STACK.replace("stack_task_bytes 65536",
                                                     "stack_task_bytes 32768")),
                   "configuration gives 65536", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace("stack_size 65536", "stack_size 131072")),
                   "names a stack of 131072", "--diagnostics")
        # A thread on a host stack measures against that stack.
        self.passes(self.with_stack(GOOD_STACK.replace("stack_size 65536", "stack_size 524288")
                                    .replace("stack_kind 0", "stack_kind 1")),
                    "--diagnostics")

    def test_the_growth_over_the_engine_is_bounded(self):
        self.fails(self.with_stack(GOOD_STACK.replace("stack_growth 2800", "stack_growth 20000")),
                   "deeper than the engine's own frame", "--diagnostics")
        self.fails(self.with_stack(GOOD_STACK.replace(" stack_growth 2800", "")),
                   "no stack sample", "--diagnostics")
        # The budget itself is allowed.
        self.passes(self.with_stack(GOOD_STACK.replace("stack_growth 2800",
                                                      f"stack_growth {runner.STACK_GROWTH_BUDGET}")),
                    "--diagnostics")

    def test_the_kernel_stack_comes_from_the_kernel_configuration(self):
        self.assertEqual(runner.kernel_stack_bytes(), 65536)
        with tempfile.TemporaryDirectory(prefix="aot-config-test-") as name:
            config = pathlib.Path(name) / "fragment"
            config.write_text("CONFIG_PAGE_SIZE_4KB=y\nCONFIG_KERNEL_STACK_ORDER=3\n")
            self.assertEqual(runner.kernel_stack_bytes(config), 32768)
            config.write_text("CONFIG_PAGE_SIZE_4KB=y\n")
            with self.assertRaises(runner.Failure):
                runner.kernel_stack_bytes(config)


if __name__ == "__main__":
    unittest.main(verbosity=2)
