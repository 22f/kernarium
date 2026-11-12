# Andarium

Andarium is the iOS application that runs one Android (LineageOS 24, Android
17) guest on the Darwin runtime. Android ships inside the app; what the user
creates, sizes and erases is the guest. [Kernarium](../Kernarium/README.md)
stays the developer application for Linux guests and for Android bundles.
Andarium also runs on an Apple silicon Mac in Designed for iPad mode, and it
builds for the iOS Simulator, where the guest renders on the CPU for UI tests
([`graphics.md`](../docs/graphics.md#ios-simulator-software-renderer)).

## Ownership

```text
Andarium/Shared/      screens and model both targets build
Andarium/Preview/     mock screens, data and launch arguments (Simulator)
Andarium/Live/        the runtime, storage and session of the device app
AndariumCore/         Swift package: the model and the adb client, no UIKit; it
                      re-exports GuestHostCore (../GuestHost/Core)
AndariumTests/        hosted tests of the device app
AndariumUITests/      UI tests of the device app in the Simulator
Config/               entitlements, Info.plist, privacy manifest, bridging header,
                      build settings (Andarium.xcconfig)
scripts/              system staging, the App Store bundle check,
                      acknowledgements, screen capture
Andarium.xcodeproj/   JSON project (project.xcproj) and its shared schemes
../GuestHost/Sources  host code shared with Kernarium: the runtime loader, the
                      display and touch view, zstd, the shader caches
```

The project file is the JSON format Xcode 27 reads, kept in the canonical form
`xcrun xcprojformatter --update Andarium/Andarium.xcodeproj` produces: format
after every hand edit. Every configuration takes its base settings from
`Config/Andarium.xcconfig`, which includes
`../GuestHost/Config/Signing.xcconfig` and sets what decides
[Send feedback](#send-feedback); the signer's team is only in the ignored
`Signing.local.xcconfig` beside the signing file
([`GuestHost/README.md`](../GuestHost/README.md#signing)), so the project names
no team.

## Targets

`Andarium` is the device application (iPhone and iPad, and an Apple silicon
Mac in Designed for iPad mode). It embeds the Android runtime frameworks and
carries the system image. It has no background mode: the guest runs while
Andarium is in the foreground and is paused when it leaves. It runs one scene
and one guest per process, and needs the increased memory limit and extended
virtual addressing entitlements (`Config/Andarium.entitlements`). The model
and the runtime belong to the process (`AndariumProcess`), so a scene that
connects again shows the same ones.

`AndariumUITests` is the UI-test target of `Andarium`, in the scheme's test
action. It is never part of the application: an App Store build contains no
test bundle ([configurations](#configurations)).

`AndariumPreview` is a Simulator-only target built from `Shared` and `Preview`
with mock data. It embeds no runtime and boots nothing. Its launch arguments
(`-screen <name>` and the others in `Preview/AppEntry.swift`) open any screen
or state directly, and `scripts/capture-screens.py` (`--help`) captures them:

```sh
xcodebuild -project Andarium/Andarium.xcodeproj -scheme AndariumPreview \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' build
```

## Devices

The guest is drawn with Metal 4, which the A14 and later chips have. No
capability key names the chip, so `UIRequiredDeviceCapabilities` names `metal`
and the application asks the GPU for the Metal 4 family at launch
(`HostDevice.runsGuestGraphics`), refusing to start a guest without it; the
Simulator draws on the CPU and always passes. iPhone and iPad are supported,
and so is an Apple silicon Mac in Designed for iPad mode: TestFlight and the
App Store offer the application to Macs, and every configuration sets
`SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD` to YES, which gives Xcode its Mac
destination and adds nothing to the built application.
`ITSAppUsesNonExemptEncryption` is NO in `Config/Info.plist`.

The iPhone interface is portrait-only and the guest owns its rotation:
Android turns its own display and the sensor axes stay fixed. A guest whose
image has a device-orientation sensor (the boot request's `orientation` host
feature, [Kernarium](../Kernarium/README.md#automation-runs)) follows the
device's orientation as iOS reports it, which already includes the person's
Portrait Orientation Lock: while a session is in front the application
observes `UIDevice` (`DeviceOrientationObserver`) and sends the orientation
over the motion connection (`MotionOrientation.swift`), which Android treats as
a rotation proposal that its own policy still decides on. The Gyroscope switch
governs only the accelerometer and gyroscope data apps may read, not this. A
guest whose image has no such sensor judges the accelerometer as before. On iPad
the interface turns with the device in a session as it does at Home, full screen
or in a window, and the host drives the guest: it is held at display rotation
zero, its display follows the scene, and the sensor axes follow the scene's
interface orientation (`MotionMount`, `SessionOrientationPolicy`). An
application that asks for one fixed orientation is letterboxed in the other
one, as in a window. The guest's own rotation preferences are restored when
the host stops driving it. The display keeps the last frame's proportions
while it turns, until Android draws the new layout.

A folding iPhone (iPhone Duo) has two displays. Its cover is an iPhone's, and
its inner display turns with the device as an iPad's does, the host driving the
guest. The layout follows the display the system lights, which the scene's root
reads from the hinge and the view's reserved regions (`FoldSceneReader`) and
`AndariumCore/FoldLayout.swift` decides as pure functions; a device that
reports no hinge keeps every layout it has. Only the inner display bent at the
fold is placed around the hinge (`FoldPose.spreads`): flat, upright or turned,
every screen is laid out as on an iPad of the same size and shape.

- **Cover.** The guest keeps an iPhone's shape beside the strip the camera
  keeps, which holds the side keys as an always-open column and the statistics.
- **Inner display, upright.** The guest is one display across both halves, so
  moving the hinge never resizes Android, and the side keys stay on its edge.
  Bent, the statistics and notices hang from the trailing half and Andarium's
  own pieces never sit on the fold; flat, they hang from the top of the display.
- **Inner display, turned.** Flat, the guest fills the display, with the keys
  on its edge and the statistics and notices at its top. Bent, the device
  stands like a laptop: the guest is the upper half and the lower half is a
  deck with the keys in one row and the statistics and notices above them.
- **Other screens.** Bent, Home is one page each side of the fold; flat it is
  Home as on an iPad, one scrolling list with Start at the foot of the screen.
  Flat, Welcome, the boot, the busy, low-space and end cards, and the cards
  inside a sheet (the system's) are laid out as on a display with no hinge,
  centred in the safe area as the Start bar and the system's alerts are. While
  the device is bent they keep to the half where the system puts its alerts,
  and the boot of a standing device to the upper half. While bent, Welcome
  and the end screen scroll when taller than their half. System alerts stay
  where iOS puts them, inside the device's own safe area. A sheet turned and
  bent rises to a little below the fold; a sheet keeps the placement it opened
  with, and what has been typed in it, until it closes, whatever the hinge or
  the turn does meanwhile.
  On the cover the file browser's Import is a round button in the toolbar's
  column beside the camera, between More and Done, and the bar keeps Export.

The guest is resized only where its shape changes (the cover and the inner
display, standing and the other turned poses), through the display's ordinary
resize. Telling Android where the crease is is not done.

While the guest's display is shown (the boot and the run) everything behind it
is black, in the light appearance too: the scene's root view and its window
carry it, so the area outside the safe area stays black through a rotation or
a window resize. Home and sheets follow the system appearance
(`AndariumHostingController`).

## Build

Packaging the Android runtime alone, and staging the system from the accepted
release, need no Linux runtime build:

```sh
cmake --preset kernarium-ios-device-android
cmake --build --preset kernarium-ios-device-android
Kernarium/scripts/package-ios-frameworks.sh --runtimes android
Andarium/scripts/stage-system-image.py
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'generic/platform=iOS' build
```

The frameworks go to `.build/kernarium-xcframeworks`, which Kernarium shares,
so package the runtime a build needs just before it. The staging script checks
the accepted release against its manifest and layout and stages the system
disk, partition layout, kernel command line and manifest into
`.build/andarium-system`, which the application bundles as it stands.

The `AndariumTests` bundle runs hosted in the application on an iPad, in the
iOS Simulator or, as an iPad app, on an Apple silicon Mac (whose derived data
must be outside the temporary directories,
[`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium)). For the Simulator,
package its slices first (below), then:

```sh
xcodebuild test -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -destination 'platform=iOS Simulator,name=iPad Pro 11-inch (M5)' \
    -only-testing:AndariumTests
```

The `Andarium` scheme also builds for the Simulator with the software
renderer: build the `kernarium-ios-simulator-android` runtime preset, package
it with `package-ios-frameworks.sh --sdk simulator --runtimes android` and
build for a concrete Simulator
([`docs/testing.md`](../docs/testing.md#apple-builds-and-kernarium) has the
destination rules).

### Configurations

The project has Debug, Release and AppStore. Release is the development and
harness build: it reads the automation environment and prints the console
lines the iPad harness reads. AppStore builds as Release does and leaves out
everything that exists for automation or only pretends:

- `APP_STORE` is a Swift compilation condition that compiles out the launch
  environment (`Live/LaunchEnvironment.swift`, so no `ANDARIUM_` or
  `KERNARIUM_` name is in the application or in `AndariumCore`, nor the thermal
  state a launch can state in place of iOS's), the harness's
  console lines (`Shared/ConsoleReport.swift`), the launch's deferred work
  (create, erase, start) and the isolated guest instance.
- It defines `UML_APP_STORE_APPLICATION` for the loader it compiles, which
  leaves out the display mirror, console input, the automation lookups and the
  JIT's preparation entries ([`GuestHost/README.md`](../GuestHost/README.md)).
  The JIT's policy and words (`Shared/Jit.swift`), debugger side
  (`Live/JitDebugger.swift`) and Home row are under `!APP_STORE`, in the
  application and not in AndariumCore, which the package builds without that
  condition.
- It embeds the App Store runtime, which reads no environment knob and carries
  no debugging or automation support (the `kernarium-ios-device-android-app-store`
  preset, packaged with `package-ios-frameworks.sh --app-store --runtimes
  android`; [`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
- Its Archive action uses it, and the archive is the build TestFlight and the
  App Store offer to Macs.
- It refuses an embedded artifact that is behind the archived commit: a build
  phase runs [`scripts/check-embedded-artifacts.py`](../scripts/check-embedded-artifacts.py),
  which compares the accepted Android release and kernel descriptors with the
  commit's inputs, and the staged and packaged system and runtime with the
  descriptors, and fails naming the changed paths and commits. It also refuses
  a packaged kernel whose Linux banner names a build user or host other than
  the neutral `kernarium@kernarium`. A release that records no input trees, a
  dirty input, or a staged or packaged artifact that is not the accepted one
  stops the build; only the user decides to ship pending changes, by naming the
  release ID in `PENDING_INPUTS_DECISION`.
- Its last build phase runs `scripts/check-app-store-bundle.py`, which fails the
  build on a missing Info.plist key, an environment knob, a symbol or log text
  of the debugging and automation support, the JIT (its symbols and text and
  the `brk #0xf00d` instruction), a test bundle or XCTest framework, a
  trace of the build host, a harness console line, or a privacy manifest that
  does not cover a binary
  ([`scripts/check-privacy-manifests.py`](../scripts/check-privacy-manifests.py));
  the script's header is the complete list.
- It alone can carry [Send feedback](#send-feedback), and only when the
  receiver's address is in the ignored `Config/Feedback.local.xcconfig`, and
  [Follow developer on X](#follow-developer-on-x), and only when the handle is
  in the ignored `Config/Developer.local.xcconfig` and the logo in the ignored
  `Config/Local/`.

```sh
cmake --preset kernarium-ios-device-android-app-store
cmake --build --preset kernarium-ios-device-android-app-store
Kernarium/scripts/package-ios-frameworks.sh --app-store --runtimes android
Andarium/scripts/stage-system-image.py
xcodebuild -project Andarium/Andarium.xcodeproj -scheme Andarium \
    -configuration AppStore -destination 'generic/platform=iOS' build
```

The privacy manifests of the application (`Config/PrivacyInfo.xcprivacy`) and
of the frameworks (`Kernarium/PrivacyManifests`) declare exactly what the App
Store build and the runtimes use. A release or harness build also reads the
system uptime for its thermal report, which the application's manifest does
not declare because only an App Store build is uploaded.

### Send feedback

Home's toolbar menu and a running session's Settings open a sheet that writes
an email to the developer (`Shared/Feedback/`, the system's Mail composer sends
it). Home's toolbar holds one menu in place of the Info button, with Info and
Send feedback; it keeps Info's glyph and accessibility label and is on Welcome
too, where Send feedback is reachable before a virtual machine is set up. A
build without Send feedback keeps the plain Info button and holds no menu code
(`Shared/HomeMenu.swift`). Info itself has no Send feedback section. The
feature is compiled in, under the `FEEDBACK_MAIL` condition, only in an
AppStore build whose ignored `Config/Feedback.local.xcconfig` (from
`Feedback.local.xcconfig.example`) names the receiver, so a fork, a sideload or
a Debug or Release build holds none of it, and every configuration's bundle
check says whether it does: it also fails on the local receiver's bytes in any
file of a bundle that does not name it (never printing it), and on an
`Info.plist` whose values differ from `Config/Info.plist` (the preprocessing
changes the feedback key alone). Tests and captures turn it on in a Debug build
with `FEEDBACK_ADDRESS=feedback@example.com` on the `xcodebuild` line, which
the bundle check accepts in Debug alone; `AndariumPreview` takes that
arrangement only, never the local receiver, and its hooks into the sheet are
not in the AppStore product. Its logic (`Shared/Feedback/Logic`) is also built,
and tested, by AndariumCore's tests through a symlinked target no product
links. The feature's words are looked up in the `Feedback` string table
(zh-Hans, zh-Hant and ja are in [Languages](#languages)); the mail's subject
prefix and its information block are English whatever the interface language,
and the bug report's overlay text is the app's own.

### Follow developer on X

Info has one passive outward link more: Follow developer on X, a row in the
section of Project on GitHub, which opens the developer's profile through the
system (`openURL`: the X app when it is installed, the browser otherwise;
`Shared/FollowX/`). It has no badge, prompt or counter, and nothing is
remembered about a tap. The name X in its text is the service's logo, drawn
inline at the text's size and colour as a template image, and the same logo is
the row's leading icon; VoiceOver reads "Follow developer on X" with the link
trait. The profile and the logo belong to the developer and are tracked
nowhere: the row is compiled in, under the `FOLLOW_X` condition, only in an
AppStore build whose ignored `Config/Developer.local.xcconfig` (from
`Developer.local.xcconfig.example`) names the handle (`DEVELOPER_X_HANDLE`: a
handle, never an address, as `//` starts an xcconfig comment; the code builds
the address) and whose ignored `Config/Local/` directory holds `x-logo.svg`, a
single-colour vector that `XLogoLocal.xcassets` links to. A build with the
handle and without the logo fails its first build phase. A fork, a sideload
or a Debug or Release build holds none of it (no row, handle, logo or
`FollowX` strings table), and the bundle check asserts both directions as it
does for Send feedback: it also fails on the local handle as a profile
address, or as a whole string of a property list, in any file of a bundle that
does not name it (never printing it), and on the arranged handle in any
product that does not arrange it. Tests and captures turn the row on in a
Debug build with `FOLLOW_X_HANDLE=example_handle` on the `xcodebuild` line,
with a glyph drawn for them (`XLogoGlyph.xcassets`) in place of the logo;
adding `FOLLOW_X_LOGO_SOURCE=local` takes the logo from `Config/Local/`.
`AndariumPreview` takes the arrangement only, never the local handle. The
handle's shape and the address are in `Shared/FollowX/Logic`, which
AndariumCore's tests build through a symlinked target no product links, and
the words are in the `FollowX` string table.

## Model and storage

`AndariumCore` holds everything the screens decide with, so it is tested on
the host (`andarium-core`, [`testing.md`](../docs/testing.md#andarium)): limits,
the memory policy, the guest and its life (`Lifecycle`), storage, boot
milestones, the adb client, motion and statistics. The application adds only
what a screen holds. A screen shows a control only when the capability behind
it exists (`Capabilities`); a build whose runtime cannot do a thing leaves its
control out.

- **Memory.** A guest has at most the shared
  [vCPU limit](../docs/architecture.md#memory-and-executable-code) and the RAM
  the runtime loader answers for that count
  (`kernarium_runtime_loader_maximum_guest_memory_mib`); a new guest gets the
  loader's default for it, which stops at 4 GiB however much is offered
  (`kernarium_runtime_loader_default_guest_memory_mib`). Android is usable
  from 1.5 GiB, so a guest has at least that (`Limits.minimumMemoryMiB`,
  Andarium's own: the runtime still accepts less for Kernarium's guests); a
  stored smaller size is raised to it on load, and a device whose bound is
  lower offers the bound alone. The runtime starts every size the picker
  offers: a start whose memory the loader refuses
  (`kernarium_runtime_loader_guest_memory_admitted`) ends before the run with
  that reason. `MemoryPolicy` holds those bounds, defaults
  and the estimates and no formula of its own. A guest with more than the
  runtime's conservative size for its vCPU count (`safeMemoryMiB`) is likely
  to be closed by iOS: Home shows a notice and the memory ruler a line and red
  ticks, both following the guest's memory and vCPU count (`MemoryWarning`).
  A guest above the graphics line (`MemoryPolicy.graphicsLineMiB`) may run out
  of graphics memory (VRAM, the host GPU's budget): the System sheet turns the
  ticks past it orange and warns that games may (`GraphicsMemoryWarning`), and
  Home says nothing. The line is the default where the runtime lowered the
  default to keep the GPU 1 GiB
  (`kernarium_runtime_loader_default_guest_memory_holds_gpu_budget`), and
  elsewhere the largest size whose estimate of VRAM is still 1 GiB or more
  (`Limits.minimumDefaultGraphicsMemoryMiB`), so a range above 4 GiB warns
  only where the estimate falls. The sheet also shows the loader's estimate of
  VRAM for the chosen memory and vCPUs and says it is separate from the
  guest's RAM.
  The memory ruler follows the range, which on a Mac with much RAM has about
  a thousand positions: it labels only the round numbers of gibibytes that do
  not touch and draws the ticks that have room (`RulerThinning`), and
  VoiceOver's increment and decrement then move to the next labelled one.
- **Storage.** A guest's files live in `Library/Guest/` of the application's
  container behind `GuestStorage`. The system disk is Android's, read-only, and
  stays in the release directory; the runtime creates the data disk `data.img`
  at the first start, sized by the guest's capacity, and only grows it after;
  Home disables Start in recovery until then (`StartPlan.offersRecovery`).
  A larger size chosen in Resize waits on Home and applies as the next start,
  of any kind, is handed to the runtime (`Lifecycle.start`), which grows the
  disk then; the guest records the new capacity at that point, so a start that
  ends early leaves no growth pending beside a disk that already has it.
  The runtime passes both paths to Linux in a kernel argument, so they stay at
  paths free of spaces and the other characters `DiskPath` refuses (which is
  why the guest is not in `Application Support`).
- **Updates.** `InstanceRules.decide` applies the rules of the
  [Android README](../android/lineage-24/README.md#guest-instance-storage) and
  `StartPlan` is what Home shows of them before Start. An update clones
  `data.img` first (the update's copy, until the updated Android has booted),
  and Erase virtual machine deletes everything the guest has stored, its snapshots and
  the adb key included.
- **Backup.** The guest is left out of iCloud and computer backups unless the
  user turns on Back up guest data. Its snapshots are never backed up.
- **Snapshots.** A snapshot is an APFS clone of `data.img` with a copy of
  `instance.json`, in `Library/Guest/Snapshots/` (`SnapshotStore`): it costs
  nothing when made and then the blocks the guest rewrites. It keeps the data
  and the record of its Android, not the guest's choices, the system disk or
  the adb key. Create, Restore and Delete run only while the guest is stopped
  (Home, with no run active, pending or launching, since iOS takes no file lock
  on the disk), and Create only after Android's own shut down and with no
  update unfinished and on a volume that clones. Restore replaces the data and
  keeps nothing of the state it replaces; a snapshot of an older Android then
  updates at the next start, and one of a newer Android or an older layout is
  refused. Rename runs at any time.
  Each operation has one commit point (the snapshot's rename into place, the
  set-aside rename of a delete, the rename of a restore's clone over
  `data.img`): an error before it changes nothing, and one after it does not
  fail the operation. A restore that took effect and did not finish completes
  at the next start or launch (`TwoDiskStorage.recoverSnapshots` runs before
  the guest is read); until then Create and Restore are refused and every
  snapshot but that one can be deleted, and the start plan is the one that
  start finds, from the snapshot's own record. The update's copy is not a
  snapshot.
  The store's file calls are injected (`SnapshotFileOperations`).
  `SnapshotDriver` is what the screens call, and a refusal is a
  `SnapshotRefusal`. The driver reports what it does as `SnapshotActivity`,
  which the model keeps (`AppModel.snapshotGate`, observable); no start begins
  and the update's Restore waits while an operation runs; a rename waits for
  nothing and is waited for. A listing is never older than the last finished
  operation (`revision`), can come without its figures, and names the release
  the data is on; a cancelled one stops measuring.
  Home's Virtual machine section has a Snapshots row (how many, the space they
  use, Manage) that opens the Snapshots sheet: the list newest first, a page
  per snapshot (Restore, its name, Delete) and a page on the space they use,
  with Take snapshot at the foot as Start is on Home. `SnapshotsModel` holds
  what Home and the sheet show (names first, then the figures; they give way
  to the measuring state when an operation other than a rename finishes or a
  run begins or ends, and are measured when nothing was read, after an
  operation and when a run ends, never because a view appeared or while the
  guest starts) and runs
  each operation;
  `SnapshotWords` says every refusal, plan and figure in one place. A restore
  closes the sheet and Home says what happened, Start being next. The preview
  plays the same screens on mock snapshots (`-screen snapshots-…`).
- **Release notes.** `Shared/WhatsNew.json` bundles the notes per build,
  from build 9 (build 5, the first public release, and builds 6 to 8 have
  none).
  The first launch of an update that carries notes shows a ribbon above Start,
  on at most three launches, that opens them; a build shows the notes up to its
  own number and no later ones. Info keeps every release's notes
  (`WhatsNewRules`, `WhatsNewStore`). A launch with a launch setting, or one a
  test runs, shows no ribbon and keeps no record.

## Runtime

The device application drives the Android runtime through the same loader as
Kernarium. A run starts when the guest's display has its size and the disk is
ready, on a thread of its own. Android's clean power-off releases the kernel's
resources and guest RAM and returns to Home, which states the storage the data
disk then holds and where Start boots again in the same process; an unclean
shutdown, a host stop or a cleanup that fails keeps the failure screen and
requires reopening Andarium.

- **Time zone.** Every start gives the guest the host's zone
  (`GuestTimeZone`, the boot request's `android_time_zone`); the system image
  applies it unless the guest's user chose a zone
  ([Android README](../android/lineage-24/README.md#time-zone)).
- **Haptics.** On a device with a Taptic Engine the boot request states the
  haptics bit (`android_host_features`), which gives the guest a vibrator, and
  the application plays the vibrator's scores on Core Haptics. It connects to
  the guest's HAL over vsock 5557 (`HapticsSession`), plays only while it is in
  front and holds the guest running, and lets everything go the moment it is
  not. The HAL sends the effect or primitive and its duration, and
  `HapticShapes` decides what fills it, so the feel is tuned in an app update.
  Android's own vibration settings decide what the guest asks for. Haptics
  under Capabilities, a per-guest switch that is on until the person turns it
  off, decides whether this device plays it: off, the application holds no
  connection and no engine for the Taptic Engine and the guest keeps its
  vibrator. A game controller's rumble is not governed by it. An iPad, a Mac or
  the Simulator, which have no Taptic Engine, give the guest no vibrator and
  Capabilities no Haptics row. The wire is `HapticsProtocol`, whose golden frames
  the HAL's test shares (`tests/unit/andarium/fixtures`); the
  [Android README](../android/lineage-24/README.md) owns the HAL.
- **Heat.** The application tells the guest's thermal HAL how hot the device is
  over vsock 5559 (`ThermalSession`; the wire is `ThermalProtocol`, whose
  golden frames the HAL's test shares in `tests/unit/andarium/fixtures`). The
  severity is the hottest iOS thermal state of the last 120 s: it rises at once
  and falls 120 s after the hotter state ended. Fair is Android's light,
  serious its severe and critical its critical, and the host never sends a
  level that shuts the guest down. At severe and above Android lowers the
  guest's frame rate to 30, and the first time in a run that this starts the
  session screen says so once. There is no switch. A system without the HAL
  hears nothing and shows no notice; the
  [Android README](../android/lineage-24/README.md) owns the HAL.
- **Game controllers.** Each game controller iOS has connected is a HID
  device in the guest, made through the guest's relay over vsock 5558
  (`GamepadSession`; the wire is `GamepadProtocol`, whose golden frames the
  relay's test shares in `tests/unit/andarium/fixtures`). A DualSense,
  DualSense Edge, DualShock 4 or Xbox Wireless Controller presents as that
  device, bound by the guest's own driver, and any other as a plain HID
  gamepad that lists only what the controller has. Controllers are looked for
  during a normal run, and nothing is scanned for or paired. Their input
  reaches the guest only while Andarium is in front, and leaving the front
  lets every button go. Home, Menu, Options and Share are taken from iOS's
  gestures while a controller is held and reach Android as buttons (iOS's own
  Home setting decides whether Andarium may have Home). A button going down
  wakes a guest that suspended itself (`wake_for_input`, runtime API 34).
  Rumble, the light and the player number follow the guest's drivers onto
  the controller, and `Config/Info.plist` declares the extended gamepad
  profile. There is no switch. An image without the relay is given up on
  quietly after boot; the [Android README](../android/lineage-24/README.md)
  owns the relay.
- **Location.** Location is a per-guest switch under Capabilities on Home,
  off for a new guest and for one stored before. iOS is asked, for
  when-in-use access, only when the person turns it on; Andarium has no
  background or Always access. The position is read only while the switch is
  on, iOS allows it, Andarium is in front, a normal run goes on and the
  guest's GNSS receiver is powered, and goes to the
  [GNSS device](../darwin/device/gnss/README.md) through the runtime. A change
  of access or of Precise Location drops the held position first. An
  approximate position is passed as approximate; the guest owns what apps see
  of it. The row is dimmed and off when iOS refuses, the device restricts
  location or Location Services are off, and says Approximate location, with an
  explanation and Settings, when Precise Location is off. No test reads a real
  location.
- **Input.** The shared [GuestHost input](../GuestHost/README.md) maps up to
  ten fingers onto the guest's display; the hardware keyboard and mouse are
  the next item. A touch the system takes over reaches
  Android as a cancelled touch, not a lift, so no tap follows. Only the side
  keys take touches: while they are open, a touch that begins anywhere else
  reaches Android from its first event and closes the keys
  (`SessionState.closeSideKeys`, called by the display's `touchDidBegin`).
  Beside the closed keys their tip, while it shows, takes touches on its card
  alone (on a Mac, where an iPad app's popover keeps its dimming view, every
  touch), and it goes with the closed keys when they open or the session ends.
- **Keyboard and mouse.** The guest has a hardware keyboard and a mouse
  (virtio-input devices, plugged and unplugged at run time) while iOS has the
  hardware. Every key goes to Android as its position (the usage on the HID
  Keyboard/Keypad page), Android owns the layout and the repeat, and Andarium
  keeps no key command or reserved combination: what iOS keeps for itself
  (Command-Tab, Command-H, the Globe key) never reaches the app. The display
  holds the keyboard focus, which shows no software keyboard, and gives it up to
  a sheet, an alert or the bug report's share sheet while one is open, so
  Andarium's own text fields take the keys then and a sheet without a field
  takes them from the guest all the same. The keyboard is plugged while iOS
  reports one and by the first key that goes down, and unplugged 3 s after the
  last report or key and once no key is held (`HardwareInputPolicy`). The system
  pointer is the guest's mouse on an iPad: Android draws its cursor and the
  system pointer is hidden over the display. The mouse is plugged once iOS
  reports one and the person has moved it over the display, clicked or scrolled,
  so a mouse the Simulator reports without one being there plugs nothing; the
  pointer's position uses the touchscreen's range and its buttons and scroll are
  the mouse's, once the guest has registered the pointer (runtime API 35 reports
  the registration of the keyboard and of the pointer and publishes both off
  when a boot ends); until then, and for good on a kernel that never registers
  it, the system pointer's clicks are touches and its use only plugs the mouse.
  The devices need a kernel that follows configuration changes (the hot-plug
  topic of the virtio-input devices); on the accepted kernel they plug and
  nothing sees them, and input sent is dropped. A touch of the pointer is the
  mouse's or a finger's from its beginning to its end. An iPhone has no pointer
  to give the guest, and Settings' "Use the pointer as touch" (off for a fresh
  install, kept as `PointerPreference`, absent on an iPhone) leaves the guest
  without a mouse so that the pointer's clicks are touches. An Android
  application that captures the pointer lights an LED on its mouse; the
  application then asks the scene for the pointer lock and sends the mouse's raw
  motion, buttons and scroll from Game Controller, and where iOS refuses the
  lock (a window that is not full screen) the raw motion goes on while the
  pointer is over the display (`PointerCapture`); the capture waits while a
  sheet or an alert is up and ends with the guest's boot. Leaving the front, the
  end of a run, a sheet, a keyboard that goes and the window losing key release
  every key and button and end the pointer's contact; the devices exist only
  while a run goes on, and plug once the guest's console has answered
  (`KeyboardMouse`). Caps Lock is Android's: the application taps it once before
  a key when iOS's state differs from the light Android reports
  (`CapsLockReconciler`). Android hides its on-screen keyboard while it has a
  hardware keyboard.
- **Resolution.** Home's Guest system section has a Resolution row: the pixels
  the guest's display will have, and the end's name where the choice is an
  end (Native, Fastest), with Change opening the list of every choice with its
  pixels and density. The choices (`ResolutionChoices`) are 1.0, every 0.5 step
  that is at least a quarter below the screen's native scale, then native. The
  guest's pixels are the display's points times the choice, rounded down, and
  its density is 160 dpi a point of choice (`GuestDisplayLayout`), so
  Android's interface keeps its size at every choice. The row states what the
  next start gives: the display is the window less the session's safe area
  (`SessionDisplayArea`, read by `DisplayAreaProbe`), and follows a window as it
  resizes. A choice applies at the next start. Native is stored as the symbol
  and follows the screen, and a stored number that is no choice of this screen
  goes to the nearest one. A person who never chose gets native where the
  system lets the application use more than 4 GiB of memory, else the choice
  nearest 1.5 (`DisplayOptions`). Native's host allocations count against the
  same iOS process limit as guest RAM; neither the graphics-memory estimate
  nor the memory default counts the display's pixels.
- **JIT.** A development build on a device has a JIT row on Home, off until
  the person turns it on and kept for the install (`JitPreference`). An iOS
  device gives the JIT its executable memory only through a debugger attached to
  the process ([`docs/architecture.md`](../docs/architecture.md#executable-code)):
  the person assigns StikDebug's universal script to Andarium once in StikDebug
  and opens Andarium from it. StikDebug spins until its request is served and
  the system ends it within a minute, so Andarium asks as it opens
  (`AppModel.prepareJitAtLaunch`), on a worker, and Home does not wait for it.
  With the row on it prepares the memory for the guest, or for the guest Create
  offers when there is none yet, and the debugger leaves within seconds; with
  the row off it tells the debugger to leave at once, and turning the row on
  later in that process says to open Andarium from StikDebug. A start uses the
  memory prepared at launch (or waits for the attempt, with "Preparing JIT" on
  the boot screen) and boots on the JIT engine; without a debugger, or if the
  preparation fails, it boots on gadgets and says why on the boot screen and
  under the row. A guest changed after the memory was prepared needs Andarium
  opened from StikDebug again, and the memory stays wired for the process
  whether the start uses it or not (the runtime's GPU plan counts it). No
  debugger request is made while none is attached, and one that goes unanswered
  because the debugger vanished is a start on gadgets, never a crash. Starting a
  Debug build from Xcode attaches lldb, which would stop at the request, so
  Debug serves the request only when the launch says a servicing debugger is
  attached (`ANDARIUM_JIT_DEBUGGER=1`). `ANDARIUM_JIT` (`1` or `0`) states the
  row for a harness launch, which keeps nothing; a launch with another setting
  starts it off. The JIT compiles every process's code again for itself
  ([cold code](../darwin/cpu/arm64/jit/README.md#cold-code)), so an Android
  guest's boot, application launches and background work run several times
  slower than on gadgets, and a long single-process computation runs faster. An
  App Store build carries none of this: its runtime has no JIT and the
  application has no row, preference, entries or sentences.
- **Motion.** Gyroscope needs no permission. The host reads a sensor only while
  a guest application uses it and sends samples over private vsock port 5556
  from the process's one Core Motion manager, which is never released (a
  released manager crashes inside Core Motion); the system image must include
  the [motion sub-HAL](../android/lineage-24/README.md#motion-sensors).
- **Free space.** Free space is the space a write can use (`HostStorage`,
  `statfs(2)`), the one figure the watch, the start and transfer checks and
  every screen use; it leaves out the purgeable space iOS Settings counts, so
  Settings may show more. Once Android has booted a normal start, the
  application reads it (`RunSpace.nextCheck`: every 2 s near the limit, every
  30 s on a roomy device) and pauses the guest below 1 GiB
  (`RunSpace.pauseBelowBytes`), before the data disk can fail to grow. A full
  volume holds the guest's disk I/O until it has room instead of
  failing it ([kernel contract](../kernel/common/README.md#block-devices-and-filesystems)),
  and the pause stays the guard. The pause is the runtime's
  suspension with the storage written back first and the RAM kept in the
  process: no image of it goes to the device that is short of space
  ([lifecycle](../docs/architecture.md#guest-lifecycle)). The suspension is
  confirmed by the guest's processor time standing still (`SuspensionCheck`);
  a guest that does not hold still is woken and suspended once more, and one
  that still runs is left running behind a screen that says so and offers
  Shut down and Check again, never Resume. A paused guest is asked nothing and
  no lifecycle edge wakes it, the background included: the application stays
  resident rather than hibernate. Settings closes and an adb operation and ADB
  over network end. A screen over the session offers Resume, which reads the
  space again and wakes the guest at 1.5 GiB (a start's reserve,
  `RunSpace.resumeAtBytes`) or more, and Shut down, which wakes the guest and
  has Android power off. The thresholds and the pause itself are
  [GuestHostCore](../GuestHost/README.md), shared with Kernarium; the reading
  comes from `AppEntry.makeModel(freeSpace:)`, which a test replaces.
- **Statistics.** The session's capsule and Settings' Stats rows show the
  runtime's readings ([`Kernarium/README.md`](../Kernarium/README.md#statistics)),
  read only while a view shows them, the application is active and the guest
  runs (`StatsSampling.isWanted`). Memory is the guest's in use against its
  size, and VRAM the host GPU's usage against the guest's budget
  ([`docs/graphics.md`](../docs/graphics.md#device-memory-budget)), a dash
  without one; each has a bar of its share, orange from 90%. The readings go
  on every second, but one reaches the views only when it draws something
  other than they show (`StatsPublishGate`, `Readout.Drawn`: the figures as
  written, the bars to a thousandth and the sparkline's slots), so a steady
  guest redraws nothing; the harness's console report prints every reading.
  Settings' Show on screen is kept for the install and restored at launch,
  off on a fresh one
  (`StatsPreference`, its own stored value beside `DisplayOptions`). A launch
  with any launch setting is a harness's and starts with them off, so a
  measured run never inherits the overlay; `ANDARIUM_STATS` (`1` or `0`) states
  the value. Such a launch keeps nothing, and neither does one that states
  the resolution, so what the person stored stays (`PreferenceKeeper`).
- **Graphics memory.** The runtime counts the graphics allocations the host
  refused the guest ([`docs/graphics.md`](../docs/graphics.md#device-memory-budget)).
  While a normal session runs in the foreground the model reads the count every
  2 s, with or without a statistics view (`AppModel.refreshGraphicsNotice`), and
  a rise shows `GraphicsNoticeBanner`: a glass banner that takes touches only
  where it is drawn, dismissible and gone after 30 s. It tells the person to
  swipe a broken app away in Recents, which restarts the process that lost its
  graphics. `GraphicsNotice` decides: the first reading is a baseline, and
  refusals less than a minute apart are one burst, which shows one notice
  whether or not it was dismissed. Recovery runs no app and is not read. In the
  same loop `GraphicsReclaim` frees the budget silently: when it stays nearly
  full or the guest is refused, it ends the cached background app that holds
  the most, over the guest's shell. It acts only on a holder the guest kernel
  names by process, never on the foreground, visible, perceptible or home app.
  It stands down while the guest's own low-memory killer reclaims the budget
  (`guest_reclaims`, runtime API 29), which is then the only one that ends an
  app.
- **Network denied.** While the application is active and has a guest, the
  model watches the system's network path (`NetworkAccessWatcher`, an
  `NWPathMonitor` that calls back for a change of the path alone and makes no
  request). A path unsatisfied because iOS denies Andarium cellular data or
  Wi-Fi shows one notice per cause, a card on Home and a banner in a session
  (`NetworkWords`), with Open Settings and a dismissal that holds until the
  path has been allowed; no network at all, an inactive VPN and the Local
  Network permission are not this notice. The runtime is told which denial is
  in force (runtime API 36), and the session record notes it beside the counts
  of the guest's network (`NetworkRecord`), which the bug report carries in
  words.
- **Session ends.** The runtime records how each session ended
  ([`docs/architecture.md`](../docs/architecture.md)). The next launch keeps
  that record as the previous session's (`PreviousSession`) and, after an end
  that was neither Android's own nor a stop, Home shows a notice with its cause
  (`SessionRecord.notice`). `ExitReports` keeps the exit and diagnostic reports
  MetricKit delivers beside the records. The record's stall slot (`StallRecord`)
  says when the guest's vCPUs stopped running on the host and what the host was
  doing; the bug report carries it in words, with the guest's `/proc` counters
  (`GuestProcFiles`), which adds at most 10 s in all to a report and says per
  file why one is missing. Everything Andarium keeps or copies of a session is
  bounded: a record longer than the runtime writes (16 KiB at most) is not read
  or copied, and a line says so; a report of the stalls is under 80 KiB per
  session whatever a record claims; and MetricKit reports are 30 at most, of
  1 MiB at most each.

## Android operations

Andarium is the adb host for its one guest. The runtime opens a connected
stream socket to the guest's vsock port 5555
(`kernarium_runtime_loader_connect_vsock`), so adbd is reached by no other
process and through no listening socket of the device's unless ADB over network
is on. `AndariumCore/Adb` speaks the protocol.

- **Key.** `AdbKeyStore` keeps one RSA-2048 key per guest in the Keychain, this
  device only and not synchronized. Its public key goes in each boot request
  (`android_adb_public_key`), which adbd authorizes for that boot.
- **Create bug report and Install APK** are in every build, in a normal
  session. A bug report is Android's zip with Andarium's session records and
  MetricKit reports added under `andarium/`, the application's container paths
  taken out (`BugReportAttachments`); a zip that cannot be amended safely is
  shared as Android made it. An install streams the file to the package
  manager in pieces that never outlive their read (`FileChunkReader`). It
  reads the file's size once the coordinated read has the file, and refuses
  before the first byte when the device (`TransferRoom`) or the virtual
  machine's data partition lacks room for the app and its reserve
  (`InstallRoom`), or when the file is a single .apk of 4 GiB or more, which
  Android cannot install (`InstallRefusal.tooLarge`). The first attempt is
  always the plain install. When Android's own answer is the refusal of an app
  built for a much older version (`INSTALL_FAILED_DEPRECATED_SDK_VERSION`),
  and for no other answer, an alert asks whether to install it anyway, and
  Install anyway sends the same file again with the package manager's
  `--bypass-low-target-sdk-block`, once (`LegacyInstallFlow`). Nothing else in
  Andarium sends that flag, and the choice is kept nowhere.
- **File browser** in Settings browses the guest's internal storage
  (`/storage/emulated/0/`, the guest Files app's root) and imports and exports
  files over the same channel, in a normal session and whatever the guest's USB
  debugging setting is, since adbd serves the application's key either way
  (`Live/LiveGuestFiles.swift`, `AdbSync`, `GuestFiles`). One transfer runs
  at a time; Install APK and Create bug report wait for it. An import is
  written to a hidden `.andarium-*.part` name beside its destination and moved
  into place, so a stopped or failed one leaves nothing and a Replace keeps the
  original until the new file is whole. A partial file that the guest's abrupt
  end or iOS ending the application left behind is removed by the next boot
  that may find one, whatever its folder and with no import or open Settings
  needed: the boot looks when an import did not end cleanly, when an older data
  disk was put in place (a snapshot or update restore), and on the first boot
  of a guest an earlier version made, and an ordinary boot does not
  (`PartialFileSweep`, `PartialSweepLedger`). An import in progress is never
  touched. The move's own shell command then has
  the guest's media index complete the file's row (a file the shell user
  writes has only a pending one, which MediaStore keeps from other apps).
  A transfer stops when the application leaves the foreground, and its files
  say so. Turning USB debugging off restarts adbd, which ends a transfer in
  flight: it fails, and Try Again runs it whole. Android reports its boot done
  before internal storage is mounted, so the browser is unavailable until a
  check in the guest finds the storage mounted, writable and holding a
  standard folder (`InternalStorage`); nothing is imported or exported before
  then.
- **ADB over network** is off at every run and never persisted, and is
  available only while the guest's USB debugging is on. `AdbNetworkBridge`
  listens on TCP 5555 and relays each computer to a fresh vsock stream, serving
  at most 8 at once; it understands no byte of adb, so the guest's adbd
  authenticates the computer. A connection whose peer address is one of the
  device's own, loopback included, is closed at once, so no process of the
  device (another application, or adb on the same Mac when Andarium runs on
  that Mac) reaches the guest's adbd through the bridge; a computer on the
  network, another Mac included, is served. iOS asks for Local Network access
  once, through `NSLocalNetworkUsageDescription`. Nothing listens before the
  toggle.
- **Privacy.** The adb features add no required-reason API beyond the
  disk-space API with reason E174.1 (the start's own space check already uses
  it) and, for the file browser, the file-timestamp API: an import reads the
  date of a file the person picked (3B52.1) and an export sets the date of its
  staged copy inside the application's container (C617.1), so both keep the
  file's own date.

## Acknowledgements

The application's Info screen lists every project it carries and shows its
copyright holders, license and full license text. The list is the bundled resource
`Andarium/Shared/Acknowledgements.json`, generated by
`scripts/acknowledgements.py` from the verified dependency sources and the
kernel's license files (never fetched; `--verify` compares the resource with
them). A tooling test fails when the Andarium project embeds a framework, or an
iOS device prefix contract declares a component, that no entry covers
([`docs/testing.md`](../docs/testing.md#andarium)). Adding a library therefore
means adding its entry to the script's table and running it.

## Languages

Andarium is in English, Simplified Chinese, Traditional Chinese (Taiwan usage)
and Japanese, and follows the language of iOS and the per-app language setting.
The strings are String Catalogs in `Localization/` (screens, permission texts,
What's New notes) and in `AndariumCore`'s resources. The JIT texts are their own
catalog, table `JIT`, which the App Store configuration leaves out of the
bundle and its check asserts is absent. The Send feedback texts are another,
table `Feedback`, built only where Send feedback is compiled in
(`EXCLUDED_SOURCE_FILE_NAMES` in `Config/Andarium.xcconfig`); the bundle check
refuses the table, by its file name, in a product without the feature and
asks for it in every language of one with it. The text of Follow developer on X
is the same, table `FollowX`. English is the key (a readout or a
joined list has a named key whose comment says which readout it serves), and
the person's virtual machine is "virtual machine" in English, 虚拟机, 虛擬機器
and 仮想マシン. Chinese says 你 and writes no space next to Latin text; a
number keeps its space before its unit. Bug report and session record text, log
lines and technical names stay English. Text shown on a card, an alert or an
overlay passes `String.keepingTogether`, so a number stays with its unit and a
short quoted Chinese or Japanese name is not cut across two lines.

`scripts/localization.py check` is the catalogs' gate. After a build with
`SWIFT_EMIT_LOC_STRINGS=YES`, `scripts/localization.py sync --check
--stringsdata <DerivedData>` fails on a key the code uses and a catalog lacks,
or the reverse. It runs on a development build made with
`FEEDBACK_ADDRESS=feedback@example.com FOLLOW_X_HANDLE=example_handle` (the
output of `Andarium` and `AndariumPreview` together; one without Send feedback
or the row has no `Feedback` or `FollowX` strings and fails) and on an App Store build with `--app-store`. A
string no catalog reaches (a `String` built in code) shows as English under
Xcode's Accented Pseudolanguage, so a run with it on is part of adding a
screen.

## Automation

A harness launch (`xcrun devicectl device process launch`, environment given
as `DEVICECTL_CHILD_` variables) reaches a state without touching the screen.
The names are the application's, in `Live/LaunchEnvironment.swift`, which the
App Store configuration leaves out; `AndariumCore` knows the settings
(`LaunchOverrides.Setting`) and not the names. The `KERNARIUM_` names mean what
they mean in Kernarium; the automation Android runtime
(`package-ios-frameworks.sh --runtimes android --android-automation`)
additionally reads `KERNARIUM_ANDROID_ADB_AUTH`
([`Kernarium/README.md`](../Kernarium/README.md#automation-runs)).
`KERNARIUM_DISPLAY_SCALE` (`native` or a number of guest pixels to a point)
states the resolution; the earlier `KERNARIUM_RETINA_MODE` (`1` native, `0`
one pixel to a point) is read where it is not stated.
`ANDARIUM_THERMAL_STATE` (`nominal`, `fair`, `serious` or `critical`) stands
for iOS's thermal state in the Simulator and in automation.
`ANDARIUM_FOLLOW_ORIENTATION` (`1` or `0`) states whether the guest follows the
device's orientation, where the device's idiom (an iPhone follows) would
otherwise decide.
`ANDARIUM_INPUT_PROBE=1` is the UI tests' launch: the session opens at once
with no guest behind it, and what the guest's keyboard and mouse would receive,
and the touches that reach its touchscreen, go to one accessibility element
(`inputProbeLog`) instead of the runtime; Settings carries a text field
(`inputProbeField`) that takes the keys a sheet keeps. The probe's guest
registers each device it is plugged; `unregistered` makes it one that never
does. The launch states "Use the pointer as touch" off, hides the tips and keeps
nothing.
`ANDARIUM_GUEST_INSTANCE=<name>` runs an isolated guest under
`Library/TestGuests/<name>`, for tests that need a fresh guest while the
ordinary guest or its pending update must be preserved. A file `free-space-mib`
in that directory, holding a number, stands for the device's free space in the
run's low-space watch while it exists, so a harness drives the pause without
filling the disk; a file `space-answer` there (`resume`, `shutdown` or
`check`) answers the screen over a paused guest once it offers that answer.
`run_ios_device_android.py --engine jit` runs the guest on the JIT, with the
stand-in debugger of `tests/integration/support/ios_jit_debugger.py` serving
the request that StikDebug serves for a person (`--jit-debugger none` shows the
fall back to gadgets).

On an Apple silicon Mac, `xcodebuild test -scheme Andarium -destination
'platform=macOS,variant=Designed for iPad'
-only-testing:AndariumTests/AutomationHoldTests` is the launcher, with each
setting given as a `TEST_RUNNER_`-prefixed variable. The test run ends the
application, so `TEST_RUNNER_ANDARIUM_HOLD_SECONDS` keeps the host up that
long. The opt-in test switches are in
[`docs/testing.md`](../docs/testing.md#andarium). The console lines the harness
reads are `Shared/ConsoleReport.swift`'s, and an App Store build prints none.

The container's `Documents` is the harness's; Andarium never deletes a file it
did not create. The guest is under `Library/Guest/` and is not the harness's to
write. A launch boots the system the build carries, so another release means
another staged build. iOS asks to allow local network access only when the
guest itself reaches the local network, and only a person answers; a harness
run that provokes it waits for the focus
(`run_ios_device_android.py --wait-active`).
