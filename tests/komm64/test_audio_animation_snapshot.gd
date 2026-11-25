extends SceneTree

# Headless verification for:
# 1. AnimationPlayer snapshot_get_state / snapshot_set_state with playback_queue and playback.blend
# 2. AudioStreamPlayer / 2D / 3D snapshot_get_state / snapshot_set_state with max_polyphony > 1
#
# Run with:
#   godot --headless --audio-driver Dummy --fixed-fps 60 --script tests/komm64/test_audio_animation_snapshot.gd

var _failures: int = 0


func _check(cond: bool, label: String) -> void:
	if cond:
		print("PASS: ", label)
	else:
		push_error("FAIL: " + label)
		printerr("FAIL: ", label)
		_failures += 1


func _initialize() -> void:
	_run_all.call_deferred()


func _run_all() -> void:
	_test_animation_queue_and_blend()
	_test_animation_stacked_blends()
	_test_animation_blend_with_section_and_speed()
	await _test_audio_polyphony()
	await _test_audio_polyphony_2d_3d()
	await _test_audio_voice_lifecycle()

	if _failures > 0:
		printerr("FAILED: %d check(s) failed." % _failures)
		quit(1)
	else:
		print("ALL CHECKS PASSED.")
		quit(0)


func _make_anim(val_start: float, val_end: float, length: float = 0.5) -> Animation:
	var anim := Animation.new()
	anim.length = length
	var track := anim.add_track(Animation.TYPE_VALUE)
	anim.track_set_path(track, NodePath(".:position:x"))
	anim.track_insert_key(track, 0.0, val_start)
	anim.track_insert_key(track, length, val_end)
	return anim


func _make_anim_pair() -> Dictionary:
	var target := Node2D.new()
	root.add_child(target)

	var ap := AnimationPlayer.new()
	ap.callback_mode_process = AnimationMixer.ANIMATION_CALLBACK_MODE_PROCESS_MANUAL
	target.add_child(ap)

	var lib := AnimationLibrary.new()
	lib.add_animation(&"a", _make_anim(0.0, 100.0, 0.5))
	lib.add_animation(&"b", _make_anim(200.0, 300.0, 0.5))
	lib.add_animation(&"c", _make_anim(400.0, 500.0, 0.5))
	lib.add_animation(&"d", _make_anim(600.0, 700.0, 0.5))
	ap.add_animation_library(&"", lib)

	return { "target": target, "ap": ap }


func _check_anim_match(ref_pair: Dictionary, test_pair: Dictionary, step_label: String) -> void:
	var ref_ap: AnimationPlayer = ref_pair["ap"]
	var test_ap: AnimationPlayer = test_pair["ap"]
	var ref_target: Node2D = ref_pair["target"]
	var test_target: Node2D = test_pair["target"]

	_check(test_ap.is_playing() == ref_ap.is_playing(),
		"%s: is_playing matches (%s vs %s)" % [step_label, test_ap.is_playing(), ref_ap.is_playing()])
	_check(test_ap.current_animation == ref_ap.current_animation,
		"%s: current_animation matches (%s vs %s)" % [step_label, test_ap.current_animation, ref_ap.current_animation])
	if ref_ap.is_playing() and test_ap.is_playing():
		_check(is_equal_approx(test_ap.current_animation_position, ref_ap.current_animation_position),
			"%s: position matches (%.6f vs %.6f)" % [step_label, test_ap.current_animation_position, ref_ap.current_animation_position])
	_check(test_ap.get_queue() == ref_ap.get_queue(),
		"%s: queue matches (%s vs %s)" % [step_label, test_ap.get_queue(), ref_ap.get_queue()])
	_check(is_equal_approx(test_target.position.x, ref_target.position.x),
		"%s: blended target position.x matches (%.6f vs %.6f)" % [step_label, test_target.position.x, ref_target.position.x])


func _test_animation_queue_and_blend() -> void:
	var ref_pair := _make_anim_pair()
	var test_pair := _make_anim_pair()
	var ref_ap: AnimationPlayer = ref_pair["ap"]
	var test_ap: AnimationPlayer = test_pair["ap"]
	var ref_target: Node2D = ref_pair["target"]
	var test_target: Node2D = test_pair["target"]

	for ap: AnimationPlayer in [ref_ap, test_ap]:
		ap.play(&"a")
		ap.advance(0.15)
		ap.play(&"b", 0.4)
		ap.queue(&"c")
		ap.queue(&"d")
		ap.advance(0.1)

	var state := test_ap.snapshot_get_state()
	var saved_x := test_target.position.x

	_check(state.has("blend") and state["blend"] is Array and state["blend"].size() == 1,
		"anim blend saved with 1 active blend")
	_check(state.has("queue") and state["queue"] is Array and state["queue"].size() == 2,
		"anim queue saved with 2 entries")
	_check(is_equal_approx(test_target.position.x, ref_target.position.x),
		"anim initial mid-blend target matches reference")

	# Mutate test player heavily.
	test_ap.clear_queue()
	test_ap.play(&"d")
	test_ap.advance(0.2)
	_check(test_ap.current_animation == &"d" and test_ap.get_queue().is_empty(),
		"anim mutated before restore")

	# Restore state and node property (as the addon does).
	test_target.position.x = saved_x
	var err := test_ap.snapshot_set_state(state)
	_check(err == OK, "anim snapshot_set_state returns OK")
	_check_anim_match(ref_pair, test_pair, "anim_qb immediately after restore")

	var steps: Array[float] = [0.1, 0.1, 0.1, 0.15, 0.2, 0.35, 0.25, 0.3]
	for i in range(steps.size()):
		ref_ap.advance(steps[i])
		test_ap.advance(steps[i])
		_check_anim_match(ref_pair, test_pair, "anim_qb step %d (+%.2fs)" % [i, steps[i]])

	ref_target.queue_free()
	test_target.queue_free()


