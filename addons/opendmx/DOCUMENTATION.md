# OpenDMX addon documentation

- [The OpenDMX node](#the-opendmx-node)
- [Examples](#examples)
- [Port names](#port-names)
- [Behaviour to know about](#behaviour-to-know-about)
- [Exporting a game](#exporting-a-game)
- [The helper program](#the-helper-program)
- [Building the helper](#building-the-helper)
- [Platform notes](#platform-notes)
- [Limits](#limits)

## The OpenDMX node

`OpenDMX` extends `Node` and drives one DMX512 universe. Channels are numbered
**1 to 512**, as on a lighting desk; values go from **0 to 255**.

### Properties

| Property | Type | Default | Meaning |
|---|---|---|---|
| `auto_open` | `bool` | `false` | Open the interface as soon as the node enters the tree. |
| `port` | `String` | `""` | Port to use, as returned by `list_ports()`. Empty means the first one found. |
| `hold_on_close` | `bool` | `false` | Keep the last values on the fixtures when closing, instead of a blackout. Read when `open()` is called. |

### Methods

| Method | Returns | Description |
|---|---|---|
| `OpenDMX.list_ports()` (static) | `PackedStringArray` | Interfaces found on this machine. |
| `open(port_path := "")` | `bool` | Starts the helper on `port_path`, else on `port`, else on the first interface found. Closes any connection already open. Returns `false` only if the helper could not be started. |
| `close()` | `void` | Stops the helper. Called automatically when the node leaves the tree. |
| `is_open()` | `bool` | `true` while the helper is running. |
| `set_channel(channel, value)` | `void` | Sets one channel. The value is clamped to 0-255; an out-of-range channel logs an error. |
| `get_channel(channel)` | `int` | Last value set on a channel (0 if out of range). |
| `set_channels(first_channel, values)` | `void` | Sets consecutive channels from a `PackedByteArray`. |
| `blackout()` | `void` | Sets all 512 channels to 0. |

### Signals

| Signal | Emitted when |
|---|---|
| `disconnected` | The helper stopped by itself: no interface found, interface unplugged or in use, wrong port. The reason is logged as an error in the Output panel. |

### Constants

| Constant | Value |
|---|---|
| `CHANNEL_COUNT` | `512` |

## Examples

An RGB fixture on address 10:

```gdscript
func set_color(dmx: OpenDMX, color: Color) -> void:
    dmx.set_channels(10, PackedByteArray([color.r8, color.g8, color.b8]))
```

Choosing an interface and reacting to its loss:

```gdscript
@onready var dmx: OpenDMX = $OpenDMX

func _ready() -> void:
    dmx.disconnected.connect(func(): print("DMX interface lost"))
    var ports := OpenDMX.list_ports()
    if not ports.is_empty():
        dmx.open(ports[0])
```

A fade with a tween:

```gdscript
func fade(dmx: OpenDMX, channel: int, to: int, seconds: float) -> void:
    var from := dmx.get_channel(channel)
    create_tween().tween_method(
            func(v: float): dmx.set_channel(channel, roundi(v)),
            float(from), float(to), seconds)
```

## Port names

`list_ports()` returns names in one of these forms; pass them unchanged to
`open()`.

| Form | Platform | Meaning |
|---|---|---|
| `usb:<serial number>` | macOS, Linux (helper built with libusb) | Direct USB access to the FTDI chip. |
| `/dev/cu.usbserial-...` | macOS | Serial port. |
| `/dev/ttyUSB0` | Linux | Serial port. |
| `COM3` | Windows | Serial port (FTDI VCP driver). |

USB entries come first, so `open()` with no argument prefers them.

## Behaviour to know about

- **Values survive a reconnect.** The node keeps the 512 values; calling
  `open()` again resends them.
- **You can set channels before opening.** They are sent once the interface
  is open.
- **One update per rendered frame.** However many channels change during a
  frame, the node sends the universe once, in `_process`. Changes therefore
  reach the fixtures at your frame rate, capped by the DMX refresh (~38 Hz).
- **`open()` returning `true` is not a connection guarantee.** It means the
  helper started. If the interface is missing or busy, the helper exits a
  moment later and `disconnected` is emitted.
- **Closing blacks out.** When the node closes, or Godot quits normally, the
  helper sends one all-zero frame, unless `hold_on_close` is set. If Godot
  crashes, the helper should see its input close and do the same; this has
  not been tested.
- **One program at a time.** The interface cannot be shared with QLC+ or
  another DMX program.

## Exporting a game

Inside an exported game, `res://` lives in the `.pck` file, where a program
cannot be executed. The node therefore looks for the helper **next to the
game executable**:

| Platform | File to copy next to the executable | Taken from |
|---|---|---|
| macOS | `opendmx_helper` (inside `YourGame.app/Contents/MacOS/`) | `addons/opendmx/bin/macos/` |
| Linux | `opendmx_helper` | `addons/opendmx/bin/linux/` |
| Windows | `opendmx_helper.exe` | `addons/opendmx/bin/windows/` |

A notarized macOS app needs the helper signed along with the rest of the
bundle.

## The helper program

`opendmx_helper` can be run by hand, which is useful for testing.

```
opendmx_helper --list            list candidate ports
opendmx_helper [--hold] [port]   run; first candidate port if none given
```

| Stream | Content |
|---|---|
| stdin | Raw 512-byte blocks, one per universe update. No header, no separator. |
| stdout | `READY <port>` once the interface is open. |
| stderr | `ERROR ...` lines, forwarded by the node to the Godot log. |

The helper keeps sending the last block it received. When stdin closes it
sends one all-zero frame (unless `--hold`) and exits with code 0; it exits
with code 1 if the interface cannot be opened or is lost.

A two-second test from a terminal, which sends blackout frames only:

```bash
sleep 2 | addons/opendmx/bin/macos/opendmx_helper
```

### The DMX frame

Each frame is: a break of at least 200 µs (longer in USB mode), a short
mark-after-break, a zero start code, then the 512 channel values at
250000 baud, 8 data bits, no parity, 2 stop bits. A full frame lasts about
26 ms, which gives roughly 38 frames per second.

### Serial mode and USB mode

- **Serial mode** uses the operating system's serial driver: `termios` on
  macOS and Linux, the Win32 serial API on Windows.
- **USB mode** (helper built with `-DUSE_LIBUSB`) sends FTDI commands to the
  chip directly through libusb, the way QLC+ and OLA do. It is selected when
  the port name starts with `usb:`.

## Building the helper

The source is one file, `addons/opendmx/helper/opendmx_helper.c`. Run the
commands from that folder.

macOS with USB mode (needs `brew install libusb`; libusb is linked in, so the
result has no dependency):

```bash
clang -O2 -DUSE_LIBUSB -I/usr/local/include/libusb-1.0 -o ../bin/macos/opendmx_helper opendmx_helper.c /usr/local/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security
```

On an Apple Silicon Mac, Homebrew lives in `/opt/homebrew`: replace
`/usr/local` with `/opt/homebrew` in that command.

macOS, serial mode only:

```bash
clang -O2 -o ../bin/macos/opendmx_helper opendmx_helper.c
```

Linux, serial mode:

```bash
cc -O2 -o ../bin/linux/opendmx_helper opendmx_helper.c
```

Linux with USB mode (needs the libusb development package):

```bash
cc -O2 -DUSE_LIBUSB -o ../bin/linux/opendmx_helper opendmx_helper.c $(pkg-config --cflags --libs libusb-1.0)
```

Windows with MinGW (or `cl /O2 opendmx_helper.c` with Visual Studio):

```bash
x86_64-w64-mingw32-gcc -O2 -o ../bin/windows/opendmx_helper.exe opendmx_helper.c
```

## Platform notes

### macOS

macOS has a built-in FTDI serial driver, but **FTDI's D2XXHelper**
(`/Library/Extensions/FTDIKext.kext`), which several DMX programs ask you to
install, deliberately stops FTDI chips from appearing as serial ports. On
such a Mac only USB mode works, which is why the shipped macOS helper is
built with libusb.

On a Mac without D2XXHelper, Apple's driver claims the interface and a
`/dev/cu.usbserial-*` port appears. Use that port there; USB mode has not
been tried in that situation and may be refused.

The shipped binary is Intel (x86_64) only.

### Linux

Not compiled or tested yet. In serial mode the user needs access to
`/dev/ttyUSB*`:

```bash
sudo usermod -aG dialout $USER
```

In USB mode the helper takes the interface from the `ftdi_sio` driver, and
needs a udev rule (or root) granting access to the USB device.

### Windows

Not compiled or tested yet. Install the FTDI VCP driver so the interface
appears as a COM port. Auto-detection reads the list of FTDI ports from the
registry; if it finds nothing, pass the port by name: `dmx.open("COM3")`.

## Limits

- Output only: the Open DMX cannot receive DMX.
- One universe per `OpenDMX` node, one node per interface.
- No RDM.
- Timing depends on the operating system's scheduler. That is inherent to
  the Open DMX; an Enttec DMX USB Pro has its own microcontroller and does
  not have this limit, but it uses a different protocol that this addon does
  not speak.
