extends SceneTree

const TEST_DIR := "user://_test_resource_snapshot_modified"

var _failures: int = 0


func _check(cond: bool, label: String) -> void:
	if cond:
		print("PASS: ", label)
	else:
		push_error("FAIL: " + label)
		printerr("FAIL: ", label)
		_failures += 1


func _cleanup_dir(dir_path: String) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.list_dir_begin()
	var name := dir.get_next()
	while name != "":
		if not dir.current_is_dir():
			dir.remove(name)
		name = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(dir_path))


func _initialize() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TEST_DIR))

	_test_custom_resource()
	_test_png_and_image_texture()
	_test_audio_stream_wav()
	_test_audited_bulk_resources()
	_test_packed_scene_and_duplicate()

	_cleanup_dir(TEST_DIR)

	if _failures > 0:
		printerr("FAILED: %d check(s) failed." % _failures)
		quit(1)
	else:
		print("ALL CHECKS PASSED.")
		quit(0)


func _test_custom_resource() -> void:
	var script_path := TEST_DIR + "/custom_res.gd"
	var f := FileAccess.open(script_path, FileAccess.WRITE)
	_check(f != null, "open custom resource script for write")
	f.store_string("""extends Resource
@export var count: int = 0
@export var label: String = ""
@export var sub: Resource
""")
	f.close()

	var script_res := ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(script_res != null, "load custom resource script")
	_check(not script_res.snapshot_is_modified(), "loaded GDScript starts unmodified")

	var sub_src := Resource.new()
	sub_src.resource_name = "initial_sub"

	var src = script_res.new()
	src.count = 7
	src.label = "hello"
	src.sub = sub_src

	var tres_path := TEST_DIR + "/custom_res.tres"
	var res_path := TEST_DIR + "/custom_res.res"
	var threaded_path := TEST_DIR + "/custom_res_threaded.tres"

	_check(ResourceSaver.save(src, tres_path) == OK, "save custom resource .tres")
	_check(ResourceSaver.save(src, res_path) == OK, "save custom resource .res")
	_check(ResourceSaver.save(src, threaded_path) == OK, "save custom resource threaded .tres")

	# 1) Text loader (.tres) + sub-resource
	var loaded_tres = ResourceLoader.load(tres_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded_tres != null, "load custom resource .tres")
	_check(not loaded_tres.snapshot_is_modified(), ".tres main resource unmodified after load")
	_check(loaded_tres.sub != null and not loaded_tres.sub.snapshot_is_modified(), ".tres sub-resource unmodified after load")

	loaded_tres.count = 42
	_check(loaded_tres.snapshot_is_modified(), ".tres main resource modified after property set")
	_check(not loaded_tres.sub.snapshot_is_modified(), ".tres sub-resource still unmodified before own edit")

	loaded_tres.sub.resource_name = "edited_sub"
	_check(loaded_tres.sub.snapshot_is_modified(), ".tres sub-resource modified after property set")

	# 2) Binary loader (.res) + sub-resource
	var loaded_res = ResourceLoader.load(res_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded_res != null, "load custom resource .res")
	_check(not loaded_res.snapshot_is_modified(), ".res main resource unmodified after load")
	_check(loaded_res.sub != null and not loaded_res.sub.snapshot_is_modified(), ".res sub-resource unmodified after load")

	loaded_res.label = "updated"
	_check(loaded_res.snapshot_is_modified(), ".res main resource modified after property set")
	loaded_res.sub.set_meta("tag", 123)
	_check(loaded_res.sub.snapshot_is_modified(), ".res sub-resource modified after set_meta")

	# 3) Threaded load + cache-reuse preservation
	var req_err := ResourceLoader.load_threaded_request(threaded_path, "", false, ResourceLoader.CACHE_MODE_REUSE)
	_check(req_err == OK, "load_threaded_request .tres")
	var loaded_threaded = ResourceLoader.load_threaded_get(threaded_path)
	_check(loaded_threaded != null, "load_threaded_get .tres")
	_check(not loaded_threaded.snapshot_is_modified(), "threaded .tres main resource unmodified after load")
	_check(loaded_threaded.sub != null and not loaded_threaded.sub.snapshot_is_modified(), "threaded .tres sub-resource unmodified after load")

	loaded_threaded.count = 99
	_check(loaded_threaded.snapshot_is_modified(), "threaded .tres modified after property set")

	var cached_again = ResourceLoader.load(threaded_path, "", ResourceLoader.CACHE_MODE_REUSE)
	_check(cached_again == loaded_threaded, "CACHE_MODE_REUSE returns same cached instance")
	_check(cached_again.snapshot_is_modified(), "CACHE_MODE_REUSE does not clear modified flag on cache hit")


