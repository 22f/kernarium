# Testing and preparation

CMake/Ninja and CTest are the first-party source of truth. Start with the
narrowest relevant target or exact test, then run the one workflow that owns
the changed behavior. `cmake --list-presets=all` lists the preset surface.

## Test suites

Every first-party CTest has exactly one primary suite, set by the registering
helper in `cmake/UmlTesting.cmake`; `ctest -N -L <suite>` lists a tree's
registrations:

- `unit`: native C++/assembly behavior; no runtime Python, shell, kernel,
  network, CLI, or rootfs dependency;
- `tooling`: repository, build, manifest, and source-policy checks; and
- `integration`: guest, rootfs, GUI, network, and kernel behavior.

Performance and long external campaigns are not CTest correctness suites.

Rules every test follows:

- Every test has a hard `TIMEOUT` (a unit test without one gets 600 s). A
  tooling test's bound is sized from its duration at gate concurrency on a
  loaded host, not alone. A wait inside a test for something the test started
  is bounded below the test's own bound and is never retried or unbounded.
- CTest ends a timed-out test with SIGINT, then SIGKILL. A Python test keeps
  its scratch in a `with` block or a `tempfile.TemporaryDirectory`, because
  `unittest` runs no cleanup callback after `KeyboardInterrupt`.
- The registration helper adds a `<suite>-build-<tool>` fixture for each built
  tool (`$<TARGET_FILE:...>`) a test runs, so running a test by name after
  configure is enough.
- Tooling tests run tools against fixtures. They do not grep tracked source for
  expected tokens or restate values another file owns. Guest shell that a test
  runs on the host gets stubs for commands with host-wide effect, such as
  `sync`.
- Unit tests create scratch files through `tests/unit/scratch/` and remove
  them, never at a host-global path unless that path is the contract under
  test. Unix-domain socket paths use `$TMPDIR` (a build-tree path may not fit
  `sun_path`). A shell tooling test's `$TMPDIR` is its `test-scratch/<test>`.
- A unit test that reads data takes its path as a registration argument, not a
  compiled-in source or build path.
- Unit tests assert with the helpers in `tests/unit/require/require.hpp`, which
  abort at the caller's `file:line`; the same directory holds `waitUntil`,
  `Watchdog` and the other shared helpers.
- CPU instruction assets are generated in the build tree from checked-in
  tables and independent golden expectations, with Python's standard library
  only. The [source contract](../tools/instruction-corpus/README.md) owns their
  format and promotion; the binary assets are not tracked.

`repository-environment-portability` is the one tooling test that scans
tracked and new unignored files, and it looks for forbidden values: host paths
and account names, private addresses and ssh endpoints, a `Claude-Session:`
trailer, and Apple signing identifiers. A site adds its private tokens to a
local denylist (`UML_PORTABILITY_DENYLIST` or
`${XDG_CONFIG_HOME:-$HOME/.config}/linux-um-darwin/portability-denylist`). A
reviewed exception goes in `tests/tooling/cmake/environment-portability-allow.tsv`,
and an entry that matches nothing fails. `check_environment_portability.py
--list-rules` describes the rules.

`repository-license-headers` enforces the [licensing rule](../LICENSES/README.md)
by location; exceptions go in `tests/tooling/cmake/license-header-exceptions.tsv`
and `check_license_headers.py --list-rules` describes the rule.

For one test, use an anchored expression:

```sh
ctest --test-dir .build/presets/macos-unit \
  -R '^exact-test-name$' --output-on-failure
```

## Canonical host workflows

