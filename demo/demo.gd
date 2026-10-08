extends Control
# Demo: four faders driving DMX channels 1 to 4 on the first Open DMX found.

const FADER_COUNT := 4

var _dmx: OpenDMX
var _status: Label
var _sliders: Array[VSlider] = []


func _ready() -> void:
	_build_ui()

	_dmx = OpenDMX.new()
	add_child(_dmx)
	_dmx.disconnected.connect(_on_disconnected)
	_connect()


func _build_ui() -> void:
	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "top", "right", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 16)
	add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	_status = Label.new()
	column.add_child(_status)

	var faders := HBoxContainer.new()
	faders.size_flags_vertical = Control.SIZE_EXPAND_FILL
	faders.alignment = BoxContainer.ALIGNMENT_CENTER
	faders.add_theme_constant_override("separation", 32)
	column.add_child(faders)
	for i in FADER_COUNT:
		faders.add_child(_build_fader(i + 1))

	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_CENTER
	buttons.add_theme_constant_override("separation", 12)
	column.add_child(buttons)
	_add_button(buttons, "Blackout", _set_all.bind(0))
	_add_button(buttons, "Full", _set_all.bind(255))
	_add_button(buttons, "Reconnect", _connect)


# One fader: its value on top, the slider, the channel number below
func _build_fader(channel: int) -> Control:
	var box := VBoxContainer.new()
	box.custom_minimum_size.x = 48

	var value := Label.new()
	value.text = "0"
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(value)

	var slider := VSlider.new()
	slider.min_value = 0
	slider.max_value = 255
	slider.step = 1
	slider.size_flags_vertical = Control.SIZE_EXPAND_FILL
	slider.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	slider.value_changed.connect(func(v: float) -> void:
		value.text = str(int(v))
		_dmx.set_channel(channel, int(v)))
	box.add_child(slider)
	_sliders.append(slider)

	var name_label := Label.new()
	name_label.text = "Ch %d" % channel
	name_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(name_label)
	return box


func _add_button(parent: Control, text: String, action: Callable) -> void:
	var button := Button.new()
	button.text = text
	button.pressed.connect(action)
	parent.add_child(button)


func _connect() -> void:
	var ports := OpenDMX.list_ports()
	if ports.is_empty():
		_status.text = "No Open DMX found"
	elif _dmx.open(ports[0]):
		_status.text = "Connected to %s" % ports[0]
	else:
		_status.text = "Could not start the helper (see Output)"


func _set_all(value: int) -> void:
	for slider in _sliders:
		slider.value = value


func _on_disconnected() -> void:
	_status.text = "Disconnected (see Output) - press Reconnect"
