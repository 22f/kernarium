# Platform video encoder

`video_encoder.hpp` is the platform's hardware video encoder interface; nothing
in it names VideoToolbox, V4L2 or the Virtio wire. The Apple backend is
`apple/video_encoder.mm` (sessions), `apple/video_encoder_discovery.mm`
(capability discovery and its cache) and `apple/video_encoder_bitstream.hpp`
and `apple/video_encoder_levels.hpp` (parameter-set readers and the level
policy). It builds as `UML::VideoEncoder` for macOS, iOS device and iOS
Simulator, and only through public VideoToolbox API. The [Virtio encoder
device](../device/video/README.md#encoder-device-id-30) maps this interface to
the wire and depends on nothing below it. The decoder counterpart is
`video_decoder.hpp` (`UML::VideoDecoder`).

## Discovery

`videoEncodeCapabilities()` returns what an encode on this host proved: a
profile, input layout, size, rate-control mode or other capability is listed only
after a stream that used it came out right. VideoToolbox lists no ranges, so
discovery drives the production encoder, and `prepareVideoEncodeCapabilities()`
runs it on its own thread; the first query waits for it, and a host that
answers nothing reports no encoder. A capability never lists a picture side
below `kVideoEncodeMinimumSide`, and profiles a guest API cannot name (ProRes,
HEVC range extensions, alpha, depth) are not probed.

The result is cached as JSON keyed by the host model, OS build and a format
revision. `UML_VIDEO_ENCODE_CACHE` names the file and an empty value turns the
cache off; a missing, foreign or unreadable file is ignored and replaced.

## Sessions

Hardware is required. The caller writes each frame once, into a pixel buffer
from the session's own pool (`acquireFrame()`, null when the pool is in flight),
and the encoder takes it without a second copy. Every submitted frame is
answered exactly once.

- **Output** is Annex B in decode order, with parameter sets placed as the
  settings ask. Frame type is read from the sample.
- **B-frames** are coded in one pattern no property shortens: a `bFrames`
  request at or above the discovered run codes the pattern, below it none.
- **Levels.** A stream states at least the level it was asked for and at least
  the lowest level its picture size, frame rate and bit rate fit, and the
  encoder lowers VideoToolbox's choice only to a level the stream provably
  fits (`apple/video_encoder_levels.hpp`). The level is that of the settings a
  session is made from; a rate or frame-rate update does not restate it, so a
  caller that will raise the rate asks for the level it needs.
- **Input and colour.** A layout is listed for a profile only when an encode
  from it came out right. `rgba` is `bgra` with its red and blue bytes
  exchanged on submit, and a full-range request from either is converted by the
  platform because VideoToolbox converts RGB to video range only. The colour
  description (`VideoEncodeColor`) is attached to the buffers and states itself
  in the stream's VUI, each aspect independently.
- **Rate control.** The standard encoder runs real-time. The low-latency encoder
  is chosen by the platform, never by the caller, when a stream needs temporal
  layers or a QP per frame type. `update()` applies a bitrate, peak or frame
  rate change to the running session; a changed quality or QP bound replaces
  the session between two frames and continues from a key frame.

## Errors and recovery

`configure()` answers `invalid`, `unsupported`, `exhausted` or `failed`. A
session the system revokes is rebuilt at the same settings and the unanswered
frames are encoded again, the first as a key frame. A second loss with no
output in between ends the stream: every frame in flight and every later one
is answered with an error until `reset()` starts a new stream.

## Tests

`platform-video-encoder-bitstream` needs no hardware. `platform-video-encoder`
exercises every advertised capability on the host's encoder and decodes the
streams with the platform decoder. `platform-video-encoder-recovery` links a
variant of the backend that fails sessions on demand, and
`platform-video-encoder-cache` runs child processes against cache files. The
three hardware tests share the `video-encoder-hardware` resource lock.
`Kernarium/KernariumTests/HostVideoEncoderTests.mm` checks the same on an
iPad.

# Platform camera

`camera.hpp` is the platform's camera interface; nothing in it names
AVFoundation, V4L2 or the Virtio wire. `UML::AppleCamera`
(`apple/camera_capture.mm`) implements it for macOS, iOS device and iOS
Simulator, and `camera_authorization.hpp` is the system's camera permission, in
the style of `microphone_authorization.hpp`. A Virtio camera device maps the
interface to the wire.

- **Discovery** (`cameras()`, `concurrentSets()`) reads devices, formats, ranges
  and the multi-camera sets and needs no authorization; it never prompts. The
  iOS 27.1 outer and inner ultra wide device types (the foldable iPhone's) are
  read only behind an availability check, and a device on 27.0 lists none. The
  order is stable, and every index in the interface is an index into it. A
  front camera publishes no flash: its flash is the screen's, which the guest's
  camera unit does not model.
- **Sessions.** `openSession()` is the one call that can light the system's
  capture indicator, and it returns null unless the system has granted access.
  One session is a single `AVCaptureMultiCamSession` on iOS (an
  `AVCaptureSession` on macOS) with an input and a video data output per running
  camera and a photo output once a still is asked for. Frames are NV12, full
  range, at the stream's size scaled by the platform from the format's at its
  aspect ratio (`deliveredFrameSize`), delivered through `CameraSessionDelegate`
  with the exposure that made them; holding a frame's `owner` holds the
  capture's buffer, and its `native` is the platform's own handle to the
  picture. A camera runs in one capture format, its **anchor**, whatever it
  is asked: a stream, a still, a still beside a stream, alone or with other
  cameras. Discovery lists the formats as data (`CameraInfo::captureFormats`);
  `chooseAnchor` picks the anchor once from them by the key in
  `photo_dimensions.hpp`, and the device gives it to the session with
  `setAnchors()` together with what the guest may ask. The session has no
  chooser and never moves a camera to another format: a camera's active format
  changes only when its input joins the session. A camera joins through one
  transaction (`admit`: the input, the anchor and its rate, the video output,
  the photo output, inside one configuration), which, when a step is refused or
  the session's hardware cost goes over its budget, removes exactly what that
  call added. While another camera is in the session every anchor must be a
  format a multi-camera session takes (`multiCamera`); a camera whose best
  such format offers less than its best format does not run with others (the
  device then publishes it as a conflict). A camera that only takes stills
  leaves the session, with its photo output, when its last still is delivered.
  A camera whose format changed has its settings applied again. The video
  output's explicit size has the anchor's aspect ratio and the photo output's
  maximum is a size the anchor lists (`outputSizesFor`), both set after the
  output has its source. A photo output's delegate is held by the session until
  its capture ends.
- **Rates** are whole numbers of frames a second. A span's end within 0.15 % of
  a whole number is that number (29.97 is 30), and the rate is programmed as the
  span's own duration where the reciprocal would fall outside it
  (`rateSettingFor`). The guest is offered the whole rates of the anchor up to
  30 (`videoRateCap`); an anchor with none that low offers its slowest.
- **Controls** arrive as one `CameraSettings` per camera, and the session
  applies what differs. A zoom camera (a virtual device) takes automatic
  exposure, focus and white balance only.
- **Stills** are a JPEG, a Bayer still, or both from one capture. A Bayer still
  carries its DNG's colour data (`dng_color.hpp`), the only place the host
  reveals it. A part that fails leaves the others: a capture fails only when
  nothing it asked for arrived. A JPEG is made at exactly the size asked for
  (`photo_dimensions.hpp`): the capture is refused when the active format does
  not list that size, and the photo is dropped when the platform resolved
  another. A photo output gets its maximum size only after it is attached to the
  session, as AVFoundation requires: the largest size the anchor lists within
  the guest's still cap, so no larger pipeline is prepared. The guest is offered
  the sizes the anchor makes (`stillsOf`), and a camera that only takes stills
  runs in the same anchor. A
  still's `exposure` is the photo's own where the photo states it: the exposure
  time, ISO, f-number, focal length and flash come from the photo's Exif
  (`apple/photo_metadata.hpp`, `still_exposure.hpp`), and a Bayer still's
  neutral point and noise profile from its DNG. The 3A states, white balance,
  zoom and lens are the session's as it reports them at delivery, not as they
  were at the exposure, and on macOS the session reports none of them, so they
  keep their defaults. A reading the photo does not give, or gives as a number
  that cannot be a measurement (an ISO outside 1 to 1,000,000, an exposure time
  of zero or over an hour, a negative or wider Flash field, not finite), is
  zero: the photo gave none, never a made-up value. A lens has one aperture and
  one focal length, so the lens's own is used when the photo names none. The
  flash fired is the Exif's word, then the resolved settings'.
- **Interruptions** (another application, a call, system pressure, iPad
  multitasking) arrive as events; the session does not stop itself. System
  pressure on a running camera also caps its frame rate (20 at serious, 15 at
  critical), as Apple advises, to the largest rate the camera offers below the
  cap (`throttledRate`), before the session is interrupted. A runtime
  error is restarted once: the restart ends with a `resumed` event, and an
  error before that ends the episode with `failed`, so that the client closes
  the session and tries again later.
- `nv12_transform.hpp` crops, rotates and scales NV12 pictures: the portable
  `transformNv12()` is the reference, and `Camera::makeFrameTransformer()` gives
  the host's fastest. The Apple camera's (`apple/accelerated_nv12.hpp`) scales a
  picture that arrives as a pixel buffer on the system's pixel-transfer scaler
  and turns quarter turns with vImage, with its buffers kept between frames; a
  copy and a quarter turn are exact and a scale agrees with the portable one
  within the filters' difference (`camera-apple-transform`).
- The synthetic camera is `synthetic_camera/` (`UML::SyntheticCamera`), a
  platform camera with no hardware: profiles of a phone (a back zoom camera over
  three lenses, and a front camera), a tablet (and one with both of an iPad's
  front cameras) and a laptop; the multi-camera rule, which counts cameras that
  only take stills and refuses a join over a hardware budget (200 million
  pixels a second); a deterministic chart
  whose top row encodes the camera and the frame counter (`decodeChartMarker`);
  brightness, blur, tint and zoom that follow the settings; JPEG and Bayer
  stills; simulated interruptions; and, with a manual clock, frames that a test
  pumps. App Store runtimes do not build it. It is never a real camera.

Tests: `camera-frame-transform` and `camera-dng-color` need nothing;
`camera-synthetic` exercises the synthetic camera through the interface; and
`camera-apple-discovery` reads the host's own cameras on any host. The Apple
backend's sessions are not run by a test, because a test must never capture from
a real camera.
