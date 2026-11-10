# Virtio input

`darwin/device/input/virtio` is the host end of the guest's Virtio-input devices
(device ID 18) over the common vhost-user transport. The guest finds them
through the kernel's `virtio_input` driver, so they are ordinary evdev devices.
`darwin/device/input/vnc` turns a VNC client's keys and pointer into events for
the touchscreen and the side-key keyboard.

## Devices

A device is identified by its name and its vendor and product (`1af4:NNNN`,
bus `BUS_VIRTUAL`). Android keys keyboard layouts and settings to that
identity, so every name and product below is permanent once shipped, and so is
each device's bitmaps: only an addition is compatible.

| Kind | Product | Name | Events |
|---|---|---|---|
| touchscreen | 0001 | Kernarium Touchscreen | multitouch, `INPUT_PROP_DIRECT` |
| side keys | 0002 | Kernarium Keyboard | Power, the volume keys, Back, Home |
| hardware keyboard | 0003 | Kernarium Hardware Keyboard | `EV_KEY` exactly the codes `hid_keyboard.hpp` can produce, `EV_LED` Num, Caps and Scroll Lock |
| mouse | 0004 | Kernarium Mouse | `EV_REL` X, Y and both wheels in steps and in high-resolution units, `EV_KEY` the five mouse buttons, `EV_LED` `LED_MISC` |
| pointer | 0005 | Kernarium Pointer | `EV_KEY` `BTN_TOUCH`, `BTN_TOOL_MOUSE` and the five buttons, `EV_ABS` slot (0 to 9), tracking id and position 0 to 32767; no input property |

The pointer is the mouse's absolute half: Android takes `BTN_TOOL_MOUSE` with
absolute positions as an absolute mouse and draws its own cursor. A host input
is the position over the guest display on the touchscreen's range, and
`pointerRawPosition` converts it so that Android's one uniform scale of the
raw range (the larger side of the display over 32768) lands on the pixel a
touch there would touch.

The hardware devices send no key repeat (Android repeats keys itself), send a
key by its position (HID usage, through the table that transcribes the Linux
HID driver's `hid_keyboard[]`), and take the layout from Android.
`LED_MISC` on the mouse is how an Android application tells the host it
captured the pointer.

## Connected state

The keyboard, the mouse and the pointer exist from boot and the host plugs and
unplugs them (`EventQueue::Attachment::pluggable`). The mouse and the pointer
are one mouse to Android and are plugged together.

- Unplugged, a device answers empty event bitmaps, which is a device with no
  events; plugged, it answers its own.
- One configuration change is raised for each plug and each unplug, and none
  while the state is unchanged.
- Before an unplug the host sends the releases of every key and button it
  holds and ends the pointer's contact, so nothing is left down.
- Input for an unplugged device is dropped.
- A plugged device whose guest input device is not yet registered holds events
  in a bounded buffer (256). The guest driver announces a registration with a
  status event ([`kernel/common/input`](../../../kernel/common/input/README.md));
  the buffer is then delivered. If it overflows, or the notice never comes, the
  events are dropped, and the host forgets the keys and the contact it held
  (`HardwareInput` resets its builders when a registration reports lost
  events), so no key stays down in the host's view only.
- The status queue also carries Android's LED writes, which reach
  `HardwareInput::setStatusCallback` as Num, Caps and Scroll Lock and as
  pointer capture on or off, and each registration the guest announces.

A kernel without the guest driver's change never plugs: it sees an unplugged
device as an input device with no events, which Android drops, and the host
drops what it held after the bound.

## Wake

`HardwareReport::wakes` is true for a key or a button going down and for a
wheel taking a whole step, and false for motion and every release. The
runtime sends a wake to a guest that suspended itself for the former only, so a
bumped mouse never resumes it.

## Testing

`tests/unit/input/virtio` drives each device through its queues and its event
descriptor with no guest (`ctest -R '^virtio-input-'`): the protocol and the
connected state, the HID table against the kernel's rows, and the reports, the
pointer mapping and the three devices together.
