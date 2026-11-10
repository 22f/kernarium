# Private Virtio video wire contract

`virtio_video.h` owns the packed little-endian protocol shared by the host
Virtio video devices and the maintained mainline and Android kernel drivers. It
is a project-local version of the stateful Virtio video draft, with the decoder
as device ID 31 and the encoder as device ID 30, and it is independent of the
public API and the host/kernel bridge ABI. The wire format is the same for every
maintained kernel slice.

Edit this canonical header, then use `sync-protocol.py --kernel-source PATH` to
update the copy under `include/uapi/linux/` in each patch-producing kernel
checkout. Amend the owning video topic and regenerate each line's series;
ordinary checks use `sync-protocol.py --kernel-source PATH --verify`. Never edit
generated patch output. A change to the driver runs `guest-v4l2-decode`,
`guest-v4l2-encode` and `guest-video-teardown` before a kernel release.

`virtio_video_v4l2_ids.h` lists the V4L2 numbers the host uses, one
`#define UML_<kernel symbol> <number>` per value. `sync-protocol.py` checks each
against the UAPI headers of the tree it synchronizes, so the host and the
kernel it is built beside cannot disagree.

## Resources and failed buffers

A scatter entry normally names guest-physical memory; with
`VIRTIO_VIDEO_F_RESOURCE_SHM_REGION` negotiated it may name an offset inside a
virtio shared-memory region instead, for a host-visible buffer with no
guest-physical address. The host validates stream IDs, buffer extents, and
response capacities.

`VIRTIO_VIDEO_CMD_RESOURCE_ATTACH` carries a buffer's scatter lists as
device-readable data after the command. A driver may spread them over any
number of descriptors and must not need one block of kernel memory for them.
The device accepts at most 32768 entries per plane and 128 MiB per resource,
and the request has to fit the command queue's descriptors, because the device
accepts no indirect ones.

A capture buffer completed with `VIRTIO_VIDEO_DEQUEUE_FLAG_ERR` or
`VIRTIO_VIDEO_DEQUEUE_FLAG_EOS`, on the decoder and the encoder alike, reaches
the client with no payload and the timestamp of the picture or frame the device
failed to deliver, or 0 for a buffer that stands for none. The
`guest-video-error-answer` test checks it.

## Decoder

The wire names the H.264 levels 1.0 to 6.2, the HEVC levels 1 to 6.2, and
Constrained Baseline and Constrained High as H.264 profiles of their own. The
host lists the levels up to the highest its decoder decodes, and the driver
turns them into the read-only V4L2 profile and level menus; a value appended to
the wire needs its row in `virtio_video_helpers.c`, which reads an unknown value
as the menu's first entry. The [device README](../../../darwin/device/video/README.md)
owns which levels the host decodes.

The device ends a picture size with one more capture buffer, empty and flagged
`VIRTIO_VIDEO_DEQUEUE_FLAG_EOS`, after every picture of that size, and only then
posts `VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED`. The driver reports the
buffer as `V4L2_BUF_FLAG_LAST` and reads the new parameters before it queues
`V4L2_EVENT_SOURCE_CHANGE`; `VIDIOC_DQBUF` then fails with `EPIPE` until the
client restarts the capture queue. A drain the client asked for ends the old size
at the drain's own last buffer, so a client takes the first LAST buffer it sees
as the end of its drain. The `guest-v4l2-decode` probe holds both orders.

## Encoder (device ID 30)

The encoder is its own device instance, and its commands are additions the
decoder never uses. It offers `VIRTIO_VIDEO_F_V4L2_CONTROLS`, the driver binds
device 30 only when it is negotiated, and a decoder never offers it. The input
queue carries raw frames (V4L2 OUTPUT) and the output queue carries coded
buffers (V4L2 CAPTURE). The comment above `virtio_video_resource_queue_resp` in
`virtio_video.h` states the buffer rules: timestamps, decode order, completion
of an input buffer once the host has copied it, and drain.

