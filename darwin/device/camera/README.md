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
  manual exposure, focus and white balance where the host allows them.
- **Tiers.** A fixed GPU budget under 1 GiB limits a session to 1080p video,
  12 MP stills and four buffers; otherwise 4K, 48 MP and six.
- **Frames.** Video buffers hold NV12, BT.709 full range, rows at the 16-byte
  pitch of `raw_pitch.hpp`. The device crops (centred, to the stream's aspect),
  rotates and scales in portable C++ (`nv12_transform.hpp`), and drops frames to
  a stream's rate. A frame with no queued buffer is dropped and its number spent.
- **Stills.** A JPEG buffer carries the platform's file behind a reserved Exif
  APP1 segment for the guest to fill in place (`jpeg_gap.hpp`); a RAW buffer
  carries 16-bit rows at the 16-byte pitch. Buffers of one non-zero cookie
  queued with `GROUP_MORE` are filled from one capture, which starts with the
  buffer that lacks the flag; a group not complete in two seconds fails.
- **Metadata.** A meta stream completes one `virtio_video_camera_meta` per
  exposure, with the sequence the frames and stills carry; black frames get a
  record too.
- **Black and interruptions.** Blocked, interrupted, restarting and test-pattern
  frames are black (`fillNv12Black`) and flagged; they are timed by the device,
  so a stalled platform camera does not stall the guest.
- **Calibration.** RAW is offered only for a lens whose colour data is known.
  The first still of an uncalibrated lens at unit zoom also takes a Bayer still
  and keeps what its DNG reveals in the `CalibrationStore` (per host model and
  lens); the next guest sees RAW. A guest keeps seeing what it saw at start.
- **Controls** belong to the camera, whichever stream carries the command, and a
  set applies every value or none. The table is `camera_controls.cpp`; the
  platform sees one `CameraSettings` per camera.

## Tests

`camera-controls`, `camera-description`, `camera-jpeg-gap` and
`camera-calibration` check the pure parts. `camera-device` drives the device over
both queues in front of the synthetic camera, including the invariant that a
blocked session never opens a platform session. The Apple backend's capture is
not run by any test.