Every workflow needs the
[host prerequisites](../dependencies/README.md#host-prerequisites). A test that
boots ten vCPUs (`guest-mmu-stress`) skips on a smaller host. A guest test
states its vCPU count with `ncpus=`; the launchers' default follows the
[applications' vCPU limit](architecture.md#memory-and-executable-code).

```sh
cmake --workflow --preset check-unit        # the ordinary host check
cmake --workflow --preset check-tooling     # tooling tests alone
cmake --workflow --preset check-integration # see below
cmake --workflow --preset check-gpu
cmake --workflow --preset check-gpu-asan-ubsan
cmake --workflow --preset check-loader-asan-ubsan
cmake --workflow --preset check-unit-census # after a gadget cache or census change
```

The full sanitizer suites are separate (`macos-unit-asan-ubsan` with
`unit-asan-ubsan`, `macos-unit-tsan` with `unit-tsan`). Every sanitizer preset
instruments Objective-C++ too. Both sanitizers keep the product's macOS native
window, so a sanitized test takes a window both leave free
(`tests/unit/memory/native_window_test_support.hpp`), and AddressSanitizer
ignores `mlock()`, so cases that need wired guest RAM report themselves
unsupported there.

The `unit-gpu` test preset's name filter is the one selection of GPU workflow
tests: every registered `gpu-*` test plus the Virtio protocol, video and
platform-video tests the GPU configuration also builds. `uml_add_gpu_unit_test`
refuses a name that does not start with `gpu-`, configure derives
`gpu-unit-test-binaries` from the filter, and a test preset that inherits
`unit-gpu` must not set its own. A GPU unit test keeps Metal's shader cache in
its scratch directory, so a host that has run guest GPU workloads does not slow
it. The GPU workflows need the `apple-gpu-vulkan-experiment` dependency prefix;
a change across the Venus or KosmicKrisp boundary also runs
`check-integration`.

### Opt-in lanes

Each lane registers only when its input is named; the runner's `--help` or the
test's header holds the mechanics.

| Lane | Entry point | Checks |
| --- | --- | --- |
| `dependency-kosmickrisp-spirv-regeneration` | `cmake --preset macos-tooling -DUML_KOSMICKRISP_SPIRV_BUILD_HOST=<host>` | the prebuilt KosmicKrisp SPIR-V against a rebuild on the pinned Debian host |
| `apple-ios-mac-vk-gl-cts` | `macos-integration` with `UML_IOS_MAC_CTS_ROOTFS`, `_ROOTFS_RECEIPT` and `_CAMPAIGN` | a CTS campaign in the unmodified iOS device build on an Apple silicon Mac |
| `apple-ios-device-vk-cts` | `macos-integration` with `UML_IOS_DEVICE_VK_CTS_DEVICE` and `_CAMPAIGN` | a Vulkan CTS campaign on a connected iOS device |
| `guest-v4l2-decode`, `-gpu` | `UML_V4L2_PROBE_BINARY` | the [Virtio video decoder](../darwin/device/video/README.md) |
| `guest-v4l2-encode` | `UML_V4L2_ENCODE_PROBE_BINARY` | the Virtio video encoder |
| `guest-v4l2-mmap-capture` | `UML_V4L2_MMAP_CAPTURE_PROBE_BINARY` | blob-backed capture buffers allocate on an idle guest |
| `guest-futex-atomic` | `UML_FUTEX_PROBE_BINARY` | the futex contract in the [common kernel README](../kernel/common/README.md) |
| `guest-gbm-dmabuf` | `UML_GBM_DMABUF_PROBE_BINARY` | the virtio-gpu dma-buf contract |
| `guest-drm-vblank` | `UML_DRM_FLIP_PROBE_BINARY` | Virtio-GPU's vblank pacing and the grid it keeps across idle gaps |
| `guest-aarch32-kernel` | built from `tests/integration/aarch32/` | the kernel's AArch32 task support (needs `python3`, `as`, `ld` in the rootfs) |
| `apple-audio-playback`, `-interruption`, `guest-virtio-sound-delay-host` | `UML_TEST_APPLE_AUDIO=ON` | the real macOS audio output, the iOS interruption behavior (which needs an iOS Simulator runtime, and runs in a simulator device it creates and deletes) and a guest's stream delay on the real output |

The probe sources are built for the guest architecture; each header says how.
The real microphone is checked by hand because granting access is the user's
act ([audio README](../darwin/device/audio/README.md)).

The iOS GPU lane runs the unmodified iOS device build on an Apple silicon Mac
in Designed for iPad mode. The iOS Simulator renders on the CPU
([`graphics.md`](graphics.md#ios-simulator-software-renderer)), which suits UI
tests and no conformance or performance campaign. Only the automation
application boots a payload
([Kernarium](../Kernarium/README.md#automation-runs)), so package its runtimes
with `package-ios-frameworks.sh --automation` before either CTS lane.

A campaign is judged by its `summary.txt`, the guest's own verdict, never by a
console marker alone. A baked device campaign is the only lane whose guest
memory the platform fixes, so it is the only one that leaves cases out
(`DEVICE_EXCLUSIONS` in `run_vk_cts.py`); an excluded case is neither a pass
nor a failure, and the Mac lanes run the released selection unchanged. Host and
renderer diagnostics share the device console with the artifact frames, so a
condition on a per-frame or per-bind path reports once.

`runtime-restart-*` covers a guest that restarts itself in the same process: at
each of 64 hand-backs nothing the test kernel left behind may remain, and the
process must be as it was before the first run.

`guest-irq-takeover` models a host that does not run a vCPU: its runner inserts
a library into the launcher (`DYLD_INSERT_LIBRARIES`, as `runtime-restart-*` does
its process audit) which suspends CPU0's carrier thread for a while every second,
and the test requires a vsock echo that begins in a hold to return soon after the
slowest echo without one, with every CPU busy, with only CPU0 busy and with
every CPU idle. An echo already under way when a hold begins is not judged, and
a hold that lands inside CPU0's dispatch stays with CPU0, so a few holds per
phase may fail. A phase with too few judged holds, and a host so busy that an
echo without a hold is already slow, fail the run. The library is test support
and nothing in the product loads it.

## Integration prerequisites and workflows

Integration needs four local inputs: the accepted kernel bundle below
`.build/kernel/artifacts`, the accepted boot base archive below `.build/rootfs`
([boot ramdisk](../tools/rootfs/boot-ramdisk/README.md)), the verified
dependency prefix, and the unified `rootfs.erofs` image. It needs no kernel
checkout or compiler.

```sh
cmake --workflow --preset check-integration
```

The workflow configures, builds, runs the tooling checks and executes the fast
integration suite; `integration-full` is the CTest preset for the complete
guest set. `ctest -N -L integration` lists the guest tests, and each runner or
probe states in its header what it checks. A contract's owning README, such as
[`../kernel/common/README.md`](../kernel/common/README.md), names the tests
that check it.
A test of a suspend contract, such as `guest-video-suspend` for the Virtio video
devices, suspends the guest both as the host's quiesce does and as the guest does
by itself, with a stream in progress as well as idle. `vhost-user-held-requests`
checks the transport's rules for a device that holds requests across a ring
disable, `vhost-user-backend-wait` the budget of the wait for a backend
request's acknowledgement and the end of the main socket during it, in time the
test moves, and
`gpu-device-suspend-held-fence` the Virtio-GPU device's fenced command across
one, on a renderer the test controls. A driver path that only a
misbehaving device reaches, such as a device that was reset during a suspend
(`guest-video-camera-reset`) or is late to stop a stream
(`guest-video-camera-slow-clear`, `guest-video-camera-slow-stop`), is reached by
serving the guest the production device behind the test-only shim of
`tests/support/vhost_user`; no production target carries a switch for it.
A test that needs what only an earlier session leaves behind boots more than
once: `guest-v4l2-capture` first lets the host learn the synthetic camera's
colour data into a calibration directory of the run, because Bayer stills are
offered from the next session on.
A guest reaches no listener on the host; the tests that need one, and the test
that proves it, are described in [user network](../darwin/README.md#user-network).
`guest-hardware-input` (needs a GPU slot) checks that a Linux guest started
with `--hardware-input` registers and unregisters the keyboard, mouse and
pointer input devices as a VNC client plugs and unplugs them
([contract](../kernel/common/input/README.md)).

The serial harness boots each guest through the launcher's per-run initramfs
and the [harness session](../tools/rootfs/boot-ramdisk/README.md#harness-session).
The emulator stays in the harness's session and process group, so a signal to
the group (a timeout, Ctrl-C, CTest) reaches the guest, and a watchdog in the
group kills the guest if the harness dies alone. A behavior one program checks
inside the guest runs through `tests/integration/support/run_guest_probe.py`.

A local override of the kernel selects one whole bundle of the matching profile
through `UML_MAINLINE_618_KERNEL_ARTIFACT_DIR`; never override individual
files.

## Rootfs contract

`rootfs.erofs` is a unified immutable guest image; the per-run `/init` mounts
it read-only beneath a writable layer, and no runner names the root device or
uses it as scratch. Ordinary test guest state lives in a disposable OverlayFS
layer; the Steam appliance owns a separate persistent GPT data disk. A rootfs
update replaces the complete authenticated image through
`tools/rootfs/provision.py`; there is no in-place maintenance workflow.

The image owns accounts, credentials, login policy, gettys, desktop packages
and payloads under `/opt`; launchers must not invent replacement users or a
general login policy, and a compatibility overlay a launcher owns stays
confined to its image and session. `tools/rootfs/starter` separately owns the
writable starter disks and boot base archives ([README](../tools/rootfs/starter/README.md)).

The [x86-64 smoke image](../tools/rootfs/x86_64-smoke/README.md) qualifies the
x86_64-userspace kernel slice. That slice registers no CTest suites: its
runners (`tests/integration/x86_64/run_guest_x86_64_*.py`, `run_v4l2_*.py`,
`run_gpu_guest.py`) run directly with the `macos-x86_64` `uml-darwin` and an
x86-64 EROFS image such as the [x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md),
and the `x86-64-guest-runners` tooling test checks their options without a
guest. The runners take `--engine jit` for the macOS-only x86-64 JIT and
`UML_TEST_JIT_TIER=N` for the
[tiered engine](../darwin/cpu/x86_64/jit/README.md#tiered-execution); a change
that reaches either engine runs both. The
[Steam producer](../tools/rootfs/steam-x86_64/README.md) owns the gaming image
and its audit.

## Apple builds and Kernarium

```sh
cmake --preset ios-device && cmake --build --preset ios-device
cmake --preset ios-simulator && cmake --build --preset ios-simulator
scripts/build-xcframework.sh
```

Kernarium has runtime presets for Linux and Android on the iOS device
(`kernarium-ios-device*`) and the iOS Simulator (`kernarium-ios-simulator*`).
`Kernarium/scripts/package-ios-frameworks.sh` packages the selected runtime and
graphics slices for Xcode (`--sdk device|simulator`). No Simulator product is
built for x86_64, so a generic Simulator destination needs
`EXCLUDED_ARCHS=x86_64` on the `xcodebuild` command line. KosmicKrisp has a
device slice only; the app targets embed it with
`Kernarium/scripts/embed-device-framework.sh`. Both projects sign with the team
in the ignored `GuestHost/Config/Signing.local.xcconfig`
([setup](../GuestHost/README.md#signing)); without it a Simulator build works
unsigned.

Apple builds validate the kernel bridge ABI, page profile, product family and
`6.18` series of an accepted kernel bundle. They never prepare sources or
invoke a kernel or Android build, and rootfs bundles are launch inputs.
Kernarium's bundles and scripts are in
[`../Kernarium/README.md`](../Kernarium/README.md); its launch defaults are
`EmulatorConfiguration.swift`'s.

`Kernarium/scripts/run-ios-mac.sh` and `Kernarium/scripts/make-guest-bundle.py`
launch and assemble a bundle on an Apple silicon Mac (`--help`). Xcode's local
install service, driven by `xcodebuild test`, is the only command-line launcher
for this mode, and it passes each `TEST_RUNNER_`-prefixed variable to the app
without the prefix. An iOS application reads nothing outside its container, so
the launcher copies the rootfs into it. Derived data for a Mac run of either
application is never in a system temporary directory (`/tmp`, `/var/tmp`,
`$TMPDIR`): Gatekeeper rejects the installed app there ("bad app wrapper") and
the run never starts, so the first build phase of each application's target
(`scripts/check-run-location.py`, `apple-run-location`) fails a Mac build below
one. The shared schemes gather no code coverage; a coverage run passes
`-enableCodeCoverage YES`.

A change to the guest memory the applications offer is proven by a start through
the applications' runtime (`kernarium_runtime_loader_run`) at the largest offered
size, in the Simulator or on a Mac, not by a command-line launcher.

On a connected iOS device, Android runs go through Andarium
(`tests/perf/run_ios_device_android.py`, [`Andarium/README.md`](../Andarium/README.md#automation))
and Kernarium's device runners keep Linux; both share
`tests/integration/support/ios_device.py`, and their tooling tests run against
fakes, so no device is needed. A device run starts with at least five free
slots of JetsamEvent reports and of each tested process's crash reports, since
iOS drops the next report once a kind is full: otherwise the
[preflight](../tests/integration/support/ios_device_reports.py) copies the
reports listed at the start to a new directory on the Mac, outside every
checkout, checks each copy by type and size (not byte for byte), and only then
empties the store, failing the run if the slots stay short. A report that
arrives after that listing is moved without a checked copy, into the mover's
own directory. Android recovery qualification uses a fresh
disposable bundle with `TEST_RUNNER_KERNARIUM_ANDROID_BOOT_MODE=recovery`;
never run wipe checks against an existing user bundle.

Kernarium's opt-in `SpacePauseGuestTests` pauses the application's own Linux
or Android guest through a free-space number, then resumes and shuts it down
(`TEST_RUNNER_KERNARIUM_SPACE_PAUSE_TESTS=1` and
`TEST_RUNNER_KERNARIUM_SPACE_HARNESS=1`; the launch selects the guest as for
any run, and the test ends it).

`KernariumTests` runs on an iPad, in the iOS Simulator
(`Kernarium/scripts/run-ios-simulator-tests.sh`, for UI tests) and, as an iPad
app, on an Apple silicon Mac. The iPad is the reference and passes everything.
On the Mac up to four tests fail because of how the Mac hosts an iPad app:
`theKeyBarNeverCoversTheConsole` and `anUnfocusedConsoleKeepsEveryRow` (the
window ends above the screen bottom), and `theKeyboardButtonFocusesTheViewOnScreen`
and `anEndedRunIsNeverStartedAgain` (SwiftUI exposes its controls only while
application accessibility is on; the latter also needs an image to boot). In
the Simulator `HostVideoEncoderTests` skip and `HostGraphicsStackTests` states
the software renderer's contract. `KernariumAutomation` is the iPad's
automation host.

`kernarium-device-framework-symbols` and `kernarium-device-framework-embed`
(tooling) cover the device-only embed script, and `kernarium-host-stop-paths`
(tooling) pins the call sites through which the application ends a running
guest, each of which has the guest write back first. The hosted
`GuestStopTests` cover that stop's order and bound.

### Andarium

`guest-host-core` (unit, macOS) runs the Swift package `GuestHost/Core`, the
free-space thresholds, suspension check and pause both applications share and
the logic of the hardware keyboard and pointer (held keys, Caps Lock,
positions, buttons, scroll), and the latch of a run's host stop with the
guest's write-back before it (`RunStop`, `StopSettle`), and
`andarium-core` (unit, macOS) runs the Swift package `Andarium/AndariumCore`
with `swift test` (it includes the hardware input's plug policy and pointer
capture, the snapshot store on APFS clones of sparse
files in a temporary directory, with a crash between every two file calls of
each operation and a real error at each call in turn, the haptics session on
fake guests and players, the haptics wire against the golden frames in
`tests/unit/andarium/fixtures`, which the guest's vibrator, thermal and gamepad
relay test against too; the game controllers' wire against that directory's
gamepad frames, each family's reports read back through a test-only HID
descriptor parser, and the stream and session on a fake relay; the session
record's stall, network, storage and camera slots (a camera's mounting and
connection lines and its failed stills) against the golden slots there, which
`runtime-stall-record` and `runtime-session-record` check the runtime's writers
against, and that the record quotes no identity value (`runtime-session-record`);
and the notice that iOS denies the network, from the path's mapping to
the watcher's start and stop on a fake path source);
`andarium-motion-protocol`, `andarium-device-orientation`,
`andarium-haptics-protocol`, `andarium-haptics-player`,
`andarium-thermal-protocol`, `andarium-thermal-levels` and
`andarium-gamepad-protocol` (unit), `andarium-stage-system-image`,
`andarium-capture-screens` and `andarium-localization` (tooling) cover the
motion wire format (its device-orientation and magnetic field records and its
control frames, "ANC3" included, against the golden frames in
`tests/unit/andarium/fixtures`, which `andarium-core` reads too, and which
sensors the sub-HAL lists and which records it takes for each launch property)
and what the orientation sensor reports and when (`andarium-core` also
covers the magnetometer: its records, the control frame that asks for them, the
status mapping, the failure it keeps for the run, the field's source (its
watchdog, movement notice and stale callbacks, on a fake Core Motion and timer)
and the final check that a reading or failure already on its way is dropped once
the guest no longer asks for the field, on fake devices and guests; the hosted
`CompassHostTests` cover the availability decision and the boot request's
compass bit, and read the real manager's availability (a Simulator and a Mac
have no magnetometer) without starting a sensor; `CompassReadoutTests` cover the
development build's compass readout: its launch setting, its rate, the span
of every sample between two lines and its line), the vibrator
HAL's haptics wire (against those golden frames, whose path the test takes as
its argument), timelines and state, the thermal HAL's wire and the status,
temperature and thresholds of each severity, the gamepad relay's wire (the
ranges of every field and the frame reader), system image staging, the
screenshot script, the String Catalogs' check and their sync with the
compiler's extraction ([the gate after a build](../Andarium/README.md#languages)),
and `kernarium-host-features` (unit) the table of host features the boot
request carries to an Android guest. `kernarium-camera` (unit) checks
what the runtime tells an Android guest about the host's cameras (the camera
fields of the boot request, the composed layout and its recomposition at each
boot, the lens cover, the host memory a live session reserves (none for a
host with no cameras), the camera state (a blocked guest's open of a camera
included) and rotation, and that an unknown or empty
`KERNARIUM_CAMERA_SOURCE` never reaches the host's camera),
`camera-layout` (unit) that the layout name composed from a host's cameras is
one of the image's layouts, `camera-orientation` (unit) the sensor
orientation of every interface orientation and facing,
`camera-still-exposure` (unit) the exposure a still reports from a photo's
metadata, fed literal metadata and an in-memory image, never a camera, and
`camera-photo-dimensions` (unit) which photo size a camera is asked for, on
fake format tables
([Cameras](../darwin/device/camera/README.md)). The Camera setting's hosted
tests (`CameraFlowTests`, and `CameraOffNoticeBannerTests` for the notice that
Camera is off, drawn in a window) play iOS's
permission with a fake and open no camera;
the Simulator has no camera and `simctl privacy` cannot grant or revoke one, so
the denied, restricted and absent states are shown by the preview
(`-systemCamera` asks the Simulator's own permission).

The session's notices stack in one column: the hosted `SessionNoticesTests`
check their order, their place clear of the statistics capsule and the side keys,
the scrolling of a column taller than its room and that the application shows
nothing for memory pressure (no notice on screen, none of its words in any
language), and `FoldSessionHostingTests` checks a notice in every pose's HUD. The
runtime's own answer to memory pressure and the reports of it are checked by
`runtime-memory-pressure-response`, `runtime-memory-pressure-exit` (a report
after `exit()` destroyed ordinary statics), `kernarium-memory-pressure` and
`kernarium-loader-memory-pressure` with its `-exit` case, with no memory pressure
raised on the machine.

The application builds with Xcode (`AndariumPreview` is the Simulator build with
mock data, `Andarium` the device application,
[targets](../Andarium/README.md#targets)); `AndariumTests` is
hosted in `Andarium` and runs on an iPad, in the Simulator and as an iPad app on
an Apple silicon Mac (`xcodebuild test -scheme Andarium -only-testing:AndariumTests
FEEDBACK_ADDRESS=feedback@example.com REVIEW_ID=id0000000000 FOLLOW_X_HANDLE=example_handle
TIP_LITE=com.example.andarium.tip.lite TIP_PRO=com.example.andarium.tip.pro
TIP_MAX=com.example.andarium.tip.max TIP_ULTRA=com.example.andarium.tip.ultra
SHEET_DEVELOPER_NAME=example SHEET_DEVELOPER_GITHUB=example SHEET_DEVELOPER_X=example_handle
ICP_NUMBER=示例ICP备00000000号-0A`;
the scheme without the filter also runs `AndariumUITests`). The scheme's Test
action pins English, so the tests that read the application's words do not
depend on the language of the device; `LocalizationTests` names a lost pin and
looks each language up by name, and the language a `-testLanguage` run asks
for fails only the tests that read the English words. `SnapshotFileApiTests`
checks the file calls the guest's snapshots use (clone, private size, extent
map, backup attribute) on a small sparse file and prints their answers on a
`SNAPSHOT_FILE_APIS` line; a device reports a refused call there and does not
fail on it. `SnapshotsModelTests` and `SnapshotWordsTests` check the snapshot
screens' flows, refusals and words on a fake driver. `SendFeedbackScreenTests`,
`SendFeedbackStartLogTests` (the sheet's tip and start log during a start) and
`SendFeedbackFilesScreenTests` (files from Files: in place, the copy of a file
that cannot be used there, the access held and stopped once) exist only in a
build with `FEEDBACK_ADDRESS=feedback@example.com` on the command line, as the
command above has it ([Send feedback](../Andarium/README.md#send-feedback)); a
run without it does not build them. Each other feature's tests need the example
values of its own: `FollowXRowTests` `FOLLOW_X_HANDLE=example_handle`
([Follow developer on X](../Andarium/README.md#follow-developer-on-x)),
`ICPFootnoteTests` `ICP_NUMBER=示例ICP备00000000号-0A`
([ICP filing number](../Andarium/README.md#icp-filing-number)),
`TipJarScreenTests`, `TipJarHeaderTests` and `TipJarStoreKitTests` the four
`TIP_` settings and the `SHEET_DEVELOPER_*` placeholders
([Buy me tokens](../Andarium/README.md#buy-me-tokens); the last plays
StoreKit's test session over `AndariumTests/TipJar.storekit`, example
identifiers only, never the App Store), and `ReviewScreenTests`,
`ReviewAskFlowTests` and `ReviewAskStoreTests` `REVIEW_ID`
([Review on the App Store](../Andarium/README.md#review-on-the-app-store); the
system's rating prompt is never asked for). `HomeMenuTests` (Home's toolbar
menu) exist with any of Send feedback, Review and Buy me tokens, and
`HomePlainInfoTests` (the plain Info button) only with none
([Home toolbar menu](../Andarium/README.md#home-toolbar-menu)).
`AndariumCoreTests` holds the tip sheet's states and the rating rule, counter
and record as pure logic, and the state of the no-Google-apps notice (the panel
on a first start and on the retry of a first Android boot that did not finish,
and nothing else; the Send feedback line until opened or closed).
`NoGoogleNoticeScreenTests` host the real boot screen and Send feedback sheet
for it (the panel reached on those starts and gone with the boot, the starts
that show none, the line opening and closing in place, the words in each
language), and `NoGoogleNoticeUITests` boot a real first start and reach the
line (see the UI-test target below).
The Mail composer, the picker and the guest's display are stand-ins. The hosted tests
share the application's one key window: a test that replaces its root view
controller or connects a window takes it with `HostWindow.take()`, and none
opens the system's Settings or another app. The bundle's principal class
(`AndariumTestBundle`) shows the side keys' tip in every session of the run,
whatever the device's tip store says, except in an `ANDARIUM_HOLD_SECONDS`
harness launch, which hides it. No test attaches, scans for or pairs a real
controller: the game controller tests use software controllers and the
system's virtual controller (which needs the host's window) against a fake
relay, and the package's test-only `andarium-gamepad-probe` plays a script to a
TCP port, where the macOS launcher's `--vsock-forward` reaches a guest's relay.
The script and the guest end of the wire are the package's
`AndariumCoreTestSupport` target, which only the package's tests, the probe
and `AndariumTests` link, and the test bundle carries in its own Frameworks
folder, so a device can load it; the application never does (the App Store
bundle check refuses a build that does).
`DeviceOrientationTests` checks UIKit's orientation numbers against the
application's readings, the mapping to Android's rotation, the observer's begin
and end of the device's notifications, the path from a turn to the record a
fake guest reads on a device without sensors, and that the Simulator and a Mac,
which have none, hold no connection for them. The Simulator cannot turn an
iPhone's `UIDevice` under a portrait-only interface (XCUIDevice reaches the
interface only), so the device is injected there; the real turns are checked on
an iPhone.

`SlowStartScreenTests` check the boot screen's slow-start notice by a clock
the test moves, so no test waits three minutes: when the notice comes (also for
a start whose boot line never comes, the case it is for), what Stop asks of the
runtime (after the dialog's answer) and the stopped end screen's words, the
later notice and plain title of a first start, the controls with and without
the feedback receiver and their size (the application's ordinary 44 points at the
default text size, no less at any size), that the sheet the notice opens outlives the boot, that
the alert and the sheet take the appearance of the screen under them, the
Boot line of the information block, and that the notice goes with the boot.
`StartHandoverTests` run the real runtime object with the files' preparation
held, and check that a stopped start's preparation is the only one at a time,
that what changes the files waits for it and is not overtaken by a start asked
for after it (a Restore on the real storage), that an old boot's console cannot
mark the next boot, not even through a restart that printed no banner, that a
guest whose first boot did not complete starts with the longer time, that the
notice's clock stands still while the application is away, and that the stop is
made off the main actor until the run ends, after the first guest it finds was
asked once to write back (and none for a run that has ended).
`SlowStartCaptureTests` save the notice in windows the Simulator's own capture
cannot take (an iPad turned, a narrow Split View window, the Duo's poses) and
do nothing unless `TEST_RUNNER_ANDARIUM_CAPTURE_DIR` names a folder.
`GuestRecoveryTests` check that a recovery the guest asked for is a recovery
session and its reboot a normal one.

`OldAppCompatibilityTests` (`andarium-core`) read the image's old-app command on
canned output of its specification (the version line and the five fields, an
unknown trailing field, a label with odd characters, a missing command, an
unknown version, a usage error, a refusal) and send it nothing that is not a
package name; `OldAppsModelTests` play the page's model against a stub runtime
and a scripted list (the run's first listing with Settings never opened, when
the virtual machine is asked, a listing older than a change, an alert for a
page that was closed, what each switch asks first, what a refused change
leaves, the hint after Install anyway), `OldAppsScreenTests` host
Settings and the page, `OldAppsOperationTests` run the adapter through the
application's adb channel against the fake adbd, and `OldAppsWordsTests` read the
words in each language.

`AndariumUITests` is the UI-test target of the same scheme
(`xcodebuild test -scheme Andarium -only-testing:AndariumUITests`, with
`-parallel-testing-enabled NO`), run on an iPad Simulator and an iPhone
Simulator. Its launch asks for the input probe
([automation](../Andarium/README.md#automation)): XCUITest's synthetic keys
and, on an iPad, pointer events (XCUITest has no pointer on an iPhone) reach
the real session and the guest's end is a log the test reads, so the tests
cover the hardware keyboard and mouse without a guest. They cover the keys as
positions, the plug order, no software keyboard, a sheet's text field taking
the keys while it is open and a sheet without one taking none from the guest,
the pointer's hover, clicks, drag and scroll, "Use the pointer as touch" (the
click is a touch of the touchscreen with it on, the mouse's without it), and a
guest that never registers its mouse, whose pointer stays the touchscreen's.
`InterfaceRotationUITests` turns the Simulator through the four orientations in
a session: an iPad's interface follows, an iPhone's stays portrait. The
results carry how many attempts each repeated step took. The Simulator produces
no Escape, Delete, Caps Lock, F13, lone Shift or Globe key, and no raw mouse
motion under the pointer lock. `SideKeysTipUITests` leaves the side keys' tip
on and taps the keys once: they open. `OldAppsUITests` reach Settings > Apps > Old
app compatibility in a probe session with a scripted list of old apps
(`ANDARIUM_OLD_APPS_PROBE`) and flip the switches: the alert that asks before
turning on, the one for an open app turned off, a change the virtual machine
refuses, and no row without the command. `NoGoogleNoticeUITests` holds the one
test of the target that is not a probe session, and boots a real guest: it
creates a virtual machine in the Simulator and starts it for real, so it needs
the Simulator Android runtime packaged and the system image staged
([Build](../Andarium/README.md#build)), and it takes about a minute to reach
Android (the test allows ten minutes to create the machine and 25 for the
boot). Its other test, in a build with Send feedback, reaches the Send feedback
line from a probe session's Settings.

The folding iPhone's layout ([devices](../Andarium/README.md#devices)) is
`andarium-core`'s `FoldLayoutTests`, one test per pose of the iPhone Duo.
`FoldSessionHostingTests` puts the real scene root in a window of each pose's
size through the reader's stub, so it runs on every Simulator: the guest's
frame, nothing of Andarium's on the fold, the guest kept and resized once where
its shape changes, and the rotation rule. `TheRealDisplay` reads the iPhone Duo
Simulator's own display in the pose a harness put it in
(`TEST_RUNNER_ANDARIUM_FOLD_POSE`; a harness outside the repository poses the
Simulator, which no Xcode control does) and checks nothing without it.

The opt-in suites each need a `TEST_RUNNER_ANDARIUM_*` switch that the suite's
header names: rotation, motion compatibility, power-off and over-vsock,
shutdown flow, low-space pause (a real guest paused through a free-space number the test
sets, its CPU time standing still), the graphics-memory notice (a real refusal
in Chrome's WebGL under the host's GPU budget, on a guest with Chrome
installed), the graphics-memory reading and the process list the reclaim
parses (any guest), the session record's network slot (a real guest's DHCP
lease, `NetworkRecordGuestTests`), Retina memory, the resolution a real guest
gets (`ResolutionGuestTests`: the size and density Android reports at a
scale), the ADB-over-vsock guest tests (including Android storage and the
touch-cancel path), Install APK of an app in parts against a guest's adbd on a
loopback port (`ContainerGuestTests`: `TEST_RUNNER_ANDARIUM_CONTAINER_GUEST_PORT`
and a folder of synthesized APKs in `TEST_RUNNER_ANDARIUM_CONTAINER_GUEST_FIXTURES`; an optional
`..._GUEST_SAMPLES` folder of numbered folders runs a person's own container files through it), the side keys' pass-through of touches to Android, the
ADB-over-network hold, the background disk locks (which also requires a guest
that was allocating buffers when the GPU was held to suspend), the resume
after a save while the application is away, the real `HostLocation` against
the Simulator's simulated location (the suite's header says how to grant
access and set one), the cameras a booted guest sees (`CameraGuestTests`: the
vendor SKU, the camera IDs and the lens cover of a live and a blocked session
on the synthetic camera, with `TEST_RUNNER_KERNARIUM_CAMERA_SOURCE=synthetic`,
on an image that has the camera HAL) and the hardware keyboard and mouse
against Android's input devices (`KeyboardMouseGuestTests`, which needs a
kernel that follows configuration changes and its Android release). The
storage test signs its helper APK with the AOSP test key, never a release
key. When derived data is on a removable volume, macOS prompts once for
access and the launch hangs until answered; build to the internal disk
instead.

Tooling tests cover `scripts/check-privacy-manifests.py`
(`ios-privacy-manifests`, which `package-ios-frameworks.sh` also runs on every
device slice it packages), `Andarium/scripts/check-app-store-bundle.py`
(`andarium-app-store-bundle`, the last build phase of the AppStore
configuration, [configurations](../Andarium/README.md#configurations)), the
tip sheet's words (`andarium-tip-words`: `Andarium/scripts/check-tip-words.py`
rejects the forbidden terms of each language and a token amount in the
`TipJar` table, and requires the statement that a tip is voluntary, one-time
and the features free; the bundle check also covers the developer header's
keys and artwork),
document pickers' build conditions (`andarium-document-pickers`: the App Store
configuration opens files in place, every other presents the copy-mode picker,
and an in-place importer anywhere else is in an App Store branch or a named
exception),
the Acknowledgements resource (`andarium-acknowledgements`) and the stand-in
debugger that serves the JIT's request for executable memory on a device
(`andarium-ios-jit-debugger`). The JIT's region route has host tests that run
on macOS (`arm64-jit-debugger-region`, `arm64-jit-debugger-request` for a
request no debugger serves, `arm64-jit-memory-preparation`); a run on a device
needs a debugger, so `run_ios_device_android.py --engine jit` is its lane.

### UI automation on a device

Unattended device automation goes through the long-lived `KernariumAutomation`
UI-test host, because each new XCTest session on a device with a passcode needs
the owner's approval. `Kernarium/scripts/ipad-automation.py --help` has its
commands and failure modes. Keep all automation code in the test targets. The
host's test run records no screen video and keeps no attachments (the scheme
sets it, and `start` refuses a built run file that does not), and its result
bundle lives under the state directory, not in DerivedData.

A host ends on its own now and then. A new one needs the owner at the device
only when the system lost the last approval, however soon it starts: when
xcodebuild's relaunch of the runner initialized (its log says so) nothing is
asked and the watch replaces the host at once; when it failed to initialize, or
the log does not say, the owner may be needed and the watch ends. A host's
supervisor tries to end xcodebuild as soon as its log reports the connection to
the device lost, before xcodebuild starts a runner that cannot initialize (best
effort), so that death is then a plain one the watch replaces. `watch` keeps
one device's host serving, and acts only on evidence: it adopts a serving host,
starts the next only once the old one is shown to be dead (three reads in a row
that all worked and all showed the same beat), and otherwise replaces nothing
and ends with a status and state of its own that `status` reports (approval the
device may have asked for, a heartbeat it cannot read, a remainder of the old
host that is not shown gone). The refusal for approval stays in the state
directory until a host serves, and a device with no host to read is the first
`start`'s job. A command in flight when its host is replaced is not repeated,
and its sender gets status 3. Run it in a terminal, detached with
`watch --detach`, or as a job manager's job (`watch --print-launchd-job
<label>` prints a launchd job that launchd never restarts, and creates nothing:
make its log directory first; launchd must be able to open the job file and the
log, so both belong on a volume with ownership enabled, and `--launchd-log`
names the log); `stop --force` ends the watch before the host. The printed job
asks for interactive scheduling, and a running watch holds an idle-sleep
assertion (never a display one) and logs any turn of its loop that starts late.
The `kernarium-ipad-automation-watch` and
`kernarium-ipad-automation-watch-process` tooling tests run the real script
against fake `xcrun`, `xcodebuild` and `caffeinate` executables.

### Edge gestures (device only)

The Simulator shows which edges a run defers (`AndariumPreview -screen
session`) but not what iPadOS does with the swipes. Check on an iPad, under a
lease with the automation host and a guest using 3-button navigation, whether
the host's synthesized edge swipes start system gestures and what iOS does for
each deferred edge. A run keeps the home indicator visible, which the bottom
edge's deferral needs.

### Window layout (device only)

Window layout follows iPadOS windowing, which only a device has. Whenever the
guest view's layout changes, check on an iPad in Full Screen Apps, Windowed
Apps and Stage Manager, switching Multitasking under the running application in
both directions: the view must move at once, resize the guest display
(`wm size`), keep touch coordinates mapped and show no lost GPU backend. In
each mode also turn the iPad through its four orientations in a session: the
interface and the guest's display follow, the last frame keeps its proportions
meanwhile, and a game that asks for one orientation is letterboxed in the
other. Restore Full Screen Apps before releasing the lease.

### Background lifecycle (device only)

Only a device revokes the GPU, freezes the process and reclaims it. Whenever
the lifecycle handlers, the GPU hold or the quiesce change, background the
application on an iPad for a KDE and an Android guest (about 20 s and two
minutes) and read its console (`xcrun devicectl device process launch
--console`). Each cycle must show the GPU requests held and the guest's
`PM: suspend entry`, then on return the release, `PM: suspend exit`, the display
rendering, and no `virtio_gpu` response errors, Metal errors, RCU stalls or
watchdog lines; on a device it must also show guest RAM hibernated and
restored. A hold count of `0` means a request was still running after a second:
a finding, not a pass. The low-space pause adds a cycle: pause the guest
(on a device no hibernation image appears in the temporary directory, and the
guest's processor time stands still), send the application to the background
and back, then Resume; the guest must stay suspended and resident until
Resume, then wake as above. The unit tests
(`runtime-thread-fence`, `runtime-guest-hibernation`, `emulator-lifecycle`, `memory-guest-ram-image`)
cover the order on any host, and `--guest-hibernation=on` forces the same code
on a Mac. The Picture in Picture and keep-running-job modes
(`KERNARIUM_BACKGROUND_MODE`) and the restart notice after a terminated
background process are checked the same way; remove the guest bundles and
payloads such a check copied to the application's Documents afterwards.

### Guest statistics

The host side of Andarium's live statistics is covered on any host by one unit
test per source (`runtime-guest-statistics`, `guest-console-memory`,
`gpu-display-statistics`, `emulator-statistics`, `emulator-lifecycle`,
`kernarium-statistics`, `emulator-statistics-report`) and the package
consumers. `guest-statistics` (integration, GPU) boots a Linux guest with
`--stats` and checks the CPU, frame and memory figures against the guest's own;
an Android check runs `uml-darwin-android17 --stats=1000` under enforcing
SELinux and requires the memory part with no `avc` denial.

## LTP

The full LTP campaign is opt-in and runs only on the gadget or JIT engines.
`tests/integration/ltp/verified-tests.tsv` and `disabled-tests.tsv` are the
exact manifests; a disabled case is an intentional product or kernel-policy
exclusion, and a kernel feature is never enabled solely to raise the count.
The runner uses `/opt/ltp` from `rootfs.erofs` and the kernel bundle's
`modules.builtin` (the shipped kernels have `CONFIG_MODULES=n`). Case bounds
are sized on the gadget engine and scaled for `--engine jit`, which compiles
every exec'd process from scratch
([cold code](../darwin/cpu/arm64/jit/README.md#cold-code)); the factor is
recorded in the results. `--guest-arch x86_64` runs the manifests on the
[x86-64 LTP rootfs](../tools/rootfs/ltp-x86_64/README.md) with the overrides in
`tests/integration/ltp/x86_64/`, which exist only where that userspace or the
x86 ABI changes a case's outcome. `python3 tests/integration/ltp/run_guest_ltp.py
--help` lists the options.

## CPU instruction tests

Instruction semantics belong to the corpora, each owned by its README:
[A64](../tests/unit/arm64/corpus/README.md) (inventory in
[`a64-instruction-coverage.md`](a64-instruction-coverage.md), representative
cases rather than exhaustive variant proof),
[AArch32](../tests/unit/arm64/aarch32_corpus/README.md) and
[x86-64](../tests/unit/x86_64/corpus/README.md). Engine behavior the corpus
cannot hold has focused tests whose headers say what they check
(`ctest -N -R '^arm64-(a32-|gadget-a32-|jit-a32-)'` lists the AArch32 ones; the
AArch32 JIT tests exist only on macOS). A guest `SIGILL` is a correctness
blocker in every one of them.

- `arm64-instruction-corpus` runs the generated A64 corpus through both
  engines; the special records (native SME, absent features, the SM = 0 family,
  top-byte ignore, native floating-point traps) have their own
  `arm64-instruction-corpus-*` and `arm64-absent-*` tests that re-derive them
  from their generators, and records that need SME or FEAT_AFP/BF16 run only on
  a host that has them.
  The runner also runs a case again, up to 16 attempts in all, when its
  program is a load, one store-exclusive that discards its status, CLREX and
  SVC in four straight-line words and the only difference from the pack is
  that store's bytes unwritten: a spurious failure the architecture allows and
  a guest's loop would absorb. Every other program runs once, and the
  [corpus README](../tests/unit/arm64/corpus/README.md) states the rule.
  `arm64-instruction-corpus-exclusive-interrupted` provokes the loss on the
  gadget lane with the host interrupting the guest's thread, and
  `-exclusive-injected` makes the first attempt end as a lost reservation and
  as each fault the retry must refuse in both lanes, and checks the rule on
  programs that do not execute their store. The two unit tests that run a pair
  once, `arm64-tlb-probe-set` and `arm64-gadget-sme`, retry only a result that
  is exactly a lost reservation: both check the status, the loaded value and
  the watched memory, and the SME test also the other registers, SP, PC, NZCV
  and the fault fields. Each checks its rule with results that differ in one
  field.
- `arm64-emitted-registers` requires that no host code names x18, which Darwin
  clears when it interrupts a thread.
- `arm64-instruction-corpus-invalid-encoding` requires every
  illegal-instruction case to carry the invalid-encoding bit that
  `tools/arm64-tests/invalid_encoding.py` decides; its `-llvm` sibling is
  opt-in with the pinned LLVM MC.
- `arm64-aarch32-decoder`, `arm64-a32-encoding-table`, `arm64-aarch32-corpus`,
  `arm64-a32-corpus` and `arm64-aarch32-semantics*` check the AArch32 decoder,
  generated tables, corpus and semantic library without an engine;
  `arm64-aarch32-corpus` also proves the store-exclusive ranges the retry
  compares and what it refuses. The corpus runs of `arm64-gadget-a32-corpus` and `arm64-jit-a32-corpus` retry a
  case that differs only by a spurious store-exclusive failure (its own bytes
  unwritten, the case's other stores in the data), and the `-interrupted`
  tests provoke that failure on purpose. Only the gadget one runs the spill
  suite, whose cases store between the pair: the JIT does not lose a
  reservation there. The `-injected` tests make an attempt end as a lost
  reservation with another register or a thread register wrong, and as other
  faults, and require that only a plain loss is retried.
- `guest-aarch32-engine-gadgets` and `-jit` boot the freestanding engine probe
  in `tests/integration/aarch32/engine_probe/` and count every `SIGILL`.

To change the A64 oracle, its generators or the pinned Arm release, configure
with `-DUML_A64_ARCHITECTURE_ARCHIVE=<archive>` and run the opt-in
`arm64-instruction-oracle-freshness` and `arm64-sme-encoding-table-freshness`
tests. The AArch32 equivalent is `-DUML_A32_ARCHITECTURE_ARCHIVE=<archive>`
with `tests/tooling/arm64/generate_a32_encoding_table.py` and the
`arm64-a32-encoding-table-freshness` and `arm64-a32-corpus-plan-freshness`
tests. All skip without the archive or the pinned LLVM MC.

The guest-SME policy is on by default and `DARWIN_UML_GUEST_SME=0` turns it
off; the unit suites hold either way on every host. On an SME host (an M4 Mac)
also run the `arm64-sme-*`, `arm64-jit-sme-*` and corpus tests both ways.
`guest-sme-gadgets`/`-jit` and `guest-sme-off-gadgets`/`-jit` check the guest
kernel's SME contract, and `guest-no-sve-gadgets`/`-jit` that no SVE appears
outside streaming mode. The opt-in `tools/arm64-tests/sme-sm0-scan.py` and
`sme-idiom-corpus.py` list what shipped code executes outside streaming mode.

A gadget that runs an instruction as the host's own (FEAT_ECV, FEAT_I8MM,
FEAT_BF16, FEAT_FHM on an A15 iPad; FEAT_AFP and FEAT_RPRES on an A17 Pro or an
M4 Mac) runs that path only on a host with the feature. Check a change to such
a path by hand on an iOS device that has it: launch Kernarium with the
`guest-hwcap-sanity` payload and the touched families' corpus cases built into
one static guest program, and require the results of a gadget-engine run on a
Mac. The device runs the gadget engine, and the JIT only in a development build
whose debugger serves it, so a JIT path needing one of these features is checked
on an M4 or newer Mac.

## Android

Build-side contracts need no Android build: `lineage24-target-files-fixtures`,
`lineage24-builder-lane`, `lineage24-release-contract`,
`lineage24-first-boot-precompile` and the `lineage24-mesa-venus-*` tests run
the producer's verifiers over fixtures, and
`android-partition-boot-selection`, `android17-instance-rules` and
`android17-macos-launcher-options` cover the host library and launcher over the
storage layout (`android17-launcher-arguments`,
where the Android CLI is built, runs the executable's own argument checks)
([storage](../android/lineage-24/README.md#static-disk-and-boot-contract),
[block device](../darwin/device/block/README.md)).
`lineage24-camera-app-contract` checks that the camera application's overlay,
its patch series and the target-files verifier's constants name the same
settings and values; what the application then does is the image's to show.
`guest-android-disks` (integration) boots the Linux test guest on a release
and a data disk the library created. `gnss-hal-nmea`, `gnss-hal-policy`,
`gnss-hal-engine`, `gnss-hal-entry` and `gnss-hal-callbacks` (unit) compile the
guest GNSS HAL's headers
([Location](../android/lineage-24/README.md#location)) against the host
writer's own epochs: the reading of the stream, the reporting rules, the
session and the AIDL entry points, with a pipe for the receiver and a counting
wakelock, and with a framework callback that does not answer, so that no entry
point waits for one and what the framework hears is the delivery contract's.

A release candidate is qualified on its image, opt-in, on every engine that
runs Android (the Mac gadgets and JIT engines, the iPad gadgets). A `SIGILL`
anywhere is a blocker. The candidate must show:

- **Boot.** `boot_completed` with the 64-bit and 32-bit ABI lists, the
  secondary zygote running without restarts, SELinux enforcing with no denial
  from the 32-bit processes, and no package compiled on first boot.
- **Applications.** Real 32-bit applications (`armeabi`-only, `armeabi-v7a`-only,
  dual-ABI installed with `--abi armeabi-v7a`, Java-only) select the expected
  primary ABI, descend from the secondary zygote, draw through GLES (and
  Vulkan where used), and leave no tombstone; a deliberate `SIGSEGV` leaves a symbolized
  `crash_dump32` tombstone. The legacy `armeabi` smoke uses three unmodified
  F-Droid archive builds (Simon Tatham's Puzzles 9561.1, Gears ES2 7, Gloomy
  Dungeons 2013.03.07.0416) on the physical iPad, installed with
  `--bypass-low-target-sdk-block` and no `--abi`.
- **CTS.** The 32-bit and 64-bit bionic, binder, OS, JNI, seccomp, ABI-override
  and ASLR modules; a test that fails only on `armeabi-v7a` is triaged before
  acceptance. `CtsUiRenderingTestCases` qualifies on the iPad's gadget engine,
  not on the shared Mac, where `ColorSpaceTests#testMultiAtlasGlyphsWithColorSpace`
  can exceed its 60 s wait when other guests load the host; record such a Mac
  failure as load ([G289](optimization-history.md#ios-graphics-and-later-gadget-work)).
- **Alignment.** The engine probe run from `/data/local/tmp` on both engines
  gives `SIGBUS` for misaligned LDM, STM, LDRD, STRD and VLDM, as on Linux.
- **Cost.** Boot time (cold and warm, ABBA against the previous release on the
  same host build), `MemAvailable`, the zygotes' `dumpsys meminfo`, and image
  sizes.
- **Storage.** The read-only partition hashes match the layout, the guest boots
  on a blank data disk it formats itself, recovery's "wipe data" resets
  `userdata`, a newer release keeps an older instance's apps and data and an
  older release on a newer instance is refused.
- **Hardware video** (a guest started with `--video`). `MediaCodecList` lists
  the four `c2.v4l2.*` codecs with no SELinux or seccomp denial; the encoder
  and decoder classes of the media CTS modules pass on both ABIs through the
  hardware codecs, which the host's video activity log confirms, and the
  software `c2.android.*` results do not regress. Run the decoder performance
  classes on a quiet host.
- **Hardware keyboard and mouse** (a guest started with `--hardware-input`;
  `guest-hardware-input` is the Linux guest's check). `android-keymouse`
  passes, and `CtsInputTestCases`, `CtsHardwareTestCases`, `CtsViewTestCases`
  and `CtsInputMethodTestCases` pass with the launcher's default devices and
  again with the option and a connected client; a result that differs from the
  accepted release's is explained
  ([Keyboard and mouse](../android/lineage-24/README.md#keyboard-and-mouse)).
- **Cameras** (a guest started with `--camera=synthetic` and a camera layout on
  `androidboot.product.vendor.sku`). `android-camera` passes for the layouts
  the synthetic profiles compose, live and blocked; the VTS camera provider
  test (`VtsAidlHalCameraProvider_TargetTest`) passes against the lazy
  provider; and these CTS modules pass sharded, with the failures rerun alone
  and a result that differs from the accepted release's explained:
  `CtsCameraTestCases` (about 6 to 15 hours, as detached shards),
  `CtsCameraApi25TestCases`, `CtsMediaRecorderTestCases`, the camera parts of
  `CtsAppOpsTestCases` and `CtsPermissionTestCases`, and
  `CtsSensorPrivacyTestCases` in a blocked session and in a live one. The
  provider process runs from cameraserver's start until cameraserver holds no
  camera, and again while an application holds one, no denial of
  `hal_camera_default` appears in the log, and the verifier's check that
  `libgooglecamerahwl_impl.so` holds the names of route `p0030` is not
  vacuous: it runs on the candidate's built library (a source tree has none)
  and is shown to fail on the stock one. The camera application shows a lens
  button for each back lens and offers no pinch zoom, zoom slider or Zoom
  action ([Camera](../android/lineage-24/README.md#camera)).

`android-oobe`, `android-adb-debugging`, `android-guest-proc` and
`android-installation-identity` (integration) each register only when
`UML_ANDROID_OOBE_RELEASE`, `UML_ANDROID_ADB_DEBUGGING_RELEASE`,
`UML_ANDROID_GUEST_PROC_RELEASE` or `UML_ANDROID_INSTALLATION_IDENTITY_RELEASE`
names the release and the Android macOS executable is built (`UML_BUILD_ANDROID17_CLI`
with `UML_KERNEL_PRODUCT=android-17`: the executable embeds its product's
kernel, and the Linux default cannot start Android's init). They boot blank
instances and check the setup wizard's flow and end state, the product's adb
defaults, that the command Andarium's bug report sends for the guest's
`/proc` counters (`GuestProcFiles`, its one source) reads each file or says why
not, and that each instance's guest reports its own serial number and eth0
address, kept across a reboot and a new launch
(`android-installation-identity-image` registers when
`UML_ANDROID_INSTALLATION_IDENTITY_IMAGE_RELEASE` names a release built with the
overlay and also checks wlan0's derived address, the Wi-Fi lease, that eth0 and
the macvlan carry no IPv6 link-local address, and that a probe application in an
ordinary application process cannot read `ro.boot.kernarium.adb_key`; that
the guest's adbd reads the key is proven on a Kernarium runtime guest by
`AndariumTests/AdbOverVsockTests` (the application's own adb client; the macOS
launcher fixes ADB authorization off, and `run_android_adb_debugging.py` cannot
be authorized by a runtime guest, which accepts only the key its application made);
`tests/integration/android/run_guest_android_adb_vsock.py` is the
opt-in ADB-over-vsock counterpart of `guest-vsock-stream`.
`android-microg-spoofing` (integration) additionally needs
`UML_ANDROID_MICROG_PROBE_DIR`, the output of
[`microg/build-apks.sh`](../tests/integration/android/microg/README.md), and
checks that only microG's own packages present Google's certificate.
`android-keymouse` (integration) registers when `UML_ANDROID_KEYMOUSE_RELEASE`
names the release and `UML_ANDROID_KEYMOUSE_PROBE_APK` the
[probe application](../tests/integration/android/keymouse/README.md)'s APK: the
runner is a VNC client of the launcher's `--hardware-input` display and checks,
through Android's input dump and the probe's log, that the host's devices
plug, are configured as the image says, deliver keys, the cursor, buttons and
wheel, pass pointer capture both ways and unplug; `--instance` names the
previous release's data disk to check the one-time on-screen keyboard change.
`android-video-suspend` (integration) registers when
`UML_ANDROID_VIDEO_SUSPEND_RELEASE` names the release: the gallery's decoder and
`screenrecord`'s encoder run across a suspend by the host's route (asked again
while the guest is busy, as the host does) and by the guest's own on one stream
each, and the host closes no stream by a reset.
`android-sync` (integration) registers when `UML_ANDROID_SYNC_RELEASE` names the
release: with SysRq off, a file the guest holds dirty reaches the host's data
image once the management console's `sync` replies, and survives a hard kill of
the emulator; a request that is refused leaves it in the guest only; a `suspend`
asked with no sync before it states in its reply that it synced and the image
holds the file (`--suspend-statement none` for a kernel before the statement).
The host's side of the settle calls is covered on any host by
`guest-console-writeback` (with `-timeout`, `-strict` for the reply grammar and
`-budget` for the one deadline), `guest-console-suspend-statement` (the reply's
statement, and that no other wording confirms),
`guest-console-suspend-partial-refusal` (a reply refused in its first part stays
refused when its end arrives late), `emulator-lifecycle` (what each call
reports, and when a saved answer is repeated), `kernarium-settle-plumbing` (the
runtime's report envelope, the handle a sync pins and the loader's forwarding)
and `runtime-session-record`.
`android-wifi-network` (integration) registers when
`UML_ANDROID_WIFI_NETWORK_RELEASE` names the release: the guest's one Wi-Fi
network stays enabled, joined and validated while the internet checks fail, and
only Android's Wi-Fi mark shows the failed check in Settings and in the status
bar, also when a sign-in or block page that the runner's server gives the probes
answers them. On one network that is never replaced or validated again, which
is validated while its first check is still unanswered (the server holds that
answer), the warning shows with no other action, follows the server from a
failing check to a passing one and back, and returns when a forced
re-evaluation checks the network; a strict private DNS name is not probed again
for each failed check, and each forced re-evaluation of a network whose strict
name fails adds its own log line, leaves the network unvalidated with its
private DNS broken and runs no check. A guest of the
release under test streams its log from the start of each boot to a host file,
confirmed carrying before a restart or suspend; crashes and ANRs of
NetworkStack, SystemUI and Settings, and illegal instructions, are read from
the whole stream and fail the run, in the section whose window holds them or in
none.
The network joins again after a suspend and after a restart, and the boot script
runs without SELinux denials; a joined network keeps its metered and
Auto-connect settings, a forgotten one stays forgotten and Wi-Fi that is off
stays off. A guest stopped when the Wi-Fi service lists the network, or when
the join is first reported, still has the network when the same data starts
again: it leases within a minute (last observation included), the script logs
that it left marker 1, and the next boot, after a restart, logs marker 2.
`UML_ANDROID_WIFI_NETWORK_PREVIOUS_RELEASE` (a `;`-separated list) adds the
upgrade sections: a data disk on which a release left the network disabled is
repaired once by the first boot of the release under test and the next boot
leaves it alone, a disk of a release that never disables the network is left
alone, a failed check shows only the warning on both, and Auto-connect turned
off on either stays off at every later boot.
`android17-wifi-network-runner` (tooling) runs its parsers, the host's server
and the sections' checks against scripted and simulated answers, without a guest;
`android17-wifi-network-script` (tooling) runs the boot script itself against a
fake Wi-Fi framework (a stop before the config store wrote the network, a join
before the write, a stop right after marker 2, an unanswered listing, a network
removed during the boot, marker 2 in every state).
`android-restart-loop` (integration) registers when
`UML_ANDROID_RESTART_LOOP_RELEASE` names the release: a guest restarted 16 s
after each kernel start, while `sys.boot_completed` is still unset, ends the
launch with status 3 after eight boots, and eight boots in a row restarted after
they finished (seven restarts, each after the window) are never limited and
power off cleanly (the loop guard, `emulator/src/restart_loop_guard.hpp`).
`android-thermal` (integration) registers when `UML_ANDROID_THERMAL_RELEASE`
names the release: the runner plays the host application over a vsock forward
and checks the thermal manager, the display manager's vote, SurfaceFlinger's
render rate and the frame rates at each severity, on a fresh data disk or, with
`--instance`, over an older release's.
`android-orientation` (integration) registers when
`UML_ANDROID_ORIENTATION_RELEASE` names the release: the runner plays the host
application over a vsock forward to the motion port with the
`androidboot.kernarium.orientation=1` argument and checks that the sensor is
listed and used by the window manager, that Android's rotation follows each
value with auto-rotate on, stays and offers the rotation suggestion with it off,
follows or keeps to what an application that asks for an orientation allows,
that the setup wizard turns to a value sent before the sensor was registered
(the first frame says the sensor is not yet in use), that the lock screen stays
upright and the newest value turns the display at unlock, that a value said
again changes nothing, that a turn made with a finger down waits for the finger,
that a value sent while the screen is off turns the display once it is on (the
sensor idle while off and in use again), and how a host that connects late or
reconnects is served. Restarting the framework or the sensors HAL needs root,
which a user build does not give, so the runner does not.
`--launch-only` stops after the launch's state (the property, the sensors the
service lists and their flags, the judge the window manager picked and what it
holds, no SELinux denial about the property), which is what to run first on a
new image. `--expect none` boots a release that does not list the sensor (or
this release without the argument) and checks that nothing changed: `ANC1`
frames, the accelerometer judging, and the display turning with accelerometer
postures. The runner needs a JDK, which builds and signs the probe
application.
`android17-orientation-runner` (tooling) runs its wire, parsers, rotation rules,
host and probe manifest without a guest.
`android-compass` and `android-compass-absent` (integration) register when
`UML_ANDROID_COMPASS_RELEASE` names the release and
`UML_ANDROID_COMPASS_PROBE_APK` the motion probe (built by
`tests/integration/android/motion/build-apk.sh`): the runner plays the host over
a vsock forward and qualifies the magnetometer and the compass feature a release
declares when the launch says the host has one, and that it declares neither
without. `android17-compass-runner` (tooling) runs its wire, oracle and parsers
without a guest.
`android-gamepad` (integration) registers when `UML_ANDROID_GAMEPAD_RELEASE`
names the release and `UML_ANDROID_GAMEPAD_PROBE_APK` the APK of
[`gamepad/build-apk.sh`](../tests/integration/android/gamepad/README.md): the
runner plays the host application over a vsock forward, creates a controller of
each driver family (generic, Xbox, DualSense), sends reports and checks what
Android sees (devices, keys, axes), what comes back (rumble, lights, the
drivers' feature requests), hot plug, the relay's recovery, and reports the
guest's cost per report and the relay's memory. `android17-gamepad-runner`
(tooling) runs its wire, controllers and parsers without a guest.
`android-camera` (integration) registers when `UML_ANDROID_CAMERA_RELEASE`
names the release: the runner boots it with the launcher's synthetic camera
(`--camera=synthetic|blocked`, `--camera-profile=`) and a layout on the vendor
SKU (`--layout auto` has the launcher compose it, as the iOS runtime does, with
RAW once `--calibration-dir` holds colour data from an earlier session), and
checks what Android lists (features, media profiles, camera IDs), one preview
frame and one still of the camera application, photo-to-video switches
(`--switch-rounds`, four by default; each from a freshly started application
with capture running and the window animations off, and each seen: the photo
activity's pause, the video activity's resume and capture after them) that
refuse no capture request and draw no SELinux denial of the camera HAL's use of
the display composer's file descriptors, three recordings of the video mode
whose audio track holds what its time stamps span and ends with its video (the
runner's bounds): a plain one; one during which the thread that serves the
Virtio sound device is held (`uml_audio_hold`, which the runner inserts into the
launcher), so that the video goes on while the audio input has nothing to read,
and the input's log must say it made the time up; and one during which the
emulator process is stopped, whose two tracks must show gaps of about the same
length with no burst of made-up audio after them. The audio capture is seen to
run before either is injected. In a blocked session the runner checks instead
that the same cameras give black frames and no still.
`android17-camera-runner` (tooling) runs its parsers and the expectations of
every layout without a guest, and `android17-camera-layouts` (tooling) the
layout set, its fallback order and the generated files.

Every first-party script that reaches a guest over adb uses the one shared adb
server, names the guest with `-s 127.0.0.1:<port>` and at the end only
disconnects its own serial; the stranger-key step of `android-adb-debugging` is
the only private server, and `repository-adb-shared-server` fails any other
script that selects a server or runs `kill-server`. Each Android runner's
default forward port is its guest slot's first port (`GUEST_SLOT_PORT_BASE`)
and a fixed port outside a slot.

## x86 instruction conformance

The x86 gadget engine's external campaign is the pinned FEX `x86-conformance`
suite (x86-64 and i386 modes); for the gadget engine FEX is a test input only,
and FEXCore is linked into nothing but the macOS-only x86-64 JIT. Materialize
each mode with `tools/fex-tests/build-asm-corpus.py` (generated data stays
outside the repository) and run it with `tools/fex-tests/run-asm-corpus.py`;
the JIT takes the same campaign through `uml_x86_64_fex_asm_jit_runner`. The
[campaign README](../tools/fex-tests/README.md) owns the exclusion policy.
Acceptance is every applicable case in both modes passing under that policy,
with no decoder or semantic failures. The macOS unit tree also holds the JIT to
`tests/unit/x86_64/jit_corpus_exclusions.tsv` and
`tier_switch_exclusions.tsv`, whose rows fail the test when they stop matching.
`ctest -N -L x86-64` lists the other x86-64 unit tests, whose headers say what
they check; the `x86-64-jit-*` tests exist only on macOS, and the
`ios-products-carry-no-fexcore` tooling test keeps FEXCore out of every iOS
product.

## Performance and external campaigns

Benchmarks, full LTP, external networking, KDE/VNC/WebGL desktop campaigns, GPU
conformance, SMP stress, official A64 semantics and connected-device runs are
opt-in; run them only when the change or question crosses that boundary.
Geekbench runs the image-local payload, or an attached payload image checked by
SHA-256, through the checked-in workload script with `--no-upload` and
`--iterations 1`. Manual performance tools are under
[`../tests/perf/README.md`](../tests/perf/README.md); record conclusions in
[`optimization-history.md`](optimization-history.md), not raw logs. GPU
capabilities and unsupported Vulkan work are in [`graphics.md`](graphics.md).

## Graphics conformance campaigns

Guest CTS qualification is opt-in, receipt-bound and separate from the
`check-gpu` workflows, which never launch a guest or make a Vulkan support
claim. Every campaign guest is JIT with four vCPUs and 4 GiB. Two rules bound
what any graphics run may claim:

- A version or capability rises only when natural capset discovery and a
  functional test agree. `MESA_GL_VERSION_OVERRIDE`,
  `MESA_GLES_VERSION_OVERRIDE`, forced Virgl capability bits and
  extension-string injection are never evidence.
- A run must exercise the real Virtio-GPU/Virgl path; an app-owned
  framebuffer, a Chromium-specific preload or any other substitute for the
  guest's driver stack invalidates the result for capability purposes.

`tests/integration/gpu/run_vk_cts.py` runs the Vulkan lane,
`run_vk_gl_cts.py` the OpenGL and OpenGL ES lanes, and
`run_graphics_cts_campaign.py` coordinates both; each `--help` describes its
modes, and the campaign runner's `--verify-only` reparses evidence without a
guest. A run is bound to its inputs: the Vulkan lane to
`tests/integration/gpu/vulkan_cts_receipt.json` (sealed image, CTS and Mesa
locks, and the staged KosmicKrisp provider digest, which must match the
verified prefix), the OpenGL matrix to the combined image's receipt and an EGL
configuration inventory from the same emulator, image and provider.
Window-system cases render on real surfaces (native Xorg with Virgl-backed
glamor, the image's KWin nested on Xorg for Wayland); Xwayland and Xvfb are
rejected.

A shard attempt passes only when its guest ran exactly the enumerated
selection once each in released order, delivered its whole QPA, stopped with a
clean Virtio-GPU takedown and reported only statuses the receipt's oracle
accepts. Accepted `NotSupported`, warning and `Waiver` results stay distinct
from `Pass` and prove nothing about an optional feature. A campaign is complete
when every shard has one passing attempt; a complete campaign is not an
all-`Pass`, all-features or Khronos conformance claim. Budget campaign
concurrency from host physical footprint and the `IOAccelerator (graphics)` row
of `vmmap -summary`, not process RSS.

### OpenGL ES3 FBO state-query waiver

`tests/integration/gpu/vk_gl_cts_waivers.json` holds the granted waivers; a new
waiver needs the project owner's grant and an entry there. The one entry covers
the fifteen `dEQP-GL45-ES3.functional.state_query` cases that assert
default-framebuffer state in an `es3-main` run that draws into a
framework-owned framebuffer object, where a conforming driver reports the bound
object. It applies only to a matrix run whose API, configuration name and draw
surface match its scope. The runner installs the waiver file in the guest and
verification accepts `Waiver` only for the listed cases of such a run whose QPA
session header names the granted list; a listed case that still fails, a
`Waiver` anywhere else or a missing header is rejected. `run.json` and the
campaign aggregate count every waived result and set `unwaived_runner_success`
to false. A waived result is not a pass and makes no conformance claim.

### Framebuffer completeness compatibility warning

`KHR-GL46.direct_state_access.framebuffers_check_status` reports
`CompatibilityWarning` when an optional observation sees
`GL_FRAMEBUFFER_COMPLETE`; the runner counts that status as a success and
expects two such observations
([why](graphics.md#guest-capability-boundary)).

## Claiming completion

A passing structural audit does not prove generated-code shape, performance,
kernel boot or guest behavior, and a unit test does not prove an external
campaign. Report only checks actually run and name any input or campaign that
was unavailable. Do not normalize compiler metadata or require byte-identical
independent builds. For a measured A/B, record the revision and dirty state,
configuration, command, result and exit status.