Controls cross the wire as V4L2 control IDs, menu indices, and enum values,
which are stable UAPI. The host lists every control it supports with its type,
range, step, default, menu skip mask, and flags, and the driver registers
exactly that list and forwards values unchanged, so a capability the host
learns needs no driver change. The
[device README](../../../darwin/device/video/README.md#encoder-controls) owns
which controls the host publishes.

The encoder driver gives the host these guarantees: it sends a raw frame only
once the coded queue streams and no drain is pending, so the host builds its
session from settled controls; it reports the host's own plane layout and
ignores a client's strides; and the colorimetry fields of the raw queue go to
the host as the client states them. A `CODEC_CONFIG` buffer reaches the client
with no frame-type flag, because V4L2 has none.

The V4L2 core resets a transfer function it does not know to
`V4L2_XFER_FUNC_DEFAULT` on every `VIDIOC_S_FMT`, and upstream has none for
hybrid log-gamma. The kernel line therefore carries one local UAPI extension,
`V4L2_XFER_FUNC_HLG = 8` (the topic "media: v4l2: add the HLG transfer
function"). If upstream adds an HLG value, adopt upstream's in that topic and in
`virtio_video_v4l2_ids.h`.

## Camera (device ID 65533)

One camera device carries every camera of the host. Its ID is
`UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID`, an experimental ID of the specification's
local range after the battery's and the GNSS receiver's. It offers
`VIRTIO_VIDEO_F_V4L2_CONTROLS`, guest pages, non-contiguous buffers, and shared
memory regions where the host has one, and the codecs' two queues (commands,
events). The driver makes each camera a capture node, for its video and still
streams, and a metadata node, for its meta streams.

- **Cameras.** `VIRTIO_VIDEO_CMD_CAMERA_QUERY` pages through
  `virtio_video_camera_desc` records: facing, lens, flags, conflicting and
  concurrent cameras, the optics, ranges, and colour data once the host has
  learned it. Camera 0 is the back zoom camera when the host has one, a lens
  otherwise; the front camera follows, then each back lens. A lens a host lacks
  has no record. `VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS` answers as
  `QUERY_CAPABILITY` does: NV12 sizes with frame-rate ranges for a video
  stream, JPEG sizes and the Bayer size for a still stream.
- **Streams.** `VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE` names a camera and a
  kind (video, still, meta); every other command is the codecs'. A stream has
  only the output queue, the guest queues empty buffers on it, and the device
  decides each plane's size and stride and reports them in `GET_PARAMS`. The
  completion is `virtio_video_camera_queue_resp`: the codecs' response and the
  sequence of the exposure. A frame's `timestamp` is its age in nanoseconds
  when the device completes it, so the driver stamps it with its own clock minus
  that age.
- **Exposures and metadata.** Each exposure has a sequence, in capture order
  from 1 for each camera. Every video frame and still it produces carries the
  sequence, and every meta stream of the camera completes a buffer holding the
  `virtio_video_camera_meta` of that sequence: what the camera actually did, so
  a control that the host applies as soon as it can stays honest.
- **Stills.** Queueing a buffer on a still stream takes one capture with the
  controls in force when the device receives it. A JPEG still carries a
  reserved Exif segment whose payload offset and length are in the metadata, for
  the guest to fill in place. A Bayer still is 16-bit little-endian samples up to
  the camera's white level. Buffers queued with
  `VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE` and one non-zero timestamp are filled
  from a single capture (a JPEG with its Bayer still). The group's assembly has
  two seconds from its first buffer; a started capture has ten more before its
  buffers complete with `VIRTIO_VIDEO_DEQUEUE_FLAG_ERR`. A still that cannot be
  taken completes with the same flag: a blocked session adds
  `_CAMERA_BLOCKED | _CAMERA_BLACK`, an interrupted capture or a session that
  cannot open adds `_CAMERA_INTERRUPTED`. A Bayer still that fails alone fails
  its own buffer only, and the JPEG of the group completes.
- **Controls.** `VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS` lists a camera's
  controls in the encoder's descriptor format. The standard ones are V4L2
  camera, flash and white-balance controls (`virtio_video_v4l2_ids.h`, which
  `sync-protocol.py` checks); the rest are the `VIRTIO_VIDEO_CAMERA_CID_*` of
  `virtio_video.h`. Controls belong to the camera: a set or get through any of
  its streams reaches the same values.
- **Blocked and interrupted.** A session the host starts blocked reports
  `VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED` and delivers black frames flagged
  `VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK | _BLOCKED` at the stream's rate, and
  never opens a capture session. A live session sends black frames flagged
  `_INTERRUPTED` while the capture cannot run, and flagged `_RESUMING` from the
  creation of a stream, from a restart of its capture and from the end of an
  interruption until the host's capture delivers its first frame, so an
  application sees a short dark gap and no stall. The camera events
  (`VIRTIO_VIDEO_EVENT_CAMERA_*`, stream ID the camera index) report each change
  once: a camera that was reported interrupted or unavailable is reported
  resumed or available when its interruption ends, including when its session
  was closed and opened again meanwhile, and a capture that failed is reported
  back when it delivers a frame.
- **Suspend and reset.** The camera's event buffers stay in the guest's ring:
  the device answers one only when it has an event, and the buffer the guest
  posted waits in the ring, across a suspend as well. A guest suspend that finds
  a request held (a queued stream buffer) resets the device, as it resets any
  device of the transport, and the driver does not use
  `virtio_uml_set_no_vq_suspend`. The driver therefore stops the device's
  streams before it suspends, which completes every held buffer, and keeps its
  own V4L2 state; a camera with no stream holds nothing, so its suspend resets
  nothing and its next event reaches the buffer already posted. After a reset
  the driver creates its streams again, applies the controls again and queues
  the buffers; the device keeps nothing across a reset and has stopped every
  capture (its camera indicator is off and no host buffer remains), and the
  first frames after a re-created stream are black and flagged `_RESUMING`.