func _test_animation_stacked_blends() -> void:
	var ref_pair := _make_anim_pair()
	var test_pair := _make_anim_pair()
	var ref_ap: AnimationPlayer = ref_pair["ap"]
	var test_ap: AnimationPlayer = test_pair["ap"]
	var test_target: Node2D = test_pair["target"]

	for ap: AnimationPlayer in [ref_ap, test_ap]:
		ap.play(&"a")
		ap.advance(0.1)
		ap.play(&"b", 0.5)
		ap.advance(0.1)
		ap.play(&"c", 0.4)
		ap.advance(0.05)

	var state := test_ap.snapshot_get_state()
	var saved_x := test_target.position.x
	_check(state["blend"].size() == 2, "stacked blends saved with 2 entries")

	test_ap.stop()
	test_target.position.x = saved_x
	_check(test_ap.snapshot_set_state(state) == OK, "stacked blends restore OK")

	for i in range(6):
		ref_ap.advance(0.08)
		test_ap.advance(0.08)
		_check_anim_match(ref_pair, test_pair, "stacked_blend step %d" % i)

	ref_pair["target"].queue_free()
	test_target.queue_free()


func _test_animation_blend_with_section_and_speed() -> void:
	var ref_pair := _make_anim_pair()
	var test_pair := _make_anim_pair()
	var ref_ap: AnimationPlayer = ref_pair["ap"]
	var test_ap: AnimationPlayer = test_pair["ap"]
	var test_target: Node2D = test_pair["target"]

	for ap: AnimationPlayer in [ref_ap, test_ap]:
		ap.play_section(&"a", 0.1, 0.45, -1.0, 1.5)
		ap.advance(0.05)
		ap.play(&"b", 0.3, 0.8)
		ap.queue(&"c")
		ap.advance(0.05)

	var state := test_ap.snapshot_get_state()
	var saved_x := test_target.position.x
	test_ap.play(&"d")
	test_ap.advance(0.1)

	test_target.position.x = saved_x
	_check(test_ap.snapshot_set_state(state) == OK, "section+speed blend restore OK")

	for i in range(8):
		ref_ap.advance(0.1)
		test_ap.advance(0.1)
		_check_anim_match(ref_pair, test_pair, "section_speed step %d" % i)

	ref_pair["target"].queue_free()
	test_target.queue_free()


func _make_wav(duration_sec: float, mix_rate: int = 44100) -> AudioStreamWAV:
	var wav := AudioStreamWAV.new()
	wav.format = AudioStreamWAV.FORMAT_16_BITS
	wav.mix_rate = mix_rate
	wav.stereo = false
	var sample_count := int(duration_sec * mix_rate)
	var data := PackedByteArray()
	data.resize(sample_count * 2)
	for i in range(sample_count):
		var s := int(sin(float(i) * 0.05) * 16000.0)
		data[i * 2] = s & 0xFF
		data[i * 2 + 1] = (s >> 8) & 0xFF
	wav.data = data
	return wav


