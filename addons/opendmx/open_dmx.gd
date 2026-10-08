class_name OpenDMX
extends Node
## Drives an Enttec Open DMX USB or DMX USB Pro interface (one DMX512 universe).
##
## Godot has no serial port API and the Open DMX needs the host to generate
## the DMX stream continuously, so this node spawns a small helper program
## (addons/opendmx/bin/<platform>/opendmx_helper) and pipes channel values to
## it. Works on macOS, Linux and Windows (FTDI VCP driver).
## The helper keeps refreshing the universe at about 38 Hz on its own.
##
## [codeblock]
## var dmx := OpenDMX.new()
## add_child(dmx)
## if dmx.open():
##     dmx.set_channel(1, 255)
## [/codeblock]

## Emitted when the helper stops by itself (interface unplugged, error...).
signal disconnected

enum Interface {
	AUTO, ## Open DMX or DMX USB Pro, guessed from the port name.
	OPEN_DMX, ## Enttec Open DMX USB (FT232R).
	DMX_USB_PRO, ## Enttec DMX USB Pro.
}

const CHANNEL_COUNT := 512
const HELPER_NAME := "opendmx_helper"

## Open the first interface found as soon as the node enters the tree.
@export var auto_open := false
## Serial port to use, as returned by [method list_ports]; empty = first found.
@export var port := ""
## Kind of interface. On Linux and Windows the port name does not reveal a
## DMX USB Pro, so choose [constant DMX_USB_PRO] there.
@export var interface := Interface.AUTO
## Keep the last values on the fixtures when closing instead of a blackout.
@export var hold_on_close := false

var _values := PackedByteArray()
var _dirty := false
var _pid := -1
var _stdio: FileAccess
var _stderr: FileAccess


func _init() -> void:
	_values.resize(CHANNEL_COUNT)


func _ready() -> void:
	if auto_open:
		open()


func _exit_tree() -> void:
	close()


func _process(_delta: float) -> void:
	if _pid < 0:
		return
	_forward_errors()
	if not OS.is_process_running(_pid):
		_cleanup()
		disconnected.emit()
		return
	# At most one update per rendered frame, however many channels changed
	if _dirty:
		_dirty = false
		_stdio.store_buffer(_values)
		_stdio.flush()


## Lists the serial ports that look like an FTDI interface:
## "usb:<serial number>" when the helper talks to the chip directly over USB,
## otherwise /dev/cu.usbserial* on macOS, /dev/ttyUSB* on Linux, COMx on Windows.
static func list_ports() -> PackedStringArray:
	var ports := PackedStringArray()
	var output := []
	if OS.execute(_helper_path(), ["--list"], output) != 0:
		return ports
	for line in str(output[0]).split("\n", false):
		ports.append(line.strip_edges())
	return ports


## Starts the helper on [param port_path] (or [member port], or the first
## interface found). Returns false if the helper could not be started; a
## wrong port is reported a moment later through [signal disconnected].
func open(port_path := "") -> bool:
	close()
	if port_path != "":
		port = port_path

	var helper := _helper_path()
	if not FileAccess.file_exists(helper):
		push_error("OpenDMX: helper not found at %s" % helper)
		return false

	var args := PackedStringArray()
	if hold_on_close:
		args.append("--hold")
	if interface == Interface.OPEN_DMX:
		args.append("--open")
	elif interface == Interface.DMX_USB_PRO:
		args.append("--pro")
	if port != "":
		args.append(port)

	var info := OS.execute_with_pipe(helper, args, false)
	if info.is_empty():
		push_error("OpenDMX: cannot start %s" % helper)
		return false

	_pid = info["pid"]
	_stdio = info["stdio"]
	_stderr = info["stderr"]
	_dirty = true
	return true


## Stops the helper. Fixtures go dark unless [member hold_on_close] is set.
func close() -> void:
	if _pid < 0:
		return
	# Closing its stdin makes the helper send its last frame and exit
	_stdio.close()
	var deadline := Time.get_ticks_msec() + 500
	while OS.is_process_running(_pid) and Time.get_ticks_msec() < deadline:
		OS.delay_msec(10)
	if OS.is_process_running(_pid):
		OS.kill(_pid)
	_cleanup()


func is_open() -> bool:
	return _pid >= 0


## Sets one channel. [param channel] is 1 to 512, [param value] 0 to 255.
func set_channel(channel: int, value: int) -> void:
	if channel < 1 or channel > CHANNEL_COUNT:
		push_error("OpenDMX: channel %d out of range (1-512)" % channel)
		return
	value = clampi(value, 0, 255)
	if _values[channel - 1] != value:
		_values[channel - 1] = value
		_dirty = true


func get_channel(channel: int) -> int:
	if channel < 1 or channel > CHANNEL_COUNT:
		return 0
	return _values[channel - 1]


## Sets consecutive channels starting at [param first_channel] (1-based).
func set_channels(first_channel: int, values: PackedByteArray) -> void:
	if first_channel < 1 or first_channel + values.size() - 1 > CHANNEL_COUNT:
		push_error("OpenDMX: channels %d..%d out of range (1-512)"
				% [first_channel, first_channel + values.size() - 1])
		return
	for i in values.size():
		_values[first_channel - 1 + i] = values[i]
	_dirty = true


## Sets every channel to zero.
func blackout() -> void:
	_values.fill(0)
	_dirty = true


static func _helper_path() -> String:
	var platform := OS.get_name().to_lower()
	var file := HELPER_NAME + (".exe" if platform == "windows" else "")
	# In an exported game res:// lives inside the .pck, so the helper has to
	# be shipped next to the executable.
	if OS.has_feature("editor"):
		return ProjectSettings.globalize_path(
				"res://addons/opendmx/bin/%s/%s" % [platform, file])
	return OS.get_executable_path().get_base_dir().path_join(file)


func _cleanup() -> void:
	_pid = -1
	_stdio = null
	_stderr = null


# The helper's pipes are non-blocking: this returns at once when it has
# nothing to say.
func _forward_errors() -> void:
	while true:
		var line := _stderr.get_line()
		if line == "":
			break
		push_error("OpenDMX helper: " + line)
