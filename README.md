# GodotOpenDMX

A Godot 4 addon that drives an **Enttec Open DMX USB** interface (or any
FT232R-based clone) from GDScript: one DMX512 universe, 512 channels.

```gdscript
var dmx := OpenDMX.new()
add_child(dmx)
if dmx.open():
    dmx.set_channel(1, 255)
```

Full API and internals: [addons/opendmx/DOCUMENTATION.md](addons/opendmx/DOCUMENTATION.md).

## Status

| Platform | State |
|---|---|
| macOS (Intel) | Built and tested up to the USB interface: the port opens and frames are sent without error. Not yet confirmed on a fixture. |
| macOS (Apple Silicon) | Needs its own build of the helper. |
| Linux | Written, never compiled. |
| Windows | Written, never compiled. |

## How it works

Godot has no serial or USB API, and the Open DMX is a bare FTDI chip with no
microcontroller: the computer has to generate the whole DMX signal itself and
repeat it continuously. The addon therefore has two parts:

- `OpenDMX`, a GDScript node that holds the 512 channel values;
- `opendmx_helper`, a small C program the node launches and feeds through a
  pipe. It refreshes the universe at about 38 Hz on its own.

## Run the demo

1. Plug in the Open DMX, with a fixture on DMX addresses 1 to 4.
2. In Godot, **Import** `project.godot` and press **F5**.

The demo shows four faders for channels 1 to 4, plus **Blackout**, **Full**
and **Reconnect** buttons. The line at the top shows the interface in use.

From a terminal:

```bash
/Applications/Godot.app/Contents/MacOS/Godot --path .
```

## Install in your own project

1. Copy the `addons/opendmx` folder into your project's `addons` folder.
2. In **Project > Project Settings > Plugins**, enable **OpenDMX**.
3. Add an `OpenDMX` node to a scene, or create one from code as above.

For an exported game, copy the helper for the target platform next to the
game executable (see the documentation).

## Requirements

- Godot 4.6. Earlier 4.x versions are untested.
- **macOS:** nothing to install; the helper talks to the interface over USB.
- **Linux:** permission on `/dev/ttyUSB*` (usually the `dialout` group).
- **Windows:** the FTDI VCP driver, so the interface shows up as a COM port.

## Troubleshooting

| What you see | Cause |
|---|---|
| `Ports: []` / "No Open DMX found" | The interface is not plugged in, or not seen by the system. |
| "... is in use by another program" | QLC+, DMXDesktop or another DMX program has the interface open. Only one program can use it at a time. |
| "helper not found at ..." | No helper has been built for this platform; see "Building the helper" in the documentation. |
| Connected, but the fixture does not react | Check the fixture's DMX address and mode, then the cable. If those are right, please report it: output on a real fixture is not yet confirmed. |

## Layout

```
addons/opendmx/
  open_dmx.gd          the OpenDMX node
  plugin.cfg, plugin.gd
  DOCUMENTATION.md
  bin/<platform>/      helper binaries
  helper/              helper source (C)
demo/                  four-fader demo
```