func _test_audio_polyphony() -> void:
	var wav := _make_wav(2.0)
	var asp := AudioStreamPlayer.new()
	asp.stream = wav
	asp.max_polyphony = 3
	root.add_child(asp)

	asp.play(0.0)
	asp.play(0.3)
	asp.play(0.6)

	var state := asp.snapshot_get_state()
	_check(state.has("playbacks") and state["playbacks"] is Array,
		"audio state uses playbacks Array")
	_check(state["playbacks"].size() == 3,
		"audio state captured all 3 playbacks (got %d)" % state["playbacks"].size())

	var saved_positions: Array[float] = []
	for entry in state["playbacks"]:
		saved_positions.append(float(entry["position"]))

	asp.stop()
	_check(not asp.is_playing(), "audio stopped before restore")
	_check(asp.snapshot_get_state()["playbacks"].is_empty(), "audio stopped state has 0 playbacks")

	var err := asp.snapshot_set_state(state)
	_check(err == OK, "audio snapshot_set_state returns OK")
	_check(asp.is_playing(), "audio is_playing after restore")

	var restored := asp.snapshot_get_state()
	_check(restored["playbacks"].size() == 3,
		"audio restored all 3 active playbacks (got %d)" % restored["playbacks"].size())
	if restored["playbacks"].size() == 3:
		for i in range(3):
			var got_pos := float(restored["playbacks"][i]["position"])
			_check(absf(got_pos - saved_positions[i]) < 0.02,
				"audio voice %d position restored in order (%.4f vs %.4f)" % [i, got_pos, saved_positions[i]])

	# Restoring a 1-voice state over 3 active voices must replace, not append.
	var one_state := { "playbacks": [{ "position": 0.4 }], "stream_paused": false, "finishing": false }
	asp.snapshot_set_state(one_state)
	var after_one := asp.snapshot_get_state()
	_check(after_one["playbacks"].size() == 1,
		"audio restore replaces existing voices (got %d)" % after_one["playbacks"].size())

	# Paused polyphony round-trip.
	asp.stop()
	asp.play(0.1)
	asp.play(0.5)
	asp.play(0.9)
	asp.stream_paused = true
	var paused_state := asp.snapshot_get_state()
	_check(paused_state["playbacks"].size() == 3 and paused_state["stream_paused"] == true,
		"audio paused state captured 3 paused playbacks")
	asp.stop()
	asp.snapshot_set_state(paused_state)
	var restored_paused := asp.snapshot_get_state()
	_check(restored_paused["playbacks"].size() == 3 and asp.stream_paused,
		"audio paused restore kept all 3 paused playbacks")

	asp.stop()
	asp.queue_free()
	await process_frame


func _test_audio_polyphony_2d_3d() -> void:
	var wav := _make_wav(2.0)

	var p2d := AudioStreamPlayer2D.new()
	p2d.stream = wav
	p2d.max_polyphony = 3
	root.add_child(p2d)

	var p3d := AudioStreamPlayer3D.new()
	p3d.stream = wav
	p3d.max_polyphony = 3
	root.add_child(p3d)

	# Play 3 voices across 3 physics frames so all 3 enter AudioServer.
	for pos: float in [0.0, 0.3, 0.6]:
		p2d.play(pos)
		p3d.play(pos)
		await physics_frame
		await physics_frame

	var s2d := p2d.snapshot_get_state()
	var s3d := p3d.snapshot_get_state()
	_check(s2d["playbacks"].size() == 3, "AudioStreamPlayer2D captured 3 playbacks (got %d)" % s2d["playbacks"].size())
	_check(s3d["playbacks"].size() == 3, "AudioStreamPlayer3D captured 3 playbacks (got %d)" % s3d["playbacks"].size())

	p2d.stop()
	p3d.stop()
	p2d.snapshot_set_state(s2d)
	p3d.snapshot_set_state(s3d)

	# Advance physics frames to ensure none of the 3 voices get dropped by process().
	await physics_frame
	await physics_frame

	var r2d := p2d.snapshot_get_state()
	var r3d := p3d.snapshot_get_state()
	_check(r2d["playbacks"].size() == 3, "AudioStreamPlayer2D kept all 3 playbacks after physics frames (got %d)" % r2d["playbacks"].size())
	_check(r3d["playbacks"].size() == 3, "AudioStreamPlayer3D kept all 3 playbacks after physics frames (got %d)" % r3d["playbacks"].size())

	p2d.stop()
	p3d.stop()
	p2d.queue_free()
	p3d.queue_free()
	await process_frame


func _test_audio_voice_lifecycle() -> void:
	# The mixer runs on wall-clock time (also with the Dummy driver), not per
	# process frame, so wait on time; each check has >= 0.15 s of margin.
	var wav := _make_wav(1.0)
	var asp := AudioStreamPlayer.new()
	asp.stream = wav
	asp.max_polyphony = 3
	root.add_child(asp)

	asp.play(0.0)
	asp.play(0.4)
	asp.play(0.8)
	var state := asp.snapshot_get_state()
	asp.stop()
	asp.snapshot_set_state(state)
	var t0 := Time.get_ticks_msec()

	# At 0.35 s: voice 2 (0.8 + 0.35 > 1.0) finished; voices 0 and 1 remain.
	await _wait_until(t0, 350)
	var mid1 := asp.snapshot_get_state()
	_check(asp.is_playing() and mid1["playbacks"].size() == 2,
		"lifecycle at 0.35 s: 2 voices left (got %d)" % mid1["playbacks"].size())

	# At 0.8 s: voice 1 (0.4 + 0.8 > 1.0) finished; voice 0 remains.
	await _wait_until(t0, 800)
	var mid2 := asp.snapshot_get_state()
	_check(asp.is_playing() and mid2["playbacks"].size() == 1,
		"lifecycle at 0.8 s: 1 voice left (got %d)" % mid2["playbacks"].size())

	# At 1.4 s: voice 0 finished too.
	await _wait_until(t0, 1400)
	var end_state := asp.snapshot_get_state()
	_check(not asp.is_playing() and end_state["playbacks"].is_empty(),
		"lifecycle at 1.4 s: all voices finished (got %d)" % end_state["playbacks"].size())

	asp.queue_free()


func _wait_until(t0: int, ms: int) -> void:
	while Time.get_ticks_msec() - t0 < ms:
		await process_frame
