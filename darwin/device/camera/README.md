# Virtio camera

`UML::CameraDevice` serves the host's cameras to a guest through the camera
device of the Virtio video wire ([`kernel/common/video`](../../../kernel/common/video/README.md),
device ID `cameraDeviceId`). It reuses the common vhost-user transport and the
video device's guest-buffer writer (`UML::VideoGuestBuffers`), and runs on a
platform camera (`darwin/platform/camera.hpp`) that the caller hands it: the
AVFoundation camera on a real host, the synthetic camera in a test or a
launcher's synthetic mode. The device itself links no capture backend.

`makeInProcessCameraService()` starts it as one transport thread. A caller
chooses a `CameraMode` and a `CameraTier` when it starts a guest and passes the
application's statements (the sensor orientation, the rotation that
`ROTATE_AND_CROP` automatic applies, a state callback) as `CameraDeviceOptions`.

## Contract

- **Modes.** Live opens a platform capture session when a guest stream first
  needs it (a queued video buffer, a still, a torch) and closes it when none
  does. Blocked never calls `Camera::openSession`, so the host's capture
  indicator cannot light: the device publishes the cameras that discovery found,
  states `VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED`, and completes every video
  buffer black and flagged blocked on a timer of its own and every still with an
  error. A host that has not granted access behaves as an interrupted live
  session.
- **Cameras.** Wire order is the back zoom camera (else the primary back lens),
  the front camera, the other back lenses, then external cameras, at most eight.
  A zoom camera and its lenses conflict; cameras run together where the host
  says they can. The zoom camera takes automatic controls only. A lens publishes
  manual exposure, focus and white balance where the host allows them. Of the
  front lenses (an iPad lists a wide and an ultra wide one over a single
  sensor) the guest sees the wide one.
- **Tiers.** A fixed GPU budget under 1 GiB limits a session to 1080p video,
  12 MP stills and four buffers; otherwise 4K, 48 MP and six.
- **Frames.** Video buffers hold NV12, BT.709 full range, rows at the 16-byte
  pitch of `raw_pitch.hpp`. The device crops (centred, to the stream's aspect),
  rotates and scales with the platform camera's transformer
  (`Camera::makeFrameTransformer()`; the Apple camera's runs on the system's
  scaler, see [`darwin/platform`](../../platform/README.md)), writes a picture
  that needs none of it straight into the guest's rows, and drops frames to a
  stream's rate. A frame with no queued buffer is dropped and its number spent.
- **Stills.** A JPEG buffer carries the platform's file behind a reserved Exif
  APP1 segment for the guest to fill in place (`jpeg_gap.hpp`); a RAW buffer
  carries 16-bit rows at the 16-byte pitch. Buffers of one non-zero cookie
  queued with `GROUP_MORE` are filled from one capture, which starts with the
  buffer that lacks the flag. A group not complete in two seconds fails; a
  capture that has started has ten seconds to deliver. At most as many captures
  as a tier has buffers are with the platform for one camera; one more fails
  its buffers. A still in a blocked or interrupted session, or one the platform
  refuses, completes with the error flag (and the interrupted flag when the
  session failed or was interrupted). The JPEG ends at the first picture's end
  of image, without a multi-picture index or any picture behind it.
- **Metadata.** A meta stream completes one `virtio_video_camera_meta` per
  exposure, with the sequence the frames and stills carry; black frames get a
  record too.
- **Black and interruptions.** Blocked, interrupted, restarting and test-pattern
  frames are black (`fillNv12Black`) and flagged; they are timed by the device,
  so a stalled platform camera does not stall the guest. A stream's frames are
  black and flagged RESUMING from the first queued buffer until the platform
  delivers its first frame, at a first start and after a resume alike, and
  INTERRUPTED while the camera cannot run (an interruption, system pressure, a
  capture that failed to start, retried once a second). An interruption belongs
  to the platform session it happened in: when the session closes, or a new one
  opens, it is over, and the guest is told it ended. The guest is told each way
  once (`CAMERA_INTERRUPTED` or `CAMERA_UNAVAILABLE`, then `CAMERA_RESUMED` or
  `CAMERA_AVAILABLE`): a second interruption says nothing new, a session that
  fails marks the cameras that have a stream, and a capture that failed is told
  to be back when it delivers a frame, so a start that fails round after round
  tells the guest once.
- **Events and reset.** The event queue never holds a buffer: with no event
  pending it returns retry, so the guest's buffer stays in its ring, and an
  event raised later signals the device's event descriptor so the server runs
  the queue again. A guest reset (and a suspend that finds a request held, which
  resets every device) drops every stream, group, pending event and control
  value; the driver creates them again (see the wire's README). A suspend with
  no stream open holds nothing and resets nothing.
- **Calibration.** RAW is offered only for a lens whose colour data is known.
  The first still of an uncalibrated lens at unit zoom also takes a Bayer still
  and keeps what its DNG reveals in the `CalibrationStore` (per host model and
  lens); the next guest sees RAW. A guest keeps seeing what it saw at start. The
  Bayer part is a courtesy: when the platform refuses it or it fails, the JPEG
  of the same capture is delivered.
- **Controls** belong to the camera, whichever stream carries the command, and a
  set applies every value or none. The table is `camera_controls.cpp`; the
  platform sees one `CameraSettings` per camera.

## Launcher

The macOS launcher starts the device with its `--camera=` option, as the last of
the Virtio devices it creates so that no other device's index moves; its
argument handling in `darwin/app/macos/main.cpp` is the reference for the
options and their values. The `host` source needs the system's camera
permission granted to the program (or the terminal it runs in) beforehand:
nothing in the launcher or the device asks for it, and the launcher refuses to
start, saying how to grant it, when it is not. The tier follows the launch's GPU
budget, and a state line goes to stderr when the host starts or stops capturing.

## Tests

`camera-controls`, `camera-description`, `camera-jpeg-gap` and
`camera-calibration` check the pure parts. `camera-device` drives the device
over both queues in front of the synthetic camera, including the invariant that
a blocked session never opens a platform session, a hostile guest, a reset with
streams and stills in flight, an event queue that retries and wakes the server,
a suspend with no stream open that resets nothing (its test models the
server's rule for a disabled ring), a still on one camera while another streams,
a join over the hardware budget, and a capture that fails to start round after
round. The Apple backend's capture is not run by any test.