func _test_png_and_image_texture() -> void:
	var png_path := TEST_DIR + "/sample.png"
	var img_src := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	img_src.fill(Color.RED)
	_check(img_src.save_png(png_path) == OK, "save sample .png")

	var img_loaded := Image.load_from_file(png_path)
	_check(img_loaded != null, "Image.load_from_file sample.png")
	_check(not img_loaded.snapshot_is_modified(), "Image.load_from_file starts unmodified")
	img_loaded.set_pixel(0, 0, Color.BLUE)
	_check(img_loaded.snapshot_is_modified(), "Image modified after set_pixel")

	var img_fresh := Image.load_from_file(png_path)
	var tex_src := ImageTexture.create_from_image(img_fresh)
	var tex_tres_path := TEST_DIR + "/sample_tex.tres"
	var tex_res_path := TEST_DIR + "/sample_tex.res"
	_check(ResourceSaver.save(tex_src, tex_tres_path) == OK, "save ImageTexture .tres")
	_check(ResourceSaver.save(tex_src, tex_res_path) == OK, "save ImageTexture .res")

	var tex_loaded: ImageTexture = ResourceLoader.load(tex_tres_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(tex_loaded != null, "load ImageTexture .tres")
	_check(not tex_loaded.snapshot_is_modified(), "loaded ImageTexture .tres starts unmodified")

	var img_update := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	img_update.fill(Color.GREEN)
	tex_loaded.update(img_update)
	_check(tex_loaded.snapshot_is_modified(), "ImageTexture modified after update()")

	var tex_bin: ImageTexture = ResourceLoader.load(tex_res_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(tex_bin != null, "load ImageTexture .res")
	_check(not tex_bin.snapshot_is_modified(), "loaded ImageTexture .res starts unmodified")
	tex_bin.set_size_override(Vector2i(8, 8))
	_check(tex_bin.snapshot_is_modified(), "ImageTexture modified after set_size_override()")


func _test_audio_stream_wav() -> void:
	var wav_src := AudioStreamWAV.new()
	wav_src.format = AudioStreamWAV.FORMAT_8_BITS
	wav_src.mix_rate = 22050
	wav_src.data = PackedByteArray([0, 16, 32, 48])

	var wav_path := TEST_DIR + "/sample_wav.tres"
	_check(ResourceSaver.save(wav_src, wav_path) == OK, "save AudioStreamWAV .tres")

	var wav_prop: AudioStreamWAV = ResourceLoader.load(wav_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(wav_prop != null, "load AudioStreamWAV .tres (property test)")
	_check(not wav_prop.snapshot_is_modified(), "loaded AudioStreamWAV starts unmodified (property test)")
	wav_prop.data = PackedByteArray([1, 2, 3, 4])
	_check(wav_prop.snapshot_is_modified(), "AudioStreamWAV modified after data property assign")

	var wav_method: AudioStreamWAV = ResourceLoader.load(wav_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(wav_method != null, "load AudioStreamWAV .tres (method test)")
	_check(not wav_method.snapshot_is_modified(), "loaded AudioStreamWAV starts unmodified (method test)")
	wav_method.set_data(PackedByteArray([5, 6, 7, 8]))
	_check(wav_method.snapshot_is_modified(), "AudioStreamWAV modified after set_data() call")


func _test_audited_bulk_resources() -> void:
	# ArrayMesh (clear_surfaces)
	var mesh_src := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3.ZERO, Vector3.RIGHT, Vector3.UP])
	mesh_src.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mesh_path := TEST_DIR + "/sample_mesh.tres"
	_check(ResourceSaver.save(mesh_src, mesh_path) == OK, "save ArrayMesh .tres")

	var mesh_loaded: ArrayMesh = ResourceLoader.load(mesh_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(mesh_loaded != null and not mesh_loaded.snapshot_is_modified(), "loaded ArrayMesh starts unmodified")
	mesh_loaded.clear_surfaces()
	_check(mesh_loaded.snapshot_is_modified(), "ArrayMesh modified after clear_surfaces()")

	# BitMap (set_bit)
	var bmp_src := BitMap.new()
	bmp_src.create(Vector2i(4, 4))
	var bmp_path := TEST_DIR + "/sample_bitmap.tres"
	_check(ResourceSaver.save(bmp_src, bmp_path) == OK, "save BitMap .tres")

	var bmp_loaded: BitMap = ResourceLoader.load(bmp_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(bmp_loaded != null and not bmp_loaded.snapshot_is_modified(), "loaded BitMap starts unmodified")
	bmp_loaded.set_bit(1, 1, true)
	_check(bmp_loaded.snapshot_is_modified(), "BitMap modified after set_bit()")

	# FontFile (set_cache_ascent)
	var font_src := FontFile.new()
	var font_path := TEST_DIR + "/sample_font.tres"
	_check(ResourceSaver.save(font_src, font_path) == OK, "save FontFile .tres")

	var font_loaded: FontFile = ResourceLoader.load(font_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(font_loaded != null and not font_loaded.snapshot_is_modified(), "loaded FontFile starts unmodified")
	font_loaded.set_cache_ascent(0, 16, 12.0)
	_check(font_loaded.snapshot_is_modified(), "FontFile modified after set_cache_ascent()")

	# JSON (parse)
	var json_src := JSON.new()
	_check(json_src.parse('{"a": 1}') == OK, "parse initial JSON")
	var json_path := TEST_DIR + "/sample_json.tres"
	_check(ResourceSaver.save(json_src, json_path) == OK, "save JSON .tres")

	var json_loaded: JSON = ResourceLoader.load(json_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(json_loaded != null and not json_loaded.snapshot_is_modified(), "loaded JSON starts unmodified")
	_check(json_loaded.parse('{"a": 2}') == OK, "re-parse loaded JSON")
	_check(json_loaded.snapshot_is_modified(), "JSON modified after parse()")


func _test_packed_scene_and_duplicate() -> void:
	# PackedScene (.tscn) with an embedded sub-resource
	var root := Sprite2D.new()
	root.name = "Root"
	var img := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	img.fill(Color.WHITE)
	root.texture = ImageTexture.create_from_image(img)

	var scene_src := PackedScene.new()
	_check(scene_src.pack(root) == OK, "pack PackedScene with sub-resource")
	root.free()

	var tscn_path := TEST_DIR + "/sample_scene.tscn"
	_check(ResourceSaver.save(scene_src, tscn_path) == OK, "save PackedScene .tscn")

	var scene_loaded: PackedScene = ResourceLoader.load(tscn_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(scene_loaded != null, "load PackedScene .tscn")
	_check(not scene_loaded.snapshot_is_modified(), "loaded PackedScene starts unmodified")

	var inst: Sprite2D = scene_loaded.instantiate()
	_check(inst != null and inst.texture != null, "instantiate PackedScene with texture sub-resource")
	_check(not inst.texture.snapshot_is_modified(), "PackedScene embedded sub-resource starts unmodified")
	inst.free()

	# Resource.duplicate() and Image.duplicate() preserve modified state
	var res_src := Resource.new()
	res_src.resource_name = "dup_test"
	var dup_path := TEST_DIR + "/dup_test.tres"
	_check(ResourceSaver.save(res_src, dup_path) == OK, "save Resource for duplicate test")

	var loaded_res: Resource = ResourceLoader.load(dup_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(not loaded_res.snapshot_is_modified(), "loaded Resource unmodified before duplicate")
	var dup_clean: Resource = loaded_res.duplicate()
	_check(not dup_clean.snapshot_is_modified(), "duplicate of unmodified Resource is unmodified")
	loaded_res.resource_name = "changed"
	var dup_dirty: Resource = loaded_res.duplicate()
	_check(dup_dirty.snapshot_is_modified(), "duplicate of modified Resource is modified")

	var png_path := TEST_DIR + "/sample.png"
	var img_loaded := Image.load_from_file(png_path)
	var img_dup_clean: Image = img_loaded.duplicate()
	_check(not img_dup_clean.snapshot_is_modified(), "duplicate of unmodified Image is unmodified")
	img_loaded.set_pixel(0, 0, Color.YELLOW)
	var img_dup_dirty: Image = img_loaded.duplicate()
	_check(img_dup_dirty.snapshot_is_modified(), "duplicate of modified Image is modified")

