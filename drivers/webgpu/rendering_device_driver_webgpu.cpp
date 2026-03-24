/**************************************************************************/
/*  rendering_device_driver_webgpu.cpp                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef WEBGPU_ENABLED

#include "rendering_device_driver_webgpu.h"
#include "rendering_context_driver_webgpu.h"
#include "rendering_shader_container_webgpu.h"
#include "pixel_formats_webgpu.h"

#include <webgpu/webgpu.h>
#include <emscripten/emscripten.h>
#include <cstdlib>

// Forward declaration for timestamp readback callback (defined below command_timestamp_query_pool_reset).
static void _timestamp_readback_callback(WGPUMapAsyncStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2);

// =============================================================================
// Storage Texture Format Validation
// =============================================================================

// Returns true if the WebGPU texture format is a depth or depth/stencil format.
static bool _is_depth_format(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_Depth16Unorm:
		case WGPUTextureFormat_Depth24Plus:
		case WGPUTextureFormat_Depth24PlusStencil8:
		case WGPUTextureFormat_Depth32Float:
		case WGPUTextureFormat_Depth32FloatStencil8:
			return true;
		default:
			return false;
	}
}

// WebGPU only allows a specific subset of formats for storage textures.
// Returns true if the format is valid, false otherwise.
static bool _is_valid_storage_texture_format(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_RGBA8Unorm:
		case WGPUTextureFormat_RGBA8Snorm:
		case WGPUTextureFormat_RGBA8Uint:
		case WGPUTextureFormat_RGBA8Sint:
		case WGPUTextureFormat_RGBA16Uint:
		case WGPUTextureFormat_RGBA16Sint:
		case WGPUTextureFormat_RGBA16Float:
		case WGPUTextureFormat_R32Float:
		case WGPUTextureFormat_R32Uint:
		case WGPUTextureFormat_R32Sint:
		case WGPUTextureFormat_RG32Float:
		case WGPUTextureFormat_RG32Uint:
		case WGPUTextureFormat_RG32Sint:
		case WGPUTextureFormat_RGBA32Float:
		case WGPUTextureFormat_RGBA32Uint:
		case WGPUTextureFormat_RGBA32Sint:
		case WGPUTextureFormat_BGRA8Unorm:
			return true;
		default:
			return false;
	}
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

RenderingDeviceDriverWebGPU::RenderingDeviceDriverWebGPU(RenderingContextDriverWebGPU *p_context_driver) {
	context_driver = p_context_driver;
}

RenderingDeviceDriverWebGPU::~RenderingDeviceDriverWebGPU() {
	if (push_constant_bind_group) {
		wgpuBindGroupRelease(push_constant_bind_group);
		push_constant_bind_group = nullptr;
	}
	if (push_constant_bind_group_layout) {
		wgpuBindGroupLayoutRelease(push_constant_bind_group_layout);
		push_constant_bind_group_layout = nullptr;
	}
	if (push_constant_ring_buffer) {
		wgpuBufferRelease(push_constant_ring_buffer);
		push_constant_ring_buffer = nullptr;
	}
	if (shader_container_format) {
		memdelete(shader_container_format);
		shader_container_format = nullptr;
	}
}

// =============================================================================
// GENERIC
// =============================================================================

Error RenderingDeviceDriverWebGPU::initialize(uint32_t p_device_index, uint32_t p_frame_count) {
	device = context_driver->get_device();
	queue = context_driver->get_queue();
	ERR_FAIL_COND_V(device == nullptr, ERR_CANT_CREATE);
	ERR_FAIL_COND_V(queue == nullptr, ERR_CANT_CREATE);

	frame_count = p_frame_count;

	// Query device limits.
	_check_capabilities();

	// Create push constant ring buffer.
	{
		WGPUBufferDescriptor desc = {};
		desc.size = PUSH_CONSTANT_RING_SIZE;
		desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		desc.mappedAtCreation = false;
		push_constant_ring_buffer = wgpuDeviceCreateBuffer(device, &desc);
		ERR_FAIL_COND_V(push_constant_ring_buffer == nullptr, ERR_CANT_CREATE);
	}

	// Create a universal push constant bind group layout:
	//   PUSH_CONSTANT_RING_BINDING, all stages, uniform buffer with dynamic offset.
	// All shaders with push constants use this same layout for their push
	// constant slot in the pipeline layout (hasDynamicOffset=true allows
	// reusing one bind group with different ring buffer offsets per draw).
	{
		WGPUBindGroupLayoutEntry pc_entry = {};
		pc_entry.binding = PUSH_CONSTANT_RING_BINDING;
		pc_entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
		pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		pc_entry.buffer.hasDynamicOffset = true;
		pc_entry.buffer.minBindingSize = 0; // Flexible — works for any push constant size.

		WGPUBindGroupLayoutDescriptor layout_desc = {};
		layout_desc.entryCount = 1;
		layout_desc.entries = &pc_entry;
		push_constant_bind_group_layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		ERR_FAIL_COND_V(push_constant_bind_group_layout == nullptr, ERR_CANT_CREATE);

		// Create the bind group (backed by the ring buffer).
		// Dynamic offset is applied at bind time via SetBindGroup(..., 1, &offset).
		WGPUBindGroupEntry bg_entry = {};
		bg_entry.binding = PUSH_CONSTANT_RING_BINDING;
		bg_entry.buffer = push_constant_ring_buffer;
		bg_entry.offset = 0;
		bg_entry.size = PUSH_CONSTANT_SLOT_ALIGNMENT;

		WGPUBindGroupDescriptor bg_desc = {};
		bg_desc.layout = push_constant_bind_group_layout;
		bg_desc.entryCount = 1;
		bg_desc.entries = &bg_entry;
		push_constant_bind_group = wgpuDeviceCreateBindGroup(device, &bg_desc);
		ERR_FAIL_COND_V(push_constant_bind_group == nullptr, ERR_CANT_CREATE);
	}

	// Create a small fallback float texture (4x4, RGBA8Unorm, all zeros).
	// Used when Godot provides a depth-format fallback texture to a BGL entry
	// that expects Float sample type. WebGPU requires format/sample-type match.
	{
		WGPUTextureDescriptor td = {};
		td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		td.dimension = WGPUTextureDimension_2D;
		td.size = { 4, 4, 1 };
		td.format = WGPUTextureFormat_RGBA8Unorm;
		td.mipLevelCount = 1;
		td.sampleCount = 1;
		fallback_float_texture = wgpuDeviceCreateTexture(device, &td);
		if (fallback_float_texture) {
			WGPUTextureViewDescriptor vd = {};
			vd.format = WGPUTextureFormat_RGBA8Unorm;
			vd.dimension = WGPUTextureViewDimension_2D;
			vd.mipLevelCount = 1;
			vd.arrayLayerCount = 1;
			vd.aspect = WGPUTextureAspect_All;
			fallback_float_texture_view = wgpuTextureCreateView(fallback_float_texture, &vd);
		}
	}

	// Create fallback cube texture (4x4, 6 layers) for BGL entries expecting Cube views.
	{
		WGPUTextureDescriptor td = {};
		td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		td.dimension = WGPUTextureDimension_2D;
		td.size = { 4, 4, 6 };
		td.format = WGPUTextureFormat_RGBA8Unorm;
		td.mipLevelCount = 1;
		td.sampleCount = 1;
		fallback_cube_texture = wgpuDeviceCreateTexture(device, &td);
		if (fallback_cube_texture) {
			WGPUTextureViewDescriptor vd = {};
			vd.format = WGPUTextureFormat_RGBA8Unorm;
			vd.dimension = WGPUTextureViewDimension_Cube;
			vd.mipLevelCount = 1;
			vd.arrayLayerCount = 6;
			vd.aspect = WGPUTextureAspect_All;
			fallback_cube_texture_view = wgpuTextureCreateView(fallback_cube_texture, &vd);
		}
	}

	// Create dummy samplers for BGL rebinding.
	// When a bind group must be re-created with a different BGL (Comparison↔Filtering),
	// these dummy samplers are substituted for the mismatched entries.
	{
		WGPUSamplerDescriptor sd = {};
		sd.addressModeU = WGPUAddressMode_ClampToEdge;
		sd.addressModeV = WGPUAddressMode_ClampToEdge;
		sd.addressModeW = WGPUAddressMode_ClampToEdge;
		sd.magFilter = WGPUFilterMode_Linear;
		sd.minFilter = WGPUFilterMode_Linear;
		sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
		sd.maxAnisotropy = 1;
		dummy_filtering_sampler = wgpuDeviceCreateSampler(device, &sd);

		WGPUSamplerDescriptor csd = {};
		csd.addressModeU = WGPUAddressMode_ClampToEdge;
		csd.addressModeV = WGPUAddressMode_ClampToEdge;
		csd.addressModeW = WGPUAddressMode_ClampToEdge;
		csd.magFilter = WGPUFilterMode_Linear;
		csd.minFilter = WGPUFilterMode_Linear;
		csd.mipmapFilter = WGPUMipmapFilterMode_Linear;
		csd.maxAnisotropy = 1;
		csd.compare = WGPUCompareFunction_Less;
		dummy_comparison_sampler = wgpuDeviceCreateSampler(device, &csd);
	}

	// Create aliasing stub buffer — substituted for duplicate writable storage buffer bindings
	// in the same uniform set. WebGPU forbids two writable storage bindings that alias the same
	// underlying buffer (Vulkan allows it via barriers). When Godot passes the same buffer for
	// both SourceEmission and DestEmission in the particle compute shader, we redirect the
	// second occurrence to this dummy buffer so the bind group passes validation.
	{
		WGPUBufferDescriptor stub_desc = {};
		stub_desc.size = ALIASING_STUB_BUFFER_SIZE;
		stub_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		aliasing_stub_buffer = wgpuDeviceCreateBuffer(device, &stub_desc);
	}

	// Create shader container format.
	shader_container_format = memnew(RenderingShaderContainerFormatWebGPU);

	return OK;
}

void RenderingDeviceDriverWebGPU::_check_capabilities() {
	capabilities.device_family = DEVICE_UNKNOWN;
	capabilities.version_major = 1;
	capabilities.version_minor = 0;

	// Query actual device limits.
	device_limits = WGPU_LIMITS_INIT;
	WGPUStatus status = wgpuDeviceGetLimits(device, &device_limits);
	if (status != WGPUStatus_Success) {
		WARN_PRINT("WebGPU: Failed to query device limits, using spec minimums.");
	}

	// Check for timestamp query support.
	timestamp_supported = wgpuDeviceHasFeature(device, WGPUFeatureName_TimestampQuery);
	if (timestamp_supported) {
		print_verbose("WebGPU: Timestamp query feature is available.");
	}

	// 16-bit SNORM/UNORM texture formats (texture-formats-tier1) are not available
	// in the base emdawnwebgpu 4.0.10 API. Mark as unavailable — these formats are
	// mapped to Undefined in pixel_formats_webgpu.h.
	has_texture_formats_tier1 = false;

	// Multiview not supported in WebGPU.
	multiview_capabilities.is_supported = false;

	// Fragment shading rate not supported.
	fsr_capabilities = {};

	// Fragment density map not supported.
	fdm_capabilities = {};
}

// =============================================================================
// BUFFERS
// =============================================================================

RDD::BufferID RenderingDeviceDriverWebGPU::buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) {
	WGBuffer *buf = new WGBuffer();

	// WebGPU buffer sizes must be a multiple of 4.
	uint64_t aligned_size = (p_size + 3) & ~3ULL;

	buf->usage = _buffer_usage_to_wgpu(p_usage);
	buf->usage |= WGPUBufferUsage_CopyDst; // Always allow writes.
	buf->size = aligned_size;

	// Don't add MapRead to generic CPU buffers — it conflicts with CopySrc.
	// Compute readback uses buffer_get_data_direct() which creates its own
	// staging buffer with the correct CopyDst|MapRead usage.

	WGPUBufferDescriptor desc = {};
	desc.size = aligned_size;
	desc.usage = buf->usage;
	desc.mappedAtCreation = false;

	buf->handle = wgpuDeviceCreateBuffer(device, &desc);
	ERR_FAIL_COND_V(buf->handle == nullptr, BufferID());

	return BufferID(buf);
}

bool RenderingDeviceDriverWebGPU::buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) {
	// WebGPU has no texel buffer views. Stub: store format, emulate later if needed.
	return true;
}

void RenderingDeviceDriverWebGPU::buffer_free(BufferID p_buffer) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL(buf);
	if (buf->handle) {
		wgpuBufferRelease(buf->handle);
	}
	if (buf->shadow_map) {
		memfree(buf->shadow_map);
	}
	delete buf;
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_allocation_size(BufferID p_buffer) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL_V(buf, 0);
	return buf->size;
}

// Callback for deferred buffer map readback (same signature as _timestamp_readback_callback).
// Copies GPU buffer data into the WGBuffer shadow_map when the async map resolves.
static void _buffer_deferred_map_cb(WGPUMapAsyncStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	WGBuffer *buf = (WGBuffer *)p_userdata1;
	if (!buf) return;

	if (p_status == WGPUMapAsyncStatus_Success) {
		const void *mapped = wgpuBufferGetConstMappedRange(buf->handle, 0, buf->size);
		if (mapped && buf->shadow_map) {
			memcpy(buf->shadow_map, mapped, buf->size);
		}
		wgpuBufferUnmap(buf->handle);
	}
	buf->map_complete = true;
}

// Callback for timestamp readback.
static void _buffer_readback_callback(WGPUMapAsyncStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	WGBuffer *buf = (WGBuffer *)p_userdata1;
	if (!buf) return;

	if (p_status == WGPUMapAsyncStatus_Success) {
		const void *mapped = wgpuBufferGetConstMappedRange(buf->handle, 0, buf->size);
		if (mapped) {
			if (!buf->shadow_map) {
				buf->shadow_map = (uint8_t *)memalloc(buf->size);
			}
			memcpy(buf->shadow_map, mapped, buf->size);
		}
		wgpuBufferUnmap(buf->handle);
	} else {
		WARN_PRINT("WebGPU buffer readback failed");
	}
	buf->map_complete = true;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_map(BufferID p_buffer) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL_V(buf, nullptr);

	// For readback buffers (staging buffers used by buffer_get_data), perform
	// async map to get actual GPU data.  This is critical for compute shader
	// readback, screenshot capture, and any CPU-side read of GPU results.
	//
	// The pattern: wgpuBufferMapAsync with AllowSpontaneous callback mode,
	// then wgpuQueueSubmit (empty) to flush, followed by polling via
	// wgpuBufferGetMapState until the map completes.
	if (buf->is_readback && buf->handle) {
		// Frame-deferred readback for WebGPU.
		// Buffer map callbacks fire when the JS event loop runs (between frames).
		// First call: initiate map, return zeros.
		// Subsequent calls: callback has fired, return real data.
		if (!buf->shadow_map) {
			buf->shadow_map = (uint8_t *)memalloc(buf->size);
			memset(buf->shadow_map, 0, buf->size);
		}

		if (buf->map_complete) {
			// Previous map completed — shadow has fresh data. Start next map.
			buf->map_complete = false;
			WGPUBufferMapCallbackInfo cb = {};
			cb.mode = WGPUCallbackMode_AllowSpontaneous;
			cb.callback = _buffer_deferred_map_cb;
			cb.userdata1 = buf;
			wgpuBufferMapAsync(buf->handle, WGPUMapMode_Read, 0, buf->size, cb);
			return buf->shadow_map;
		}

		// First call: initiate async map.
		WGPUBufferMapCallbackInfo cb = {};
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.callback = _buffer_deferred_map_cb;
		cb.userdata1 = buf;
		wgpuBufferMapAsync(buf->handle, WGPUMapMode_Read, 0, buf->size, cb);

		return buf->shadow_map;
	}

	// For non-readback buffers, use the shadow CPU buffer (for upload staging).
	if (!buf->shadow_map) {
		buf->shadow_map = (uint8_t *)memalloc(buf->size);
		memset(buf->shadow_map, 0, buf->size);
	}
	return buf->shadow_map;
}

void RenderingDeviceDriverWebGPU::buffer_unmap(BufferID p_buffer) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL(buf);
	if (buf->shadow_map && buf->map_dirty) {
		wgpuQueueWriteBuffer(queue, buf->handle, 0, buf->shadow_map, buf->size);
		buf->map_dirty = false;
	}
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL_V(buf, nullptr);
	if (!buf->shadow_map) {
		buf->shadow_map = (uint8_t *)memalloc(buf->size);
		memset(buf->shadow_map, 0, buf->size);
	}
	return buf->shadow_map;
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_dynamic_offsets(Span<BufferID> p_buffers) {
	return 0;
}

void RenderingDeviceDriverWebGPU::buffer_flush(BufferID p_buffer) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	if (buf && buf->shadow_map) {
		wgpuQueueWriteBuffer(queue, buf->handle, 0, buf->shadow_map, buf->size);
	}
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_device_address(BufferID p_buffer) {
	return 0; // No device addresses in WebGPU.
}

// =============================================================================
// ASYNC BUFFER READBACK (WebGPU-specific)
// =============================================================================

void RenderingDeviceDriverWebGPU::_readback_map_cb(WGPUMapAsyncStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	ReadbackEntry *entry = (ReadbackEntry *)p_userdata1;
	if (!entry || !entry->staging) return;

	if (p_status == WGPUMapAsyncStatus_Success) {
		const void *mapped = wgpuBufferGetConstMappedRange(entry->staging, 0, entry->size);
		if (mapped && entry->shadow) {
			memcpy(entry->shadow, mapped, entry->size);
			entry->has_data = true;
		}
		wgpuBufferUnmap(entry->staging);
	}
	entry->map_complete = true;
}

bool RenderingDeviceDriverWebGPU::buffer_get_data_direct(BufferID p_buffer, uint64_t p_offset, uint64_t p_size, Vector<uint8_t> &r_data) {
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL_V(buf, false);

	uint64_t key = (uint64_t)(uintptr_t)buf;
	ReadbackEntry *entry = nullptr;

	if (_readback_cache.has(key)) {
		entry = &_readback_cache[key];
	}

	// If we have completed readback data from a previous frame, return it.
	if (entry && entry->has_data && entry->map_complete) {
		r_data.resize(p_size);
		memcpy(r_data.ptrw(), entry->shadow + p_offset, p_size);

		// Initiate a new readback for this frame's data.
		entry->map_complete = false;
		// Copy GPU buffer → persistent staging buffer.
		WGPUCommandEncoderDescriptor enc_desc = {};
		WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, &enc_desc);
		wgpuCommandEncoderCopyBufferToBuffer(encoder, buf->handle, 0, entry->staging, 0, entry->size);
		WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, nullptr);
		wgpuQueueSubmit(queue, 1, &cmd);
		wgpuCommandBufferRelease(cmd);
		wgpuCommandEncoderRelease(encoder);

		// Initiate async map for next readback.
		WGPUBufferMapCallbackInfo cb = {};
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.callback = _readback_map_cb;
		cb.userdata1 = entry;
		wgpuBufferMapAsync(entry->staging, WGPUMapMode_Read, 0, entry->size, cb);

		return true;
	}

	// First call for this buffer, or readback not yet complete.
	// Create persistent staging buffer and initiate first readback.
	if (!entry) {
		ReadbackEntry new_entry;
		new_entry.size = buf->size;
		new_entry.shadow = (uint8_t *)memalloc(buf->size);
		memset(new_entry.shadow, 0, buf->size);

		// Create persistent staging buffer with CopyDst + MapRead.
		WGPUBufferDescriptor desc = {};
		desc.size = (buf->size + 3) & ~3ULL;
		desc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
		new_entry.staging = wgpuDeviceCreateBuffer(device, &desc);
		new_entry.map_complete = false;
		new_entry.has_data = false;

		_readback_cache[key] = new_entry;
		entry = &_readback_cache[key];
	}

	if (!entry->map_complete) {
		// Copy GPU buffer → persistent staging buffer.
		WGPUCommandEncoderDescriptor enc_desc = {};
		WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, &enc_desc);
		wgpuCommandEncoderCopyBufferToBuffer(encoder, buf->handle, 0, entry->staging, 0, entry->size);
		WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, nullptr);
		wgpuQueueSubmit(queue, 1, &cmd);
		wgpuCommandBufferRelease(cmd);
		wgpuCommandEncoderRelease(encoder);

		// Initiate async map.
		WGPUBufferMapCallbackInfo cb = {};
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.callback = _readback_map_cb;
		cb.userdata1 = entry;
		wgpuBufferMapAsync(entry->staging, WGPUMapMode_Read, 0, entry->size, cb);
	}

	// Return zeros (first call) or previous frame's data.
	r_data.resize(p_size);
	if (entry->has_data) {
		memcpy(r_data.ptrw(), entry->shadow + p_offset, p_size);
	} else {
		memset(r_data.ptrw(), 0, p_size);
	}
	return true; // Handled — don't use the default staging path.
}

WGPUBufferUsage RenderingDeviceDriverWebGPU::_buffer_usage_to_wgpu(BitField<BufferUsageBits> p_usage) const {
	WGPUBufferUsage flags = 0;
	if (p_usage.has_flag(BUFFER_USAGE_TRANSFER_FROM_BIT)) {
		flags |= WGPUBufferUsage_CopySrc;
	}
	if (p_usage.has_flag(BUFFER_USAGE_TRANSFER_TO_BIT)) {
		flags |= WGPUBufferUsage_CopyDst;
	}
	if (p_usage.has_flag(BUFFER_USAGE_UNIFORM_BIT)) {
		flags |= WGPUBufferUsage_Uniform;
	}
	if (p_usage.has_flag(BUFFER_USAGE_STORAGE_BIT) || p_usage.has_flag(BUFFER_USAGE_TEXEL_BIT)) {
		flags |= WGPUBufferUsage_Storage;
	}
	if (p_usage.has_flag(BUFFER_USAGE_INDEX_BIT)) {
		flags |= WGPUBufferUsage_Index;
	}
	if (p_usage.has_flag(BUFFER_USAGE_VERTEX_BIT)) {
		flags |= WGPUBufferUsage_Vertex;
	}
	if (p_usage.has_flag(BUFFER_USAGE_INDIRECT_BIT)) {
		flags |= WGPUBufferUsage_Indirect;
	}
	return flags;
}

// =============================================================================
// TEXTURES
// =============================================================================

// Returns the sRGB counterpart format for view compatibility, or Undefined if none.
static WGPUTextureFormat _get_srgb_view_format(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_RGBA8Unorm: return WGPUTextureFormat_RGBA8UnormSrgb;
		case WGPUTextureFormat_RGBA8UnormSrgb: return WGPUTextureFormat_RGBA8Unorm;
		case WGPUTextureFormat_BGRA8Unorm: return WGPUTextureFormat_BGRA8UnormSrgb;
		case WGPUTextureFormat_BGRA8UnormSrgb: return WGPUTextureFormat_BGRA8Unorm;
		case WGPUTextureFormat_BC1RGBAUnorm: return WGPUTextureFormat_BC1RGBAUnormSrgb;
		case WGPUTextureFormat_BC1RGBAUnormSrgb: return WGPUTextureFormat_BC1RGBAUnorm;
		case WGPUTextureFormat_BC2RGBAUnorm: return WGPUTextureFormat_BC2RGBAUnormSrgb;
		case WGPUTextureFormat_BC2RGBAUnormSrgb: return WGPUTextureFormat_BC2RGBAUnorm;
		case WGPUTextureFormat_BC3RGBAUnorm: return WGPUTextureFormat_BC3RGBAUnormSrgb;
		case WGPUTextureFormat_BC3RGBAUnormSrgb: return WGPUTextureFormat_BC3RGBAUnorm;
		case WGPUTextureFormat_ETC2RGB8Unorm: return WGPUTextureFormat_ETC2RGB8UnormSrgb;
		case WGPUTextureFormat_ETC2RGB8UnormSrgb: return WGPUTextureFormat_ETC2RGB8Unorm;
		case WGPUTextureFormat_ETC2RGB8A1Unorm: return WGPUTextureFormat_ETC2RGB8A1UnormSrgb;
		case WGPUTextureFormat_ETC2RGB8A1UnormSrgb: return WGPUTextureFormat_ETC2RGB8A1Unorm;
		case WGPUTextureFormat_ETC2RGBA8Unorm: return WGPUTextureFormat_ETC2RGBA8UnormSrgb;
		case WGPUTextureFormat_ETC2RGBA8UnormSrgb: return WGPUTextureFormat_ETC2RGBA8Unorm;
		default: return WGPUTextureFormat_Undefined;
	}
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create(const TextureFormat &p_format, const TextureView &p_view) {
	WGTexture *tex = new WGTexture();

	tex->format = _data_format_to_wgpu(p_format.format);
	tex->dimension = _texture_type_to_dimension(p_format.texture_type);
	tex->view_dimension = _texture_type_to_view_dimension(p_format.texture_type);
	tex->width = p_format.width;
	tex->height = p_format.height;
	tex->depth = p_format.depth;
	tex->mipmaps = p_format.mipmaps;
	tex->layers = p_format.array_layers;
	tex->sample_count = 1 << p_format.samples;
	tex->usage = _texture_usage_to_wgpu(p_format.usage_bits);

	// WebGPU does not support R8/RG8/R16/RG16 as storage texel formats.
	// Upgrade to 32-bit equivalents when storage binding is needed.
	if (tex->usage & WGPUTextureUsage_StorageBinding) {
		switch (tex->format) {
			case WGPUTextureFormat_R8Unorm:
			case WGPUTextureFormat_R8Snorm:
			case WGPUTextureFormat_R16Float:
			// R16Snorm/R16Unorm not in base emdawnwebgpu 4.0.10 headers
				tex->format = WGPUTextureFormat_R32Float;
				break;
			case WGPUTextureFormat_R8Uint:
			case WGPUTextureFormat_R16Uint:
				tex->format = WGPUTextureFormat_R32Uint;
				break;
			case WGPUTextureFormat_R8Sint:
			case WGPUTextureFormat_R16Sint:
				tex->format = WGPUTextureFormat_R32Sint;
				break;
			case WGPUTextureFormat_RG8Unorm:
			case WGPUTextureFormat_RG8Snorm:
			case WGPUTextureFormat_RG16Float:
			// RG16Snorm/RG16Unorm not in base emdawnwebgpu 4.0.10 headers
				tex->format = WGPUTextureFormat_RG32Float;
				break;
			case WGPUTextureFormat_RG8Uint:
			case WGPUTextureFormat_RG16Uint:
				tex->format = WGPUTextureFormat_RG32Uint;
				break;
			case WGPUTextureFormat_RG8Sint:
			case WGPUTextureFormat_RG16Sint:
				tex->format = WGPUTextureFormat_RG32Sint;
				break;
			// RGBA16Snorm/RGBA16Unorm not in base emdawnwebgpu 4.0.10 headers
			default:
				break;
		}
	}

	WGPUTextureDescriptor desc = {};
	desc.dimension = tex->dimension;
	desc.format = tex->format;
	desc.size.width = tex->width;
	desc.size.height = tex->height;
	desc.size.depthOrArrayLayers = (tex->dimension == WGPUTextureDimension_3D) ? tex->depth : tex->layers;
	desc.mipLevelCount = tex->mipmaps;
	desc.sampleCount = tex->sample_count;
	desc.usage = tex->usage;

	// Allow reinterpretation between sRGB and linear variants via texture views.
	// However, sRGB viewFormats are incompatible with StorageBinding in WebGPU,
	// so skip adding them for storage textures.
	WGPUTextureFormat srgb_compat = _get_srgb_view_format(tex->format);
	if (srgb_compat != WGPUTextureFormat_Undefined && !(tex->usage & WGPUTextureUsage_StorageBinding)) {
		desc.viewFormatCount = 1;
		desc.viewFormats = &srgb_compat;
	}

	tex->handle = wgpuDeviceCreateTexture(device, &desc);
	ERR_FAIL_COND_V(tex->handle == nullptr, TextureID());
	tex->view_source = tex->handle; // Always the owning WGPUTexture; inherited by shared/sliced textures.

	// Create default view.
	WGPUTextureViewDescriptor view_desc = {};
	view_desc.format = tex->format;
	view_desc.dimension = tex->view_dimension;
	view_desc.baseMipLevel = 0;
	view_desc.mipLevelCount = tex->mipmaps;
	view_desc.baseArrayLayer = 0;
	view_desc.arrayLayerCount = tex->layers;
	view_desc.aspect = WGPUTextureAspect_All;

	tex->default_view = wgpuTextureCreateView(tex->handle, &view_desc);

	return TextureID(tex);
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) {
	// Not supported on web platform.
	ERR_FAIL_V_MSG(TextureID(), "WebGPU: texture_create_from_extension not supported.");
}

// Returns true if the format is an sRGB variant.
static bool _is_srgb_format(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_RGBA8UnormSrgb:
		case WGPUTextureFormat_BGRA8UnormSrgb:
		case WGPUTextureFormat_BC1RGBAUnormSrgb:
		case WGPUTextureFormat_BC2RGBAUnormSrgb:
		case WGPUTextureFormat_BC3RGBAUnormSrgb:
		case WGPUTextureFormat_ETC2RGB8UnormSrgb:
		case WGPUTextureFormat_ETC2RGB8A1UnormSrgb:
		case WGPUTextureFormat_ETC2RGBA8UnormSrgb:
			return true;
		default:
			return false;
	}
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create_shared(TextureID p_original_texture, const TextureView &p_view) {
	WGTexture *orig = (WGTexture *)(p_original_texture.id);
	ERR_FAIL_NULL_V(orig, TextureID());

	WGTexture *tex = new WGTexture();
	*tex = *orig; // Copy base properties.

	// Create a new view with potentially different format.
	WGPUTextureViewDescriptor view_desc = {};
	if (p_view.format != DATA_FORMAT_MAX) {
		view_desc.format = _data_format_to_wgpu(p_view.format);
		tex->format = view_desc.format;
	} else {
		view_desc.format = orig->format;
	}
	view_desc.dimension = tex->view_dimension;
	view_desc.baseMipLevel = 0;
	view_desc.mipLevelCount = tex->mipmaps;
	view_desc.baseArrayLayer = 0;
	view_desc.arrayLayerCount = tex->layers;
	view_desc.aspect = WGPUTextureAspect_All;

	// sRGB formats are incompatible with StorageBinding in WebGPU.
	// If creating an sRGB view of a storage texture, fall back to linear format.
	if (_is_srgb_format(view_desc.format) && (orig->usage & WGPUTextureUsage_StorageBinding)) {
		view_desc.format = orig->format;
		tex->format = orig->format;
	}

	// view_source was already inherited from orig via *tex = *orig.
	ERR_FAIL_COND_V_MSG(tex->view_source == nullptr, TextureID(), "WebGPU: texture_create_shared: original texture has no GPU handle (view_source is null).");
	tex->default_view = wgpuTextureCreateView(tex->view_source, &view_desc);
	tex->handle = nullptr; // Shared texture does not own the WGPUTexture.

	return TextureID(tex);
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) {
	WGTexture *orig = (WGTexture *)(p_original_texture.id);
	ERR_FAIL_NULL_V(orig, TextureID());

	WGTexture *tex = new WGTexture();
	*tex = *orig;

	WGPUTextureViewDescriptor view_desc = {};
	view_desc.format = (p_view.format != DATA_FORMAT_MAX) ? _data_format_to_wgpu(p_view.format) : orig->format;
	view_desc.baseMipLevel = p_mipmap;
	view_desc.mipLevelCount = p_mipmaps;
	view_desc.baseArrayLayer = p_layer;
	view_desc.arrayLayerCount = p_layers;
	view_desc.aspect = WGPUTextureAspect_All;

	switch (p_slice_type) {
		case TEXTURE_SLICE_2D:
			view_desc.dimension = WGPUTextureViewDimension_2D;
			break;
		case TEXTURE_SLICE_CUBEMAP:
			view_desc.dimension = WGPUTextureViewDimension_Cube;
			break;
		case TEXTURE_SLICE_3D:
			view_desc.dimension = WGPUTextureViewDimension_3D;
			break;
		case TEXTURE_SLICE_2D_ARRAY:
			view_desc.dimension = WGPUTextureViewDimension_2DArray;
			break;
		default:
			view_desc.dimension = orig->view_dimension;
			break;
	}

	// view_source was already inherited from orig via *tex = *orig.
	ERR_FAIL_COND_V_MSG(tex->view_source == nullptr, TextureID(), "WebGPU: texture_create_shared_from_slice: original texture has no GPU handle (view_source is null).");

	// sRGB formats are incompatible with StorageBinding in WebGPU.
	// Fall back to the parent's linear format if sRGB is requested on a storage texture.
	if (_is_srgb_format(view_desc.format) && (orig->usage & WGPUTextureUsage_StorageBinding)) {
		view_desc.format = orig->format;
		tex->format = orig->format;
	}

	tex->default_view = wgpuTextureCreateView(tex->view_source, &view_desc);
	tex->handle = nullptr;
	tex->layers = p_layers;
	tex->mipmaps = p_mipmaps;

	return TextureID(tex);
}

void RenderingDeviceDriverWebGPU::texture_free(TextureID p_texture) {
	WGTexture *tex = (WGTexture *)(p_texture.id);
	ERR_FAIL_NULL(tex);
	if (tex->default_view) {
		wgpuTextureViewRelease(tex->default_view);
	}
	if (tex->handle && !tex->is_from_swap_chain) {
		wgpuTextureRelease(tex->handle);
	}
	delete tex;
}

uint64_t RenderingDeviceDriverWebGPU::texture_get_allocation_size(TextureID p_texture) {
	WGTexture *tex = (WGTexture *)(p_texture.id);
	ERR_FAIL_NULL_V(tex, 0);
	// Approximate: width * height * depth * layers * bpp * mipmaps.
	// TODO: Use proper bytes_per_pixel for the format.
	uint64_t bpp = 4; // Assume 4 bytes per pixel for now.
	return tex->width * tex->height * tex->depth * tex->layers * bpp;
}

void RenderingDeviceDriverWebGPU::texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) {
	WGTexture *tex = (WGTexture *)(p_texture.id);
	ERR_FAIL_NULL(tex);
	ERR_FAIL_NULL(r_layout);

	uint32_t bpp = 4; // TODO: Get from format.
	uint32_t mip_width = MAX(1u, tex->width >> p_subresource.mipmap);
	uint32_t mip_height = MAX(1u, tex->height >> p_subresource.mipmap);

	// WebGPU requires 256-byte row alignment for buffer <-> texture copies.
	r_layout->row_pitch = ((mip_width * bpp + 255) / 256) * 256;
	r_layout->size = r_layout->row_pitch * mip_height;
}

Vector<uint8_t> RenderingDeviceDriverWebGPU::texture_get_data(TextureID p_texture, uint32_t p_layer) {
	// TODO: Implement async texture readback (copy texture → staging buffer → map → read).
	WARN_PRINT_ONCE("WebGPU: texture_get_data not yet implemented.");
	return Vector<uint8_t>();
}

BitField<RDD::TextureUsageBits> RenderingDeviceDriverWebGPU::texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) {
	// If there's no WebGPU equivalent, the format is unsupported.
	if (_data_format_to_wgpu(p_format) == WGPUTextureFormat_Undefined) {
		return 0;
	}

	// These bits apply to every supported format.
	BitField<TextureUsageBits> flags = TEXTURE_USAGE_SAMPLING_BIT | TEXTURE_USAGE_CAN_UPDATE_BIT | TEXTURE_USAGE_CAN_COPY_FROM_BIT | TEXTURE_USAGE_CAN_COPY_TO_BIT;

	// Classify the format into depth/stencil, compressed, or plain color.
	bool is_depth_stencil = false;
	bool is_compressed = false;

	switch (p_format) {
		// Depth and depth/stencil formats.
		case DATA_FORMAT_D16_UNORM:
		case DATA_FORMAT_X8_D24_UNORM_PACK32:
		case DATA_FORMAT_D32_SFLOAT:
		case DATA_FORMAT_S8_UINT:
		case DATA_FORMAT_D16_UNORM_S8_UINT:
		case DATA_FORMAT_D24_UNORM_S8_UINT:
		case DATA_FORMAT_D32_SFLOAT_S8_UINT:
			is_depth_stencil = true;
			break;

		// BC compressed formats.
		case DATA_FORMAT_BC1_RGB_UNORM_BLOCK:
		case DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
		case DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
		case DATA_FORMAT_BC2_UNORM_BLOCK:
		case DATA_FORMAT_BC2_SRGB_BLOCK:
		case DATA_FORMAT_BC3_UNORM_BLOCK:
		case DATA_FORMAT_BC3_SRGB_BLOCK:
		case DATA_FORMAT_BC4_UNORM_BLOCK:
		case DATA_FORMAT_BC4_SNORM_BLOCK:
		case DATA_FORMAT_BC5_UNORM_BLOCK:
		case DATA_FORMAT_BC5_SNORM_BLOCK:
		case DATA_FORMAT_BC6H_UFLOAT_BLOCK:
		case DATA_FORMAT_BC6H_SFLOAT_BLOCK:
		case DATA_FORMAT_BC7_UNORM_BLOCK:
		case DATA_FORMAT_BC7_SRGB_BLOCK:
		// ETC2 compressed formats.
		case DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
		case DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
		case DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
		case DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
		case DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
		case DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
		case DATA_FORMAT_EAC_R11_UNORM_BLOCK:
		case DATA_FORMAT_EAC_R11_SNORM_BLOCK:
		case DATA_FORMAT_EAC_R11G11_UNORM_BLOCK:
		case DATA_FORMAT_EAC_R11G11_SNORM_BLOCK:
		// ASTC compressed formats.
		case DATA_FORMAT_ASTC_4x4_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_4x4_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_5x4_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_5x4_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_5x5_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_5x5_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_6x5_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_6x5_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_6x6_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_6x6_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_8x5_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_8x5_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_8x6_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_8x6_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_8x8_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_8x8_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_10x5_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_10x5_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_10x6_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_10x6_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_10x8_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_10x8_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_10x10_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_10x10_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_12x10_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_12x10_SRGB_BLOCK:
		case DATA_FORMAT_ASTC_12x12_UNORM_BLOCK:
		case DATA_FORMAT_ASTC_12x12_SRGB_BLOCK:
			is_compressed = true;
			break;

		default:
			break;
	}

	if (is_depth_stencil) {
		flags.set_flag(TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
		// Depth textures can't be updated via CAN_UPDATE (CopyDst is not allowed).
		flags.clear_flag(TEXTURE_USAGE_CAN_UPDATE_BIT);
		return flags;
	}

	if (is_compressed) {
		// Compressed textures: sampling + copy only. No render attachment, no storage.
		flags.clear_flag(TEXTURE_USAGE_CAN_UPDATE_BIT); // Must use CAN_COPY_TO to upload.
		return flags;
	}

	// Plain color format: render attachment is generally supported.
	flags.set_flag(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);

	// Storage binding: only formats explicitly listed in the WebGPU spec support it.
	// See https://gpuweb.github.io/gpuweb/#storage-texel-format-capability-matrix
	// Formats not natively supported (R8, RG8, R16, RG16) are included here because
	// Godot's renderer uses them with storage; they are silently upgraded to 32-bit
	// equivalents at WGPUTexture creation time and in WGSL text.
	switch (p_format) {
		case DATA_FORMAT_R8_UNORM:
		case DATA_FORMAT_R8_SNORM:
		case DATA_FORMAT_R8G8_UNORM:
		case DATA_FORMAT_R8G8_SNORM:
		case DATA_FORMAT_R8G8B8A8_UNORM:
		case DATA_FORMAT_R8G8B8A8_SNORM:
		case DATA_FORMAT_R8G8B8A8_UINT:
		case DATA_FORMAT_R8G8B8A8_SINT:
		case DATA_FORMAT_R16_SFLOAT:
		case DATA_FORMAT_R16_SNORM:
		case DATA_FORMAT_R16_UNORM:
		case DATA_FORMAT_R16G16_SFLOAT:
		case DATA_FORMAT_R16G16_SNORM:
		case DATA_FORMAT_R16G16_UNORM:
		case DATA_FORMAT_R16G16_SINT:
		case DATA_FORMAT_R16G16_UINT:
		case DATA_FORMAT_R16G16B16A16_SFLOAT:
		case DATA_FORMAT_R16G16B16A16_UINT:
		case DATA_FORMAT_R16G16B16A16_SINT:
		case DATA_FORMAT_R32_SFLOAT:
		case DATA_FORMAT_R32_UINT:
		case DATA_FORMAT_R32_SINT:
		case DATA_FORMAT_R32G32_SFLOAT:
		case DATA_FORMAT_R32G32_UINT:
		case DATA_FORMAT_R32G32_SINT:
		case DATA_FORMAT_R32G32B32A32_SFLOAT:
		case DATA_FORMAT_R32G32B32A32_UINT:
		case DATA_FORMAT_R32G32B32A32_SINT:
			flags.set_flag(TEXTURE_USAGE_STORAGE_BIT);
			break;
		default:
			break;
	}

	return flags;
}

bool RenderingDeviceDriverWebGPU::texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) {
	r_raw_reinterpretation = false;
	// TODO: Check WebGPU view format compatibility rules.
	return true;
}

WGPUTextureUsage RenderingDeviceDriverWebGPU::_texture_usage_to_wgpu(BitField<TextureUsageBits> p_usage) const {
	WGPUTextureUsage flags = 0;
	if (p_usage.has_flag(TEXTURE_USAGE_SAMPLING_BIT)) {
		flags |= WGPUTextureUsage_TextureBinding;
	}
	if (p_usage.has_flag(TEXTURE_USAGE_COLOR_ATTACHMENT_BIT) || p_usage.has_flag(TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
		flags |= WGPUTextureUsage_RenderAttachment;
	}
	if (p_usage.has_flag(TEXTURE_USAGE_STORAGE_BIT) || p_usage.has_flag(TEXTURE_USAGE_STORAGE_ATOMIC_BIT)) {
		flags |= WGPUTextureUsage_StorageBinding;
	}
	if (p_usage.has_flag(TEXTURE_USAGE_CPU_READ_BIT) || p_usage.has_flag(TEXTURE_USAGE_CAN_COPY_FROM_BIT)) {
		flags |= WGPUTextureUsage_CopySrc;
	}
	if (p_usage.has_flag(TEXTURE_USAGE_CAN_UPDATE_BIT) || p_usage.has_flag(TEXTURE_USAGE_CAN_COPY_TO_BIT)) {
		flags |= WGPUTextureUsage_CopyDst;
	}
	return flags;
}

WGPUTextureDimension RenderingDeviceDriverWebGPU::_texture_type_to_dimension(TextureType p_type) const {
	switch (p_type) {
		case TEXTURE_TYPE_1D:
			return WGPUTextureDimension_1D;
		case TEXTURE_TYPE_3D:
			return WGPUTextureDimension_3D;
		default:
			return WGPUTextureDimension_2D;
	}
}

WGPUTextureViewDimension RenderingDeviceDriverWebGPU::_texture_type_to_view_dimension(TextureType p_type) const {
	switch (p_type) {
		case TEXTURE_TYPE_1D:
			return WGPUTextureViewDimension_1D;
		case TEXTURE_TYPE_2D:
			return WGPUTextureViewDimension_2D;
		case TEXTURE_TYPE_3D:
			return WGPUTextureViewDimension_3D;
		case TEXTURE_TYPE_CUBE:
			return WGPUTextureViewDimension_Cube;
		case TEXTURE_TYPE_2D_ARRAY:
			return WGPUTextureViewDimension_2DArray;
		case TEXTURE_TYPE_CUBE_ARRAY:
			return WGPUTextureViewDimension_CubeArray;
		default:
			return WGPUTextureViewDimension_2D;
	}
}

WGPUTextureFormat RenderingDeviceDriverWebGPU::_data_format_to_wgpu(DataFormat p_format) const {
	// TODO: Move to pixel_formats_webgpu.cpp. See DESIGN.md Appendix B for full table.
	switch (p_format) {
		case DATA_FORMAT_R8_UNORM: return WGPUTextureFormat_R8Unorm;
		case DATA_FORMAT_R8_SNORM: return WGPUTextureFormat_R8Snorm;
		case DATA_FORMAT_R8_UINT: return WGPUTextureFormat_R8Uint;
		case DATA_FORMAT_R8_SINT: return WGPUTextureFormat_R8Sint;
		case DATA_FORMAT_R8G8_UNORM: return WGPUTextureFormat_RG8Unorm;
		case DATA_FORMAT_R8G8_SNORM: return WGPUTextureFormat_RG8Snorm;
		case DATA_FORMAT_R8G8_UINT: return WGPUTextureFormat_RG8Uint;
		case DATA_FORMAT_R8G8_SINT: return WGPUTextureFormat_RG8Sint;
		case DATA_FORMAT_R8G8B8A8_UNORM: return WGPUTextureFormat_RGBA8Unorm;
		case DATA_FORMAT_R8G8B8A8_SNORM: return WGPUTextureFormat_RGBA8Snorm;
		case DATA_FORMAT_R8G8B8A8_UINT: return WGPUTextureFormat_RGBA8Uint;
		case DATA_FORMAT_R8G8B8A8_SINT: return WGPUTextureFormat_RGBA8Sint;
		case DATA_FORMAT_R8G8B8A8_SRGB: return WGPUTextureFormat_RGBA8UnormSrgb;
		case DATA_FORMAT_B8G8R8A8_UNORM: return WGPUTextureFormat_BGRA8Unorm;
		case DATA_FORMAT_B8G8R8A8_SRGB: return WGPUTextureFormat_BGRA8UnormSrgb;
		case DATA_FORMAT_R16_UNORM: return WGPUTextureFormat_R16Float /* R16Unorm N/A in emdawnwebgpu 4.0.10 */;
		case DATA_FORMAT_R16_SNORM: return WGPUTextureFormat_R16Float /* R16Snorm N/A in emdawnwebgpu 4.0.10 */;
		case DATA_FORMAT_R16_UINT: return WGPUTextureFormat_R16Uint;
		case DATA_FORMAT_R16_SINT: return WGPUTextureFormat_R16Sint;
		case DATA_FORMAT_R16_SFLOAT: return WGPUTextureFormat_R16Float;
		case DATA_FORMAT_R16G16_UNORM: return WGPUTextureFormat_RG16Float /* RG16Unorm N/A in emdawnwebgpu 4.0.10 */;
		case DATA_FORMAT_R16G16_SNORM: return WGPUTextureFormat_RG16Float /* RG16Snorm N/A in emdawnwebgpu 4.0.10 */;
		case DATA_FORMAT_R16G16_UINT: return WGPUTextureFormat_RG16Uint;
		case DATA_FORMAT_R16G16_SINT: return WGPUTextureFormat_RG16Sint;
		case DATA_FORMAT_R16G16_SFLOAT: return WGPUTextureFormat_RG16Float;
		case DATA_FORMAT_R16G16B16A16_UNORM: return WGPUTextureFormat_RGBA16Float /* RGBA16Unorm N/A in emdawnwebgpu 4.0.10 */;
		case DATA_FORMAT_R16G16B16A16_SNORM: return WGPUTextureFormat_RGBA16Float /* RGBA16Snorm N/A in emdawnwebgpu 4.0.10 */;
		case DATA_FORMAT_R16G16B16A16_UINT: return WGPUTextureFormat_RGBA16Uint;
		case DATA_FORMAT_R16G16B16A16_SINT: return WGPUTextureFormat_RGBA16Sint;
		case DATA_FORMAT_R16G16B16A16_SFLOAT: return WGPUTextureFormat_RGBA16Float;
		case DATA_FORMAT_R32_UINT: return WGPUTextureFormat_R32Uint;
		case DATA_FORMAT_R32_SINT: return WGPUTextureFormat_R32Sint;
		case DATA_FORMAT_R32_SFLOAT: return WGPUTextureFormat_R32Float;
		case DATA_FORMAT_R32G32_UINT: return WGPUTextureFormat_RG32Uint;
		case DATA_FORMAT_R32G32_SINT: return WGPUTextureFormat_RG32Sint;
		case DATA_FORMAT_R32G32_SFLOAT: return WGPUTextureFormat_RG32Float;
		case DATA_FORMAT_R32G32B32A32_UINT: return WGPUTextureFormat_RGBA32Uint;
		case DATA_FORMAT_R32G32B32A32_SINT: return WGPUTextureFormat_RGBA32Sint;
		case DATA_FORMAT_R32G32B32A32_SFLOAT: return WGPUTextureFormat_RGBA32Float;
		case DATA_FORMAT_A2B10G10R10_UNORM_PACK32: return WGPUTextureFormat_RGB10A2Unorm;
		case DATA_FORMAT_B10G11R11_UFLOAT_PACK32: return WGPUTextureFormat_RG11B10Ufloat;
		case DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32: return WGPUTextureFormat_RGB9E5Ufloat;
		case DATA_FORMAT_D16_UNORM: return WGPUTextureFormat_Depth16Unorm;
		case DATA_FORMAT_D32_SFLOAT: return WGPUTextureFormat_Depth32Float;
		case DATA_FORMAT_X8_D24_UNORM_PACK32: return WGPUTextureFormat_Depth24Plus;
		case DATA_FORMAT_D24_UNORM_S8_UINT: return WGPUTextureFormat_Depth24PlusStencil8;
		case DATA_FORMAT_D32_SFLOAT_S8_UINT: return WGPUTextureFormat_Depth32FloatStencil8;
		case DATA_FORMAT_S8_UINT: return WGPUTextureFormat_Stencil8;
		// No depth16+stencil8 in WebGPU; use depth24plus-stencil8 as nearest approximation.
		case DATA_FORMAT_D16_UNORM_S8_UINT: return WGPUTextureFormat_Depth24PlusStencil8;
		default:
			WARN_PRINT(vformat("WebGPU: Unsupported DataFormat %d", (int)p_format));
			return WGPUTextureFormat_Undefined;
	}
}

RDD::DataFormat RenderingDeviceDriverWebGPU::_wgpu_to_data_format(WGPUTextureFormat p_format) const {
	// TODO: Full reverse mapping. For now, handle common cases.
	switch (p_format) {
		case WGPUTextureFormat_BGRA8Unorm: return DATA_FORMAT_B8G8R8A8_UNORM;
		case WGPUTextureFormat_RGBA8Unorm: return DATA_FORMAT_R8G8B8A8_UNORM;
		default: return DATA_FORMAT_MAX;
	}
}

WGPUVertexFormat RenderingDeviceDriverWebGPU::_data_format_to_wgpu_vertex(DataFormat p_format) {
	switch (p_format) {
		case DATA_FORMAT_R32_SFLOAT: return WGPUVertexFormat_Float32;
		case DATA_FORMAT_R32G32_SFLOAT: return WGPUVertexFormat_Float32x2;
		case DATA_FORMAT_R32G32B32_SFLOAT: return WGPUVertexFormat_Float32x3;
		case DATA_FORMAT_R32G32B32A32_SFLOAT: return WGPUVertexFormat_Float32x4;
		case DATA_FORMAT_R32_UINT: return WGPUVertexFormat_Uint32;
		case DATA_FORMAT_R32G32_UINT: return WGPUVertexFormat_Uint32x2;
		case DATA_FORMAT_R32G32B32_UINT: return WGPUVertexFormat_Uint32x3;
		case DATA_FORMAT_R32G32B32A32_UINT: return WGPUVertexFormat_Uint32x4;
		case DATA_FORMAT_R32_SINT: return WGPUVertexFormat_Sint32;
		case DATA_FORMAT_R32G32_SINT: return WGPUVertexFormat_Sint32x2;
		case DATA_FORMAT_R32G32B32_SINT: return WGPUVertexFormat_Sint32x3;
		case DATA_FORMAT_R32G32B32A32_SINT: return WGPUVertexFormat_Sint32x4;
		case DATA_FORMAT_R16G16_SFLOAT: return WGPUVertexFormat_Float16x2;
		case DATA_FORMAT_R16G16B16A16_SFLOAT: return WGPUVertexFormat_Float16x4;
		case DATA_FORMAT_R16G16_UINT: return WGPUVertexFormat_Uint16x2;
		case DATA_FORMAT_R16G16B16A16_UINT: return WGPUVertexFormat_Uint16x4;
		case DATA_FORMAT_R16G16_SINT: return WGPUVertexFormat_Sint16x2;
		case DATA_FORMAT_R16G16B16A16_SINT: return WGPUVertexFormat_Sint16x4;
		case DATA_FORMAT_R16G16_UNORM: return WGPUVertexFormat_Unorm16x2;
		case DATA_FORMAT_R16G16B16A16_UNORM: return WGPUVertexFormat_Unorm16x4;
		case DATA_FORMAT_R16G16_SNORM: return WGPUVertexFormat_Snorm16x2;
		case DATA_FORMAT_R16G16B16A16_SNORM: return WGPUVertexFormat_Snorm16x4;
		case DATA_FORMAT_R8G8_UNORM: return WGPUVertexFormat_Unorm8x2;
		case DATA_FORMAT_R8G8B8A8_UNORM: return WGPUVertexFormat_Unorm8x4;
		case DATA_FORMAT_R8G8_SNORM: return WGPUVertexFormat_Snorm8x2;
		case DATA_FORMAT_R8G8B8A8_SNORM: return WGPUVertexFormat_Snorm8x4;
		case DATA_FORMAT_R8G8_UINT: return WGPUVertexFormat_Uint8x2;
		case DATA_FORMAT_R8G8B8A8_UINT: return WGPUVertexFormat_Uint8x4;
		case DATA_FORMAT_R8G8_SINT: return WGPUVertexFormat_Sint8x2;
		case DATA_FORMAT_R8G8B8A8_SINT: return WGPUVertexFormat_Sint8x4;
		case DATA_FORMAT_A2B10G10R10_UNORM_PACK32: return WGPUVertexFormat_Unorm10_10_10_2;
		default:
			WARN_PRINT(vformat("WebGPU: Unsupported vertex DataFormat %d", (int)p_format));
			return (WGPUVertexFormat)0;
	}
}

// =============================================================================
// SAMPLERS
// =============================================================================

RDD::SamplerID RenderingDeviceDriverWebGPU::sampler_create(const SamplerState &p_state) {
	WGPUSamplerDescriptor desc = {};

	auto map_filter = [](SamplerFilter f) -> WGPUFilterMode {
		return (f == SAMPLER_FILTER_LINEAR) ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	};
	auto map_address = [](SamplerRepeatMode m) -> WGPUAddressMode {
		switch (m) {
			case SAMPLER_REPEAT_MODE_REPEAT: return WGPUAddressMode_Repeat;
			case SAMPLER_REPEAT_MODE_MIRRORED_REPEAT: return WGPUAddressMode_MirrorRepeat;
			default: return WGPUAddressMode_ClampToEdge;
		}
	};
	auto map_compare = [](CompareOperator op) -> WGPUCompareFunction {
		switch (op) {
			case COMPARE_OP_NEVER: return WGPUCompareFunction_Never;
			case COMPARE_OP_LESS: return WGPUCompareFunction_Less;
			case COMPARE_OP_EQUAL: return WGPUCompareFunction_Equal;
			case COMPARE_OP_LESS_OR_EQUAL: return WGPUCompareFunction_LessEqual;
			case COMPARE_OP_GREATER: return WGPUCompareFunction_Greater;
			case COMPARE_OP_NOT_EQUAL: return WGPUCompareFunction_NotEqual;
			case COMPARE_OP_GREATER_OR_EQUAL: return WGPUCompareFunction_GreaterEqual;
			case COMPARE_OP_ALWAYS: return WGPUCompareFunction_Always;
			default: return WGPUCompareFunction_Undefined;
		}
	};

	desc.magFilter = map_filter(p_state.mag_filter);
	desc.minFilter = map_filter(p_state.min_filter);
	desc.mipmapFilter = (p_state.mip_filter == SAMPLER_FILTER_LINEAR) ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	desc.addressModeU = map_address(p_state.repeat_u);
	desc.addressModeV = map_address(p_state.repeat_v);
	desc.addressModeW = map_address(p_state.repeat_w);
	desc.lodMinClamp = p_state.min_lod;
	desc.lodMaxClamp = p_state.max_lod;

	if (p_state.enable_compare) {
		desc.compare = map_compare(p_state.compare_op);
	}

	if (p_state.use_anisotropy) {
		desc.maxAnisotropy = (uint16_t)p_state.anisotropy_max;
		// WebGPU requires minFilter, magFilter, and mipmapFilter to all be
		// Linear when maxAnisotropy > 1.
		if (desc.maxAnisotropy > 1) {
			desc.minFilter = WGPUFilterMode_Linear;
			desc.magFilter = WGPUFilterMode_Linear;
			desc.mipmapFilter = WGPUMipmapFilterMode_Linear;
		}
	} else {
		desc.maxAnisotropy = 1;
	}

	WGPUSampler sampler = wgpuDeviceCreateSampler(device, &desc);
	ERR_FAIL_COND_V(sampler == nullptr, SamplerID());

	// Store the WGPUSampler handle directly as the ID (no wrapper struct needed).
	return SamplerID((uint64_t)sampler);
}

void RenderingDeviceDriverWebGPU::sampler_free(SamplerID p_sampler) {
	WGPUSampler sampler = (WGPUSampler)(p_sampler.id);
	if (sampler) {
		wgpuSamplerRelease(sampler);
	}
}

bool RenderingDeviceDriverWebGPU::sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) {
	if (p_filter == SAMPLER_FILTER_NEAREST) {
		return true; // All formats support nearest filtering.
	}
	// TODO: Check if float32-filterable feature is available for R32Float, etc.
	// Integer formats don't support linear filtering.
	// Depth formats may or may not depending on implementation.
	return true;
}

// =============================================================================
// VERTEX FORMAT
// =============================================================================

RDD::VertexFormatID RenderingDeviceDriverWebGPU::vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) {
	WGVertexFormat *vf = new WGVertexFormat();

	// Build attribute list.
	for (uint32_t i = 0; i < p_vertex_attribs.size(); i++) {
		const VertexAttribute &va = p_vertex_attribs[i];
		WGVertexFormat::Attribute attr;
		attr.location = va.location;
		attr.format = _data_format_to_wgpu_vertex(va.format);
		attr.offset = va.offset;
		attr.binding = (va.binding == UINT32_MAX) ? i : va.binding;
		vf->attributes.push_back(attr);
	}

	// Build binding list from p_vertex_bindings map.
	if (!p_vertex_bindings.is_empty()) {
		uint32_t max_binding = 0;
		for (const KeyValue<uint32_t, VertexAttributeBinding> &kv : p_vertex_bindings) {
			max_binding = MAX(max_binding, kv.key);
		}
		vf->bindings.resize(max_binding + 1);
		for (const KeyValue<uint32_t, VertexAttributeBinding> &kv : p_vertex_bindings) {
			vf->bindings[kv.key].stride = kv.value.stride;
			vf->bindings[kv.key].step_mode = (kv.value.frequency == VERTEX_FREQUENCY_INSTANCE)
					? WGPUVertexStepMode_Instance
					: WGPUVertexStepMode_Vertex;
		}
	} else {
		// Build bindings from vertex attribute data (stride/frequency stored per-attribute in older API).
		HashMap<uint32_t, bool> seen;
		for (uint32_t i = 0; i < p_vertex_attribs.size(); i++) {
			const VertexAttribute &va = p_vertex_attribs[i];
			uint32_t binding = (va.binding == UINT32_MAX) ? i : va.binding;
			if (!seen.has(binding)) {
				if (vf->bindings.size() <= binding) {
					vf->bindings.resize(binding + 1);
				}
				vf->bindings[binding].stride = va.stride;
				vf->bindings[binding].step_mode = (va.frequency == VERTEX_FREQUENCY_INSTANCE)
						? WGPUVertexStepMode_Instance
						: WGPUVertexStepMode_Vertex;
				seen[binding] = true;
			}
		}
	}

	return VertexFormatID(vf);
}

void RenderingDeviceDriverWebGPU::vertex_format_free(VertexFormatID p_vertex_format) {
	WGVertexFormat *vf = (WGVertexFormat *)(p_vertex_format.id);
	delete vf;
}

// =============================================================================
// BARRIERS (ALL NO-OPS)
// =============================================================================

void RenderingDeviceDriverWebGPU::command_pipeline_barrier(
		CommandBufferID p_cmd_buffer,
		BitField<PipelineStageBits> p_src_stages,
		BitField<PipelineStageBits> p_dst_stages,
		VectorView<MemoryAccessBarrier> p_memory_barriers,
		VectorView<BufferBarrier> p_buffer_barriers,
		VectorView<TextureBarrier> p_texture_barriers) {
	// No-op: WebGPU handles synchronization automatically.
}

// =============================================================================
// FENCES
// =============================================================================

RDD::FenceID RenderingDeviceDriverWebGPU::fence_create() {
	WGFence *fence = new WGFence();
	return FenceID(fence);
}

Error RenderingDeviceDriverWebGPU::fence_wait(FenceID p_fence) {
	WGFence *fence = (WGFence *)(p_fence.id);
	ERR_FAIL_NULL_V(fence, ERR_INVALID_PARAMETER);

	// In the browser's single-threaded model, GPU work submitted via
	// wgpuQueueSubmit completes asynchronously.  However, the emdawnwebgpu
	// implementation resolves AllowSpontaneous callbacks during
	// wgpuInstanceProcessEvents.  Poll the instance to allow pending
	// callbacks (including buffer maps and work completion) to resolve.
	WGPUInstance inst = context_driver ? context_driver->get_instance() : nullptr;
	if (inst) {
		wgpuInstanceProcessEvents(inst);
	}
	fence->signaled = true;
	return OK;
}

void RenderingDeviceDriverWebGPU::fence_free(FenceID p_fence) {
	WGFence *fence = (WGFence *)(p_fence.id);
	delete fence;
}

// =============================================================================
// SEMAPHORES (NO-OPS - single queue)
// =============================================================================

RDD::SemaphoreID RenderingDeviceDriverWebGPU::semaphore_create() {
	WGSemaphore *sem = new WGSemaphore();
	return SemaphoreID(sem);
}

void RenderingDeviceDriverWebGPU::semaphore_free(SemaphoreID p_semaphore) {
	WGSemaphore *sem = (WGSemaphore *)(p_semaphore.id);
	delete sem;
}

// =============================================================================
// COMMAND BUFFERS
// =============================================================================

RDD::CommandQueueFamilyID RenderingDeviceDriverWebGPU::command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface) {
	return CommandQueueFamilyID(1); // Single family that supports everything.
}

RDD::CommandQueueID RenderingDeviceDriverWebGPU::command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue) {
	WGCommandQueue *cq = new WGCommandQueue();
	cq->queue = queue; // Share the single device queue.
	return CommandQueueID(cq);
}

Error RenderingDeviceDriverWebGPU::command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) {
	// Submit all command buffers.
	LocalVector<WGPUCommandBuffer> wgpu_cmd_buffers;
	for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
		WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffers[i].id);
		if (cmd && cmd->finished_buffer) {
			wgpu_cmd_buffers.push_back(cmd->finished_buffer);
		}
	}

	if (wgpu_cmd_buffers.size() > 0) {
		// Flush batched push constant data to GPU before submitting command buffers.
		if (push_constant_shadow_dirty_start < push_constant_shadow_dirty_end) {
			wgpuQueueWriteBuffer(queue, push_constant_ring_buffer, push_constant_shadow_dirty_start,
					push_constant_shadow + push_constant_shadow_dirty_start,
					push_constant_shadow_dirty_end - push_constant_shadow_dirty_start);
			push_constant_shadow_dirty_start = UINT32_MAX;
			push_constant_shadow_dirty_end = 0;
		}

		wgpuQueueSubmit(queue, wgpu_cmd_buffers.size(), wgpu_cmd_buffers.ptr());
		// Diagnostic: log submit count for the first few frames.
		static int _submit_log = 0;
		if (_submit_log < 10) {
			EM_ASM({ console.log('[DIAG-SUBMIT] frame=' + $0 + ' cmds=' + $1); },
					_submit_log, (int)wgpu_cmd_buffers.size());
			_submit_log++;
		}
	}

	// Signal fence if provided.
	if (p_cmd_fence) {
		WGFence *fence = (WGFence *)(p_cmd_fence.id);
		if (fence) {
			// TODO: Use wgpuQueueOnSubmittedWorkDone() callback to set fence->signaled = true.
			fence->signaled = true;
		}
	}

	// Present swap chains.
	for (uint32_t i = 0; i < p_swap_chains.size(); i++) {
		WGSwapChain *sc = (WGSwapChain *)(p_swap_chains[i].id);
		if (sc && sc->surface) {
#ifndef __EMSCRIPTEN__
			wgpuSurfacePresent(sc->surface);
#endif
			// Post-submit diagnostic clear DISABLED — only pre-submit CYAN is active.
			// This lets us see if Godot's submit overwrites the cyan.

			// Release the surface texture and view so the browser can composite the
			// frame within this requestAnimationFrame callback. If we hold onto the
			// texture reference until next frame's acquire, the browser won't present
			// the rendered content.
			if (sc->current_view) {
				wgpuTextureViewRelease(sc->current_view);
				sc->current_view = nullptr;
			}
			if (sc->current_texture) {
				wgpuTextureRelease(sc->current_texture);
				sc->current_texture = nullptr;
			}
		}
	}

	// Clear finished command buffers (they are consumed by submit).
	for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
		WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffers[i].id);
		if (cmd) {
			// Trigger async readback for any query pools that had timestamps resolved.
			for (uint32_t j = 0; j < cmd->written_query_pools.size(); j++) {
				WGQueryPool *pool = cmd->written_query_pools[j];
				if (pool->readback_pending && pool->readback_buffer) {
					WGPUBufferMapCallbackInfo cb_info = {};
					cb_info.mode = WGPUCallbackMode_AllowSpontaneous;
					cb_info.callback = _timestamp_readback_callback;
					cb_info.userdata1 = pool;
					cb_info.userdata2 = nullptr;
					wgpuBufferMapAsync(pool->readback_buffer, WGPUMapMode_Read, 0, sizeof(uint64_t) * pool->count, cb_info);
				}
			}
			cmd->written_query_pools.clear();
			cmd->finished_buffer = nullptr;
		}
	}

	return OK;
}

void RenderingDeviceDriverWebGPU::command_queue_free(CommandQueueID p_cmd_queue) {
	WGCommandQueue *cq = (WGCommandQueue *)(p_cmd_queue.id);
	delete cq;
}

RDD::CommandPoolID RenderingDeviceDriverWebGPU::command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) {
	WGCommandPool *pool = new WGCommandPool();
	pool->buffer_type = p_cmd_buffer_type;
	return CommandPoolID(pool);
}

bool RenderingDeviceDriverWebGPU::command_pool_reset(CommandPoolID p_cmd_pool) {
	// Nothing to reset — each command buffer creates its own encoder.
	return true;
}

void RenderingDeviceDriverWebGPU::command_pool_free(CommandPoolID p_cmd_pool) {
	WGCommandPool *pool = (WGCommandPool *)(p_cmd_pool.id);
	delete pool;
}

RDD::CommandBufferID RenderingDeviceDriverWebGPU::command_buffer_create(CommandPoolID p_cmd_pool) {
	WGCommandBuffer *cmd = new WGCommandBuffer();
	return CommandBufferID(cmd);
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin(CommandBufferID p_cmd_buffer) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL_V(cmd, false);

	WGPUCommandEncoderDescriptor desc = {};
	cmd->encoder = wgpuDeviceCreateCommandEncoder(device, &desc);
	ERR_FAIL_COND_V(cmd->encoder == nullptr, false);

	cmd->active_encoder = WGCommandBuffer::NONE;
	cmd->push_constants_dirty = false;
	cmd->push_constant_data_len = 0;

	return true;
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) {
	// WebGPU has no secondary command buffers. Treat as primary.
	return command_buffer_begin(p_cmd_buffer);
}

void RenderingDeviceDriverWebGPU::command_buffer_end(CommandBufferID p_cmd_buffer) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	// End any active render/compute pass.
	cmd->end_active_encoder();

	// Resolve any query pools that had timestamps written during this command buffer.
	for (uint32_t i = 0; i < cmd->written_query_pools.size(); i++) {
		WGQueryPool *pool = cmd->written_query_pools[i];
		wgpuCommandEncoderResolveQuerySet(cmd->encoder, pool->handle, 0, pool->count, pool->resolve_buffer, 0);
		uint64_t byte_size = sizeof(uint64_t) * pool->count;
		wgpuCommandEncoderCopyBufferToBuffer(cmd->encoder, pool->resolve_buffer, 0, pool->readback_buffer, 0, byte_size);
		pool->readback_pending = true;
	}

	// Finish the command encoder.
	if (cmd->encoder) {
		cmd->finished_buffer = wgpuCommandEncoderFinish(cmd->encoder, nullptr);
		wgpuCommandEncoderRelease(cmd->encoder);
		cmd->encoder = nullptr;
	}
}

void RenderingDeviceDriverWebGPU::command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) {
	// No-op: WebGPU has no secondary command buffers.
}

// =============================================================================
// SWAP CHAIN
// =============================================================================

RDD::SwapChainID RenderingDeviceDriverWebGPU::swap_chain_create(RenderingContextDriver::SurfaceID p_surface) {
	WGSwapChain *sc = new WGSwapChain();
	sc->surface = context_driver->surface_get_handle(p_surface);
	sc->surface_id = p_surface;
	sc->format = WGPUTextureFormat_BGRA8Unorm; // Standard format for browser canvas.

	// Create a render pass descriptor for this swap chain.
	// Used by swap_chain_get_render_pass() so the RD layer can create compatible pipelines.
	WGRenderPass *rp = new WGRenderPass();
	RDD::Attachment att;
	att.format = DATA_FORMAT_B8G8R8A8_UNORM;
	att.samples = TEXTURE_SAMPLES_1;
	att.load_op = ATTACHMENT_LOAD_OP_CLEAR;
	att.store_op = ATTACHMENT_STORE_OP_STORE;
	att.stencil_load_op = ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencil_store_op = ATTACHMENT_STORE_OP_DONT_CARE;
	att.initial_layout = TEXTURE_LAYOUT_UNDEFINED;
	att.final_layout = TEXTURE_LAYOUT_UNDEFINED;
	rp->attachments.push_back(att);

	WGRenderPass::SubpassInfo subpass;
	RDD::AttachmentReference color_ref;
	color_ref.attachment = 0;
	color_ref.layout = TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	subpass.color_references.push_back(color_ref);
	rp->subpasses.push_back(subpass);
	rp->is_swap_chain_pass = true;
	sc->render_pass = rp;

	return SwapChainID(sc);
}

Error RenderingDeviceDriverWebGPU::swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) {
	WGSwapChain *sc = (WGSwapChain *)(p_swap_chain.id);
	ERR_FAIL_NULL_V(sc, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(sc->surface == nullptr, ERR_INVALID_PARAMETER);

	uint32_t width = context_driver->surface_get_width(sc->surface_id);
	uint32_t height = context_driver->surface_get_height(sc->surface_id);
	if (width == 0 || height == 0) {
		return ERR_SKIP; // Canvas not yet sized.
	}

	// If previously configured, unconfigure first to reconfigure with new dimensions.
	if (sc->configured) {
		wgpuSurfaceUnconfigure(sc->surface);
		sc->configured = false;
	}

	WGPUSurfaceConfiguration config = {};
	config.device = device;
	config.format = sc->format;
	config.usage = WGPUTextureUsage_RenderAttachment;
	config.alphaMode = WGPUCompositeAlphaMode_Opaque;
	config.presentMode = WGPUPresentMode_Fifo; // Browser always vsyncs via requestAnimationFrame.
	config.width = width;
	config.height = height;
	wgpuSurfaceConfigure(sc->surface, &config);

	// Diagnostic: verify JS-side canvas context state after configure.
	EM_ASM({
		var c = document.querySelector('#canvas');
		if (!c) { console.error('[DIAG-CFG] canvas element not found'); return; }
		var ctx = c.getContext('webgpu');
		console.log('[DIAG-CFG] canvas=' + c.tagName + '#' + c.id +
			' drawingBuffer=' + c.width + 'x' + c.height +
			' ctx=' + (ctx ? ctx.constructor.name : 'null'));
		// Monkey-patch queue.submit to wrap with error scope checking.
		var d = Module['preinitializedWebGPUDevice'];
		if (d && d.queue && !d.queue._submitPatched) {
			var origSubmit = d.queue.submit.bind(d.queue);
			var submitCount = 0;
			d.queue.submit = function(cmdBufs) {
				submitCount++;
				d.pushErrorScope('validation');
				var result = origSubmit(cmdBufs);
				d.popErrorScope().then(function(error) {
					if (error) {
						console.error('[SUBMIT-ERROR] #' + submitCount + ' bufs=' + cmdBufs.length + ' err=' + error.message);
					} else if (submitCount <= 5) {
						console.log('[SUBMIT-OK] #' + submitCount + ' bufs=' + cmdBufs.length + ' no errors');
					}
				});
				return result;
			};
			d.queue._submitPatched = true;
			console.log('[DIAG-CFG] queue.submit monkey-patched with error scope');
		}
		// Monkey-patch createRenderPipeline to capture creation-time errors with full messages.
		if (d && !d._pipelinePatched) {
			var origCreate = d.createRenderPipeline.bind(d);
			d.createRenderPipeline = function(desc) {
				d.pushErrorScope('validation');
				var pipeline = origCreate(desc);
				d.popErrorScope().then(function(err) {
					if (err) {
						console.error('[PIPELINE-CREATE-ERROR] ' + err.message.substring(0, 1200));
					}
				});
				return pipeline;
			};
			d._pipelinePatched = true;
			console.log('[DIAG-CFG] createRenderPipeline monkey-patched');
		}
		// Monkey-patch createShaderModule to capture WGSL compilation errors.
		if (d && !d._shaderModPatched) {
			var origCreateMod = d.createShaderModule.bind(d);
			d.createShaderModule = function(desc) {
				d.pushErrorScope('validation');
				var mod = origCreateMod(desc);
				d.popErrorScope().then(function(err) {
					if (err) {
						console.error('[SHADER-COMPILE-ERROR] ' + err.message.substring(0, 1200));
					}
				});
				return mod;
			};
			d._shaderModPatched = true;
			console.log('[DIAG-CFG] createShaderModule monkey-patched');
		}
		// Catch all uncaptured WebGPU errors (draw-time validation, etc.)
		if (d && !d._uncapturedPatched) {
			d.addEventListener('uncapturederror', function(e) {
				console.error('[UNCAPTURED-GPU-ERROR] ' + e.error.message);
			});
			d._uncapturedPatched = true;
			console.log('[DIAG-CFG] uncaptured error handler registered');
		}
	});

	sc->width = width;
	sc->height = height;
	sc->configured = true;
	// Clear the needs_resize flag so swap_chain_acquire_framebuffer doesn't
	context_driver->surface_set_needs_resize(sc->surface_id, false);
	return OK;
}

RDD::FramebufferID RenderingDeviceDriverWebGPU::swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) {
	WGSwapChain *sc = (WGSwapChain *)(p_swap_chain.id);
	ERR_FAIL_NULL_V(sc, FramebufferID());
	if (!sc->configured || context_driver->surface_get_needs_resize(sc->surface_id)) {
		// Not yet sized or surface dimensions changed — request a resize.
		if (!sc->configured) {
			print_verbose("WebGPU: swap_chain_acquire_framebuffer: not configured, requesting resize");
		}
		r_resize_required = true;
		return FramebufferID();
	}

	// Release resources from the previous frame.
	if (sc->current_framebuffer) {
		delete sc->current_framebuffer;
		sc->current_framebuffer = nullptr;
	}
	if (sc->current_view) {
		wgpuTextureViewRelease(sc->current_view);
		sc->current_view = nullptr;
	}
	if (sc->current_texture) {
		wgpuTextureRelease(sc->current_texture);
		sc->current_texture = nullptr;
	}

	// Acquire the next swap chain texture.
	WGPUSurfaceTexture surface_texture = {};
	wgpuSurfaceGetCurrentTexture(sc->surface, &surface_texture);

	// Diagnostic: log surface texture status for the first few frames.
	static int _st_log = 0;
	if (_st_log < 10) {
		EM_ASM({ console.log('[SURFACE] status=' + $0 + ' texture=' + ($1 ? 'valid' : 'NULL')); },
				(int)surface_texture.status, (int)(surface_texture.texture != nullptr));
		_st_log++;
	}

	if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal ||
			surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
			surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
		WARN_PRINT_ONCE(vformat("WebGPU: wgpuSurfaceGetCurrentTexture: resize-needed status=%d", (int)surface_texture.status));
		r_resize_required = true;
		return FramebufferID();
	}
	if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal) {
		WARN_PRINT_ONCE(vformat("WebGPU: wgpuSurfaceGetCurrentTexture: unexpected status=%d", (int)surface_texture.status));
		return FramebufferID();
	}

	sc->current_texture = surface_texture.texture;

	// Get actual surface texture dimensions. On web, wgpuSurfaceConfigure may be called
	// with OS physical-pixel dimensions (e.g. 1152×648 on HiDPI), but the browser canvas
	// may be CSS-sized (e.g. 756×417). wgpuTextureGetWidth gives the real GPU texture size.
	uint32_t actual_w = wgpuTextureGetWidth(sc->current_texture);
	uint32_t actual_h = wgpuTextureGetHeight(sc->current_texture);
	if (actual_w != sc->width || actual_h != sc->height) {
		WARN_PRINT_ONCE(vformat("WebGPU: surface texture (%d x %d) differs from configured (%d x %d) — using actual size",
				actual_w, actual_h, sc->width, sc->height));
		sc->width = actual_w;
		sc->height = actual_h;
		// Sync the context driver's stored surface dimensions so that screen_get_width/height
		// (used by the blit dst_rect normalization) returns the actual canvas size, not the
		// configured HiDPI physical-pixel size. Without this, the blit only covers a fraction
		// of the swap chain surface.
		context_driver->surface_set_size(sc->surface_id, actual_w, actual_h);
	}

	// Create a view for this frame's swap chain texture.
	WGPUTextureViewDescriptor view_desc = {};
	view_desc.format = sc->format;
	view_desc.dimension = WGPUTextureViewDimension_2D;
	view_desc.baseMipLevel = 0;
	view_desc.mipLevelCount = 1;
	view_desc.baseArrayLayer = 0;
	view_desc.arrayLayerCount = 1;
	view_desc.aspect = WGPUTextureAspect_All;
	sc->current_view = wgpuTextureCreateView(sc->current_texture, &view_desc);
	ERR_FAIL_COND_V(sc->current_view == nullptr, FramebufferID());

	// Wrap in a WGFramebuffer. The WGTexture pointer is null since we manage
	// the texture lifetime through the swap chain, not the framebuffer.
	WGFramebuffer *fb = new WGFramebuffer();
	fb->render_pass = sc->render_pass;
	fb->width = sc->width;
	fb->height = sc->height;
	fb->attachments.push_back(nullptr);
	fb->attachment_views.push_back(sc->current_view);
	sc->current_framebuffer = fb;

	r_resize_required = false;
	return FramebufferID(fb);
}

RDD::RenderPassID RenderingDeviceDriverWebGPU::swap_chain_get_render_pass(SwapChainID p_swap_chain) {
	WGSwapChain *sc = (WGSwapChain *)(p_swap_chain.id);
	ERR_FAIL_NULL_V(sc, RenderPassID());
	return RenderPassID(sc->render_pass);
}

RDD::DataFormat RenderingDeviceDriverWebGPU::swap_chain_get_format(SwapChainID p_swap_chain) {
	WGSwapChain *sc = (WGSwapChain *)(p_swap_chain.id);
	ERR_FAIL_NULL_V(sc, DATA_FORMAT_MAX);
	return _wgpu_to_data_format(sc->format);
}

void RenderingDeviceDriverWebGPU::swap_chain_free(SwapChainID p_swap_chain) {
	WGSwapChain *sc = (WGSwapChain *)(p_swap_chain.id);
	ERR_FAIL_NULL(sc);
	if (sc->current_framebuffer) {
		delete sc->current_framebuffer;
	}
	if (sc->current_view) {
		wgpuTextureViewRelease(sc->current_view);
	}
	if (sc->current_texture) {
		wgpuTextureRelease(sc->current_texture);
	}
	if (sc->render_pass) {
		delete sc->render_pass;
	}
	if (sc->surface && sc->configured) {
		wgpuSurfaceUnconfigure(sc->surface);
	}
	delete sc;
}

// =============================================================================
// FRAMEBUFFER
// =============================================================================

RDD::FramebufferID RenderingDeviceDriverWebGPU::framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) {
	WGFramebuffer *fb = new WGFramebuffer();
	fb->render_pass = (WGRenderPass *)(p_render_pass.id);
	fb->width = p_width;
	fb->height = p_height;

	for (uint32_t i = 0; i < p_attachments.size(); i++) {
		WGTexture *tex = (WGTexture *)(p_attachments[i].id);
		fb->attachments.push_back(tex);
		fb->attachment_views.push_back(tex ? tex->default_view : nullptr);
	}

	return FramebufferID(fb);
}

void RenderingDeviceDriverWebGPU::framebuffer_free(FramebufferID p_framebuffer) {
	WGFramebuffer *fb = (WGFramebuffer *)(p_framebuffer.id);
	delete fb;
}

// =============================================================================
// SHADER
// =============================================================================

// Helper: map Godot stage mask bits to WGPUShaderStage visibility flags.
// After NAGA processing (comparison splitting, type changes), the WGSL shader
// may reference bindings in stages not predicted by the original SPIR-V reflection.
// To avoid visibility mismatches, OR in both Vertex and Fragment for any render
// shader that uses either stage. Compute stays separate.
static WGPUShaderStage _stages_to_wgpu_visibility(uint32_t p_stage_mask) {
	WGPUShaderStage vis = WGPUShaderStage_None;
	bool has_render = (p_stage_mask & ((1u << RDD::SHADER_STAGE_VERTEX) | (1u << RDD::SHADER_STAGE_FRAGMENT) |
			(1u << RDD::SHADER_STAGE_TESSELATION_CONTROL) | (1u << RDD::SHADER_STAGE_TESSELATION_EVALUATION))) != 0;
	if (has_render) {
		vis = (WGPUShaderStage)(WGPUShaderStage_Vertex | WGPUShaderStage_Fragment);
	}
	if (p_stage_mask & (1u << RDD::SHADER_STAGE_COMPUTE)) {
		vis = (WGPUShaderStage)(vis | WGPUShaderStage_Compute);
	}
	return vis;
}

RDD::ShaderID RenderingDeviceDriverWebGPU::shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) {
	ERR_FAIL_COND_V(p_shader_container.is_null(), ShaderID());

	Ref<RenderingShaderContainerWebGPU> wg_container = p_shader_container;
	ERR_FAIL_COND_V(wg_container.is_null(), ShaderID());

	RenderingDeviceCommons::ShaderReflection shader_refl = p_shader_container->get_shader_reflection();

	WGShader *shader = new WGShader();
	shader->name = String(p_shader_container->shader_name.ptr());
	shader->push_constant_bind_group = wg_container->get_push_constant_bind_group();
	shader->push_constant_binding = wg_container->get_push_constant_binding();
	shader->push_constant_size = shader_refl.push_constant_size;

	print_verbose(vformat("WebGPU: shader_create_from_container '%s' (%d stages, push_const_size=%d)", shader->name, (int)p_shader_container->shaders.size(), (int)shader_refl.push_constant_size));

	// Maps (set_index << 16 | binding) to the WGSL texture view dimension detected from NAGA output.
	// Used so SAMPLER_WITH_TEXTURE and TEXTURE BGL entries get the correct viewDimension.
	HashMap<uint32_t, WGPUTextureViewDimension> wgsl_tex_dims;

	// Maps (set_index << 16 | binding) → true if the WGSL storage buffer is read-only (var<storage, read>),
	// false if read-write (var<storage, read_write>). Used to correctly set BGL buffer type for SSBOs.
	HashMap<uint32_t, bool> wgsl_ssbo_readonly;

	// Maps (set_index << 16 | binding) → storage texture access mode detected from NAGA WGSL output.
	// NAGA emits: texture_storage_2d<format, write/read/read_write>. Used for IMAGE BGL entries.
	HashMap<uint32_t, WGPUStorageTextureAccess> wgsl_storage_tex_access;

	// Maps (set_index << 16 | binding) → storage texture format (from WGSL scan). Used for IMAGE BGL entries.
	HashMap<uint32_t, WGPUTextureFormat> wgsl_storage_tex_format;

	// Maps (set_index << 16 | binding) → true if NAGA output has var<uniform> (e.g. for texture buffers).
	HashMap<uint32_t, bool> wgsl_is_uniform;

	// Maps (set_index << 16 | binding) → true if the WGSL has texture_depth_* at this binding.
	HashMap<uint32_t, bool> wgsl_is_depth_texture;

	// Maps (set_index << 16 | binding) → true if the WGSL has sampler_comparison at this binding.
	HashMap<uint32_t, bool> wgsl_is_comparison_sampler;

	// Depth alias bindings: NAGA splits mixed-usage depth textures into two globals
	// (one Depth at binding B, one Float alias at binding B+1). Track (set,B+1) pairs
	// so we can add extra BGL and bind group entries.
	// Maps (set_index << 16 | alias_binding) → depth_binding (the adjacent depth texture).
	HashMap<uint32_t, uint32_t> wgsl_depth_alias_bindings;

	// --- Create one WGPUShaderModule per stage ---
	Vector<RenderingShaderContainer::Shader> &stage_shaders = p_shader_container->shaders;
	for (int i = 0; i < stage_shaders.size(); i++) {
		const RenderingShaderContainer::Shader &s = stage_shaders[i];

		// The code_compressed_bytes holds raw SPIR-V (no compression — code_decompressed_size == 0).
		const PackedByteArray &spv_bytes = s.code_compressed_bytes;
		ERR_FAIL_COND_V_MSG(spv_bytes.is_empty(), ShaderID(), "WebGPU: empty SPIR-V for shader stage.");
		ERR_FAIL_COND_V_MSG(spv_bytes.size() % 4 != 0, ShaderID(), "WebGPU: SPIR-V size must be a multiple of 4.");

		// Store raw SPIR-V for potential re-conversion with specialization constants.
		shader->stage_spirv[(int)s.shader_stage] = spv_bytes;

		// emdawnwebgpu does NOT support WGPUShaderSourceSPIRV — it's a thin wrapper
		// around the browser's WebGPU API which only accepts WGSL.
		// We convert SPIR-V → WGSL at runtime using naga (compiled to WASM).
		// The naga converter is loaded in the HTML shell and exposed as window.nagaSpirvToWgsl().
		// NOTE: Must use MAIN_THREAD_EM_ASM_PTR because shader creation runs on a
		// worker thread (pthread) where `window` is not defined. This proxies the
		// call to the main thread synchronously.
		char *wgsl_str = (char *)(uintptr_t)MAIN_THREAD_EM_ASM_PTR({
			try {
				if (typeof window.nagaSpirvToWgsl !== 'function') {
					console.error('naga SPIR-V→WGSL converter not loaded!');
					return 0;
				}
				var spirvBytes = new Uint8Array(HEAPU8.buffer, $0, $1);
				var wgsl = window.nagaSpirvToWgsl(spirvBytes);
				if (!wgsl) { return 0; }
				var len = lengthBytesUTF8(wgsl) + 1;
				var ptr = _malloc(len);
				stringToUTF8(wgsl, ptr, len);
				return ptr;
			} catch (e) {
				console.error('[SHADER] NAGA conversion exception:', e.message || e);
				return 0;
			}
		}, spv_bytes.ptr(), (int)spv_bytes.size());

		ERR_FAIL_COND_V_MSG(wgsl_str == nullptr, ShaderID(), vformat("WebGPU: SPIR-V→WGSL conversion failed for stage %d.", (int)s.shader_stage));

		// Diagnostic: check push constant representation in WGSL.
		{
			static int _wgsl_diag = 0;
			if (_wgsl_diag < 40) {
				bool has_pc_group3 = strstr(wgsl_str, "@group(3)") != nullptr;
				bool has_pc_binding120 = strstr(wgsl_str, "@binding(120)") != nullptr;
				bool has_push_constants = strstr(wgsl_str, "push_constants") != nullptr;
				bool has_push_constant = strstr(wgsl_str, "push_constant") != nullptr;
				int wgsl_len = strlen(wgsl_str);
				EM_ASM({ console.log('[WGSL#' + $0 + '] stage=' + $1 + ' len=' + $2 + ' grp3=' + $3 + ' b120=' + $4 + ' push_constants=' + $5 + ' push_constant=' + $6); },
						_wgsl_diag, (int)s.shader_stage, wgsl_len, has_pc_group3 ? 1 : 0, has_pc_binding120 ? 1 : 0, has_push_constants ? 1 : 0, has_push_constant ? 1 : 0);
				_wgsl_diag++;
			}
		}

		// DEPTH_ALIAS parsing removed — depth=2 images are now depth=1 in SPIR-V,
		// and a single texture_depth_2d variable handles both sampling modes.

		// Remap unsupported 8-bit storage texture format names in WGSL.
		// r8* and rg8* are not valid WebGPU storage texel formats — remap to 32-bit equivalents.
		// This changes string length, so we rebuild the string via Godot's String class.
		if (strstr(wgsl_str, "r8unorm") || strstr(wgsl_str, "r8snorm") ||
				strstr(wgsl_str, "r8uint") || strstr(wgsl_str, "r8sint") ||
				strstr(wgsl_str, "rg8unorm") || strstr(wgsl_str, "rg8snorm") ||
				strstr(wgsl_str, "rg8uint") || strstr(wgsl_str, "rg8sint")) {
			String ws(wgsl_str);
			ws = ws.replace("rg8unorm", "rg32float");
			ws = ws.replace("rg8snorm", "rg32float");
			ws = ws.replace("rg8uint", "rg32uint");
			ws = ws.replace("rg8sint", "rg32sint");
			ws = ws.replace("r8unorm", "r32float");
			ws = ws.replace("r8snorm", "r32float");
			ws = ws.replace("r8uint", "r32uint");
			ws = ws.replace("r8sint", "r32sint");
			free(wgsl_str);
			CharString cs = ws.utf8();
			wgsl_str = (char *)malloc(cs.length() + 1);
			memcpy(wgsl_str, cs.get_data(), cs.length() + 1);
		}

		// If texture-formats-tier1 is not available, remap 16-bit SNORM/UNORM storage
		// texture format names to their float equivalents in the WGSL text. The format
		// string lengths are preserved (pad with spaces) so scan offsets remain valid.
		// r16snorm  → r16float  (same 8 chars)
		// r16unorm  → r16float  (same 8 chars)
		// rg16snorm → rg16float (same 9 chars — "rg16float" is 9 chars, perfect)
		// rg16unorm → rg16float (same 9 chars)
		// rgba16snorm → rgba16float (11 vs 11 — perfect)
		// rgba16unorm → rgba16float (11 vs 11 — perfect)
		if (!has_texture_formats_tier1) {
			char *q = wgsl_str;
			while (*q) {
				if (strncmp(q, "rgba16snorm", 11) == 0) { memcpy(q, "rgba16float", 11); q += 11; }
				else if (strncmp(q, "rgba16unorm", 11) == 0) { memcpy(q, "rgba16float", 11); q += 11; }
				else if (strncmp(q, "rg16snorm", 9) == 0) { memcpy(q, "rg16float", 9); q += 9; }
				else if (strncmp(q, "rg16unorm", 9) == 0) { memcpy(q, "rg16float", 9); q += 9; }
				else if (strncmp(q, "r16snorm", 8) == 0) { memcpy(q, "r16float", 8); q += 8; }
				else if (strncmp(q, "r16unorm", 8) == 0) { memcpy(q, "r16float", 8); q += 8; }
				else { q++; }
			}
		}

		// WebGPU only supports a limited set of storage texel formats (see spec §26.1.1).
		// 16-bit single/dual-channel formats (r16*, rg16*) are NOT valid for storage.
		// Remap them to 32-bit equivalents. Also handles rgba16snorm/unorm → rgba32float.
		// Format names only appear in texture_storage_*<format, access> declarations in WGSL.
		// All replacements preserve string length (in-place memcpy).
		{
			char *q = wgsl_str;
			while (*q) {
				// RGBA16 snorm/unorm → rgba16float (rgba16float IS a valid storage format)
				if (strncmp(q, "rgba16snorm", 11) == 0) { memcpy(q, "rgba16float", 11); q += 11; }
				else if (strncmp(q, "rgba16unorm", 11) == 0) { memcpy(q, "rgba16float", 11); q += 11; }
				// RG16 all variants → rg32 equivalents
				else if (strncmp(q, "rg16float", 9) == 0) { memcpy(q, "rg32float", 9); q += 9; }
				else if (strncmp(q, "rg16snorm", 9) == 0) { memcpy(q, "rg32float", 9); q += 9; }
				else if (strncmp(q, "rg16unorm", 9) == 0) { memcpy(q, "rg32float", 9); q += 9; }
				else if (strncmp(q, "rg16uint", 8) == 0) { memcpy(q, "rg32uint", 8); q += 8; }
				else if (strncmp(q, "rg16sint", 8) == 0) { memcpy(q, "rg32sint", 8); q += 8; }
				// R16 all variants → r32 equivalents
				else if (strncmp(q, "r16float", 8) == 0) { memcpy(q, "r32float", 8); q += 8; }
				else if (strncmp(q, "r16snorm", 8) == 0) { memcpy(q, "r32float", 8); q += 8; }
				else if (strncmp(q, "r16unorm", 8) == 0) { memcpy(q, "r32float", 8); q += 8; }
				else if (strncmp(q, "r16uint", 7) == 0) { memcpy(q, "r32uint", 7); q += 7; }
				else if (strncmp(q, "r16sint", 7) == 0) { memcpy(q, "r32sint", 7); q += 7; }
				else { q++; }
			}
		}

		// WebGPU restriction: Storage buffers with read_write access cannot be used in vertex shaders.
		// NAGA generates var<storage, read_write> for any SSBO without NonWritable decoration.
		// For render stages (vertex + fragment), demote all read_write storage to read (in-place,
		// same string length). This ensures the BGL can use ReadOnlyStorage with Vertex|Fragment
		// visibility. Compute stages keep read_write for actual writes.
		if (s.shader_stage == RDD::SHADER_STAGE_VERTEX || s.shader_stage == RDD::SHADER_STAGE_FRAGMENT) {
			char *q = wgsl_str;
			while ((q = strstr(q, "var<storage, read_write>")) != nullptr) {
				// "var<storage, read_write>" = 24 chars → "var<storage, read>      " = 24 chars
				memcpy(q, "var<storage, read>      ", 24);
				q += 24;
			}
		}

		// Chrome doesn't support the 'sized_binding_array' WGSL language feature.
		// Naga converts GLSL sampler arrays like "sampler2DArray tex[1]" to
		// "binding_array<texture_2d_array<f32>, 1>" in WGSL. Fix: replace
		// "binding_array<T, 1>" with just "T", and fix "varname[0]" → "varname".
		if (strstr(wgsl_str, "binding_array<")) {
			String ws(wgsl_str);
			Vector<String> binding_array_vars;
			int64_t search_from = 0;
			while (true) {
				int64_t ba_pos = ws.find(": binding_array<", search_from);
				if (ba_pos == -1) break;
				int64_t inner_start = ba_pos + (int64_t)strlen(": binding_array<");
				int depth = 1;
				int64_t p = inner_start;
				int64_t ws_len = (int64_t)ws.length();
				while (p < ws_len && depth > 0) {
					char32_t c = ws[p];
					if (c == '<') depth++;
					else if (c == '>') depth--;
					p++;
				}
				// ws[inner_start .. p-2] = "TYPE, COUNT"
				String inner = ws.substr(inner_start, p - 1 - inner_start);
				int64_t last_comma = inner.rfind(",");
				if (last_comma == -1) { search_from = p; continue; }
				String type_part = inner.substr(0, last_comma).strip_edges();
				int count_val = inner.substr(last_comma + 1).strip_edges().to_int();
				if (count_val == 1) {
					// Extract variable name (identifier immediately before the ':')
					int64_t name_end = ba_pos;
					while (name_end > 0 && ws[name_end - 1] == ' ') name_end--;
					int64_t name_start = name_end;
					while (name_start > 0) {
						char32_t c = ws[name_start - 1];
						if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
							name_start--;
						else break;
					}
					String var_name = ws.substr(name_start, name_end - name_start);
					if (!var_name.is_empty()) {
						binding_array_vars.push_back(var_name);
					}
					// Replace ": binding_array<TYPE, 1>" with ": TYPE"
					String new_type = ": " + type_part;
					ws = ws.substr(0, ba_pos) + new_type + ws.substr(p);
					search_from = ba_pos + (int64_t)new_type.length();
				} else {
					search_from = p;
				}
			}
			// Replace VAR_NAME[any_expr] with VAR_NAME for all unwrapped size-1 binding arrays.
			// Naga may use a variable index (e.g. varname[_e889]) not just varname[0].
			for (const String &var : binding_array_vars) {
				int64_t vlen = (int64_t)var.length();
				int64_t search_pos = 0;
				while (true) {
					String needle = var + "[";
					int64_t idx_pos = ws.find(needle, search_pos);
					if (idx_pos == -1) break;
					// Ensure 'var' is not a suffix of a longer identifier
					if (idx_pos > 0) {
						char32_t before = ws[idx_pos - 1];
						if (before == '_' || (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || (before >= '0' && before <= '9')) {
							search_pos = idx_pos + 1;
							continue;
						}
					}
					// Scan past the matching ']'
					int64_t p = idx_pos + vlen + 1; // skip var + '['
					int depth = 1;
					int64_t ws_len2 = (int64_t)ws.length();
					while (p < ws_len2 && depth > 0) {
						if (ws[p] == '[') depth++;
						else if (ws[p] == ']') depth--;
						p++;
					}
					ws = ws.substr(0, idx_pos) + var + ws.substr(p);
					search_pos = idx_pos + vlen;
				}
			}
			free(wgsl_str);
			CharString cs = ws.utf8();
			wgsl_str = (char *)malloc(cs.length() + 1);
			memcpy(wgsl_str, cs.get_data(), cs.length() + 1);
		}

		WGPUShaderSourceWGSL wgsl_source = {};
		wgsl_source.chain.sType = WGPUSType_ShaderSourceWGSL;
		wgsl_source.code = WGPUStringView{ wgsl_str, WGPU_STRLEN };

		WGPUShaderModuleDescriptor mod_desc = {};
		mod_desc.nextInChain = (WGPUChainedStruct *)&wgsl_source;

		WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &mod_desc);

		// Scan WGSL for texture dimension declarations so the BGL uses the right viewDimension.
		// NAGA format: "@group(G) @binding(B) var NAME: texture_TYPE<...>;"
		// Also detects sampler / sampler_comparison types.
		{
			const char *p = wgsl_str;
			while ((p = strstr(p, "@group(")) != nullptr) {
				unsigned int grp = 0, bnd = 0;
				if (sscanf(p, "@group(%u) @binding(%u)", &grp, &bnd) == 2) {
					// Find the ':' that separates the variable name from the type.
					// This avoids matching "sampler" in variable names like "shadow_sampler".
					const char *colon = strchr(p, ':');
					const char *semi = strchr(p, ';');
					if (!semi) { p++; continue; }
					const char *type_start = colon && colon < semi ? colon + 1 : p;
					const char *limit = semi; // scan up to the semicolon

					const char *fwd = type_start;
					const char *tp = nullptr;
					const char *sp = nullptr; // sampler type position
					while (fwd < limit && *fwd) {
						if (!tp && strncmp(fwd, "texture_", 8) == 0) { tp = fwd; break; }
						if (!sp && strncmp(fwd, "sampler_comparison", 18) == 0) { sp = fwd; break; }
						if (!sp && strncmp(fwd, "sampler", 7) == 0 && fwd[7] != '_') { sp = fwd; break; }
						fwd++;
					}
					// Check for comparison sampler (sampler_comparison type).
					if (sp && strncmp(sp, "sampler_comparison", 18) == 0) {
						uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
						wgsl_is_comparison_sampler[key] = true;
					}
					if (tp) {
						WGPUTextureViewDimension dim = WGPUTextureViewDimension_Undefined;
						if (strncmp(tp, "texture_depth_2d_array", 22) == 0) {
							dim = WGPUTextureViewDimension_2DArray;
						} else if (strncmp(tp, "texture_depth_cube_array", 24) == 0) {
							dim = WGPUTextureViewDimension_CubeArray;
						} else if (strncmp(tp, "texture_depth_cube", 18) == 0) {
							dim = WGPUTextureViewDimension_Cube;
						} else if (strncmp(tp, "texture_depth_2d", 16) == 0) {
							dim = WGPUTextureViewDimension_2D;
						} else if (strncmp(tp, "texture_2d_array", 16) == 0) {
							dim = WGPUTextureViewDimension_2DArray;
						} else if (strncmp(tp, "texture_cube_array", 18) == 0) {
							dim = WGPUTextureViewDimension_CubeArray;
						} else if (strncmp(tp, "texture_cube", 12) == 0) {
							dim = WGPUTextureViewDimension_Cube;
						} else if (strncmp(tp, "texture_3d", 10) == 0) {
							dim = WGPUTextureViewDimension_3D;
						} else if (strncmp(tp, "texture_2d", 10) == 0) {
							dim = WGPUTextureViewDimension_2D;
						} else if (strncmp(tp, "texture_1d", 10) == 0) {
							dim = WGPUTextureViewDimension_1D;
						} else if (strncmp(tp, "texture_storage_2d_array", 24) == 0) {
							dim = WGPUTextureViewDimension_2DArray;
						} else if (strncmp(tp, "texture_storage_cube_array", 26) == 0) {
							dim = WGPUTextureViewDimension_CubeArray;
						} else if (strncmp(tp, "texture_storage_2d", 18) == 0) {
							dim = WGPUTextureViewDimension_2D;
						} else if (strncmp(tp, "texture_storage_3d", 18) == 0) {
							dim = WGPUTextureViewDimension_3D;
						}
						if (dim != WGPUTextureViewDimension_Undefined) {
							uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
							if (!wgsl_tex_dims.has(key)) {
								wgsl_tex_dims[key] = dim;
							} else if (wgsl_tex_dims[key] != dim) {
								// Different stages have different types for the same binding — use the later stage value.
								wgsl_tex_dims[key] = dim;
							}
						}
						// Check if this is a depth texture (texture_depth_*).
						if (tp && strncmp(tp, "texture_depth_", 14) == 0) {
							uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
							wgsl_is_depth_texture[key] = true;
						}
					}
					// Check for depth alias variable: NAGA names it "*_depth_alias".
					// The variable name is between "var " and ":".
					{
						const char *var_kw = strstr(p, "var ");
						if (var_kw && var_kw < semi) {
							const char *name_start = var_kw + 4;
							// Skip any <...> (e.g., var<uniform>)
							if (*name_start == '<') {
								const char *gt = strchr(name_start, '>');
								if (gt && gt < semi) {
									name_start = gt + 1;
									while (name_start < semi && *name_start == ' ') { name_start++; }
								}
							}
							int name_len = 0;
							const char *nc = name_start;
							while (nc < semi && *nc != ':' && *nc != ' ') { nc++; name_len++; }
							if (name_len > 12) { // "_depth_alias" is 12 chars
								const char *suffix = name_start + name_len - 12;
								if (strncmp(suffix, "_depth_alias", 12) == 0) {
									uint32_t alias_key = ((uint32_t)grp << 16) | (uint32_t)bnd;
									uint32_t depth_bnd = bnd > 0 ? bnd - 1 : 0;
									wgsl_depth_alias_bindings[alias_key] = depth_bnd;
								}
							}
						}
					}
				}
				p++;
			}
		}

		// Scan WGSL for storage buffer access modes.
		// NAGA actual output formats:
		//   - Read-write: "var<storage, read_write>"  (space after comma)
		//   - Read-only:  "var<storage>"              (NO access modifier — NAGA omits "read" for LOAD-only)
		// Note: "var<storage, read>" and "var<storage,read>" are valid WGSL but NOT emitted by NAGA;
		// we match them anyway for robustness.
		{
			const char *p = wgsl_str;
			while ((p = strstr(p, "@group(")) != nullptr) {
				unsigned int grp = 0, bnd = 0;
				if (sscanf(p, "@group(%u) @binding(%u)", &grp, &bnd) == 2) {
					const char *fwd = p;
					const char *limit = fwd + 256;
					while (fwd < limit && *fwd && *fwd != ';') {
						if (strncmp(fwd, "var<storage, read_write>", 24) == 0 ||
								strncmp(fwd, "var<storage,read_write>", 23) == 0) {
							uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
							wgsl_ssbo_readonly[key] = false;
							break;
						} else if (strncmp(fwd, "var<storage, read>", 18) == 0 ||
								strncmp(fwd, "var<storage,read>", 17) == 0) {
							uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
							wgsl_ssbo_readonly[key] = true;
							break;
						} else if (strncmp(fwd, "var<storage>", 12) == 0) {
							// NAGA emits var<storage> (no access mode) for LOAD-only (read-only) storage:
							// address_space_str returns (Some("storage"), None) when !access.contains(STORE).
							uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
							wgsl_ssbo_readonly[key] = true;
							break;
						} else if (strncmp(fwd, "var<uniform", 11) == 0) {
							uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
							wgsl_is_uniform[key] = true;
							break;
						}
						fwd++;
					}
				}
				p++;
			}
		}

		// Scan WGSL for storage texture access mode and format.
		// NAGA format: "@group(G) @binding(B) var name: texture_storage_*<format, access>;"
		{
			const char *p = wgsl_str;
			while ((p = strstr(p, "@group(")) != nullptr) {
				unsigned int grp = 0, bnd = 0;
				if (sscanf(p, "@group(%u) @binding(%u)", &grp, &bnd) == 2) {
					const char *fwd = p;
					const char *limit = fwd + 300;
					while (fwd < limit && *fwd && *fwd != ';') {
						if (strncmp(fwd, "texture_storage_", 16) == 0) {
							const char *lt = strchr(fwd, '<');
							if (lt && lt < limit) {
								// Extract format (between < and ,)
								const char *comma = strchr(lt + 1, ',');
								if (comma && comma < limit) {
									uint32_t key = ((uint32_t)grp << 16) | (uint32_t)bnd;
									// Parse format name
									const char *fmt = lt + 1;
									WGPUTextureFormat tf = WGPUTextureFormat_RGBA8Unorm; // fallback
									if (strncmp(fmt, "rgba8unorm,", 11) == 0) tf = WGPUTextureFormat_RGBA8Unorm;
									else if (strncmp(fmt, "rgba8snorm,", 11) == 0) tf = WGPUTextureFormat_RGBA8Snorm;
									else if (strncmp(fmt, "rgba8uint,", 10) == 0) tf = WGPUTextureFormat_RGBA8Uint;
									else if (strncmp(fmt, "rgba8sint,", 10) == 0) tf = WGPUTextureFormat_RGBA8Sint;
									else if (strncmp(fmt, "rgba16float,", 12) == 0) tf = WGPUTextureFormat_RGBA16Float;
									else if (strncmp(fmt, "rgba16uint,", 11) == 0) tf = WGPUTextureFormat_RGBA16Uint;
									else if (strncmp(fmt, "rgba16sint,", 11) == 0) tf = WGPUTextureFormat_RGBA16Sint;
									else if (strncmp(fmt, "rgba32float,", 12) == 0) tf = WGPUTextureFormat_RGBA32Float;
									else if (strncmp(fmt, "rgba32uint,", 11) == 0) tf = WGPUTextureFormat_RGBA32Uint;
									else if (strncmp(fmt, "rgba32sint,", 11) == 0) tf = WGPUTextureFormat_RGBA32Sint;
									else if (strncmp(fmt, "rg32float,", 10) == 0) tf = WGPUTextureFormat_RG32Float;
									else if (strncmp(fmt, "rg32uint,", 9) == 0) tf = WGPUTextureFormat_RG32Uint;
									else if (strncmp(fmt, "rg32sint,", 9) == 0) tf = WGPUTextureFormat_RG32Sint;
									else if (strncmp(fmt, "r32float,", 9) == 0) tf = WGPUTextureFormat_R32Float;
									else if (strncmp(fmt, "r32uint,", 8) == 0) tf = WGPUTextureFormat_R32Uint;
									else if (strncmp(fmt, "r32sint,", 8) == 0) tf = WGPUTextureFormat_R32Sint;
									else if (strncmp(fmt, "r16float,", 9) == 0) tf = WGPUTextureFormat_R16Float;
									else if (strncmp(fmt, "r16uint,", 8) == 0) tf = WGPUTextureFormat_R16Uint;
									else if (strncmp(fmt, "r16sint,", 8) == 0) tf = WGPUTextureFormat_R16Sint;
									else if (strncmp(fmt, "r16snorm,", 9) == 0) tf = WGPUTextureFormat_R16Float; // Fallback
									else if (strncmp(fmt, "r16unorm,", 9) == 0) tf = WGPUTextureFormat_R16Float; // Fallback
									else if (strncmp(fmt, "r8unorm,", 8) == 0) tf = WGPUTextureFormat_R8Unorm;
									else if (strncmp(fmt, "r8snorm,", 8) == 0) tf = WGPUTextureFormat_R8Snorm;
									else if (strncmp(fmt, "r8uint,", 7) == 0) tf = WGPUTextureFormat_R8Uint;
									else if (strncmp(fmt, "r8sint,", 7) == 0) tf = WGPUTextureFormat_R8Sint;
									else if (strncmp(fmt, "rg8unorm,", 9) == 0) tf = WGPUTextureFormat_RG8Unorm;
									else if (strncmp(fmt, "rg8snorm,", 9) == 0) tf = WGPUTextureFormat_RG8Snorm;
									else if (strncmp(fmt, "rg8uint,", 8) == 0) tf = WGPUTextureFormat_RG8Uint;
									else if (strncmp(fmt, "rg8sint,", 8) == 0) tf = WGPUTextureFormat_RG8Sint;
									else if (strncmp(fmt, "rg16float,", 10) == 0) tf = WGPUTextureFormat_RG16Float;
									else if (strncmp(fmt, "rg16uint,", 9) == 0) tf = WGPUTextureFormat_RG16Uint;
									else if (strncmp(fmt, "rg16sint,", 9) == 0) tf = WGPUTextureFormat_RG16Sint;
									else if (strncmp(fmt, "rg16snorm,", 10) == 0) tf = WGPUTextureFormat_RG16Float; // Fallback
									else if (strncmp(fmt, "rg16unorm,", 10) == 0) tf = WGPUTextureFormat_RG16Float; // Fallback
									else if (strncmp(fmt, "rgba16snorm,", 12) == 0) tf = WGPUTextureFormat_RGBA16Float; // Fallback
									else if (strncmp(fmt, "rgba16unorm,", 12) == 0) tf = WGPUTextureFormat_RGBA16Float; // Fallback
									else if (strncmp(fmt, "bgra8unorm,", 11) == 0) tf = WGPUTextureFormat_BGRA8Unorm;
									wgsl_storage_tex_format[key] = tf;
									// Parse access mode (after comma, skip space)
									const char *acc = comma + 1;
									while (*acc == ' ') acc++;
									WGPUStorageTextureAccess access = WGPUStorageTextureAccess_Undefined;
									if (strncmp(acc, "read_write>", 11) == 0) access = WGPUStorageTextureAccess_ReadWrite;
									else if (strncmp(acc, "write>", 6) == 0) access = WGPUStorageTextureAccess_WriteOnly;
									else if (strncmp(acc, "read>", 5) == 0) access = WGPUStorageTextureAccess_ReadOnly;
									if (access != WGPUStorageTextureAccess_Undefined) {
										wgsl_storage_tex_access[key] = access;
									}
								}
							}
							break;
						}
						fwd++;
					}
				}
				p++;
			}
		}

		free(wgsl_str); // Free the EM_ASM-allocated string.
		ERR_FAIL_COND_V_MSG(mod == nullptr, ShaderID(), vformat("WebGPU: wgpuDeviceCreateShaderModule failed for stage %d.", (int)s.shader_stage));

		if (s.shader_stage < 6) {
			shader->stage_modules[s.shader_stage] = mod;
		}
		// Set the legacy module alias to the first created module.
		if (!shader->module) {
			shader->module = mod;
		}
	}

	// --- Build WGPUBindGroupLayout for each descriptor set ---
	const uint32_t set_count = (uint32_t)shader_refl.uniform_sets.size();
	shader->bind_group_infos.resize(set_count);
	shader->bind_group_layouts.resize(set_count);

	for (uint32_t set = 0; set < set_count; set++) {
		const Vector<RenderingDeviceCommons::ShaderUniform> &set_uniforms = shader_refl.uniform_sets[set];

		// Count entries — combined sampler+texture expands to 2 entries each.
		uint32_t entry_count = 0;
		for (int i = 0; i < set_uniforms.size(); i++) {
			if (set_uniforms[i].type == RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
				entry_count += 2; // Sampler + texture.
			} else {
				entry_count += 1;
			}
		}


		LocalVector<WGPUBindGroupLayoutEntry> entries;
		entries.resize(entry_count);

		WGShader::BindGroupInfo &bgi = shader->bind_group_infos[set];
		bgi.entries.resize(set_uniforms.size());

		uint32_t e_idx = 0;
		for (int u_idx = 0; u_idx < set_uniforms.size(); u_idx++) {
			const RenderingDeviceCommons::ShaderUniform &u = set_uniforms[u_idx];
			WGShader::BindGroupEntry &bge = bgi.entries[u_idx];
			WGPUShaderStage vis = _stages_to_wgpu_visibility((uint32_t)u.stages);

			bge.godot_type = u.type;

			switch (u.type) {
				case RDD::UNIFORM_TYPE_SAMPLER: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					// NAGA reduces binding arrays to size 1.
					if (u.length > 1) {
						entry.bindingArraySize = 1;
					}
					{ uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					  entry.sampler.type = (wgsl_is_comparison_sampler.has(k) && wgsl_is_comparison_sampler[k])
						  ? WGPUSamplerBindingType_Comparison : WGPUSamplerBindingType_Filtering; }
					bge.layout_entry = entry;
					bge.array_length = 1;
				} break;

				case RDD::UNIFORM_TYPE_TEXTURE:
				case RDD::UNIFORM_TYPE_INPUT_ATTACHMENT: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					// NAGA reduces binding arrays to size 1.
					if (u.length > 1) {
						entry.bindingArraySize = 1;
					}
					{ uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					  entry.texture.sampleType = (wgsl_is_depth_texture.has(k) && wgsl_is_depth_texture[k])
						  ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float;
					  entry.texture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D; }
					entry.texture.multisampled = false;
					bge.layout_entry = entry;
					bge.array_length = 1;
				} break;

				case RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
					// Combined sampler+texture split by our SPIR-V preprocessor:
					// Sampler at binding*2+0, texture at binding*2+1 in the modified SPIR-V → matches NAGA WGSL output.
					WGPUBindGroupLayoutEntry &samp_entry = entries[e_idx++];
					samp_entry = {};
					samp_entry.binding = u.binding * 2 + 0;
					samp_entry.visibility = vis;
					{ uint32_t k = ((uint32_t)set << 16) | (u.binding * 2 + 0);
					  samp_entry.sampler.type = (wgsl_is_comparison_sampler.has(k) && wgsl_is_comparison_sampler[k])
						  ? WGPUSamplerBindingType_Comparison : WGPUSamplerBindingType_Filtering; }

					WGPUBindGroupLayoutEntry &tex_entry = entries[e_idx++];
					tex_entry = {};
					tex_entry.binding = u.binding * 2 + 1;
					tex_entry.visibility = vis;
					{ uint32_t k = ((uint32_t)set << 16) | (u.binding * 2 + 1);
					  tex_entry.texture.sampleType = (wgsl_is_depth_texture.has(k) && wgsl_is_depth_texture[k])
						  ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float;
					  tex_entry.texture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D; }
					tex_entry.texture.multisampled = false;

					bge.layout_entry = tex_entry; // Store texture entry as the primary.
				} break;

				case RDD::UNIFORM_TYPE_IMAGE: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					WGPUTextureFormat fmt = wgsl_storage_tex_format.has(k) ? wgsl_storage_tex_format[k] : WGPUTextureFormat_RGBA8Unorm;
					WGPUStorageTextureAccess access = wgsl_storage_tex_access.has(k)
						? wgsl_storage_tex_access[k]
						: (u.writable ? WGPUStorageTextureAccess_WriteOnly : WGPUStorageTextureAccess_ReadOnly);
					entry.storageTexture.access = access;
					entry.storageTexture.format = fmt;
					entry.storageTexture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D;
					bge.layout_entry = entry;
				} break;

				case RDD::UNIFORM_TYPE_UNIFORM_BUFFER: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
					bge.layout_entry = entry;
				} break;

				case RDD::UNIFORM_TYPE_STORAGE_BUFFER: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					// Check if WGSL scan shows this is actually a storage texture.
					if (wgsl_storage_tex_format.has(k)) {
						WGPUTextureFormat fmt = wgsl_storage_tex_format[k];
						WGPUStorageTextureAccess access = wgsl_storage_tex_access.has(k)
							? wgsl_storage_tex_access[k]
							: (u.writable ? WGPUStorageTextureAccess_WriteOnly : WGPUStorageTextureAccess_ReadOnly);
						entry.storageTexture.access = access;
						entry.storageTexture.format = fmt;
						entry.storageTexture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D;

					} else if (wgsl_is_uniform.has(k) && wgsl_is_uniform[k]) {
						// NAGA emitted var<uniform> for this binding.
						entry.buffer.type = WGPUBufferBindingType_Uniform;
					} else {
						bool is_readonly = wgsl_ssbo_readonly.has(k) ? wgsl_ssbo_readonly[k] : !u.writable;
						entry.buffer.type = is_readonly ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
						if (!is_readonly) { entry.visibility = entry.visibility & ~(WGPUShaderStage)WGPUShaderStage_Vertex; }

					}
					bge.layout_entry = entry;
				} break;

				// Dynamic variants treated as static — dynamic offsets not yet implemented.
				case RDD::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
					bge.layout_entry = entry;
				} break;

				case RDD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					{ uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					  if (wgsl_storage_tex_format.has(k)) {
						  WGPUTextureFormat fmt = wgsl_storage_tex_format[k];
						  WGPUStorageTextureAccess access = wgsl_storage_tex_access.has(k)
							  ? wgsl_storage_tex_access[k]
							  : (u.writable ? WGPUStorageTextureAccess_WriteOnly : WGPUStorageTextureAccess_ReadOnly);
						  entry.storageTexture.access = access;
						  entry.storageTexture.format = fmt;
						  entry.storageTexture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D;
					  } else if (wgsl_is_uniform.has(k) && wgsl_is_uniform[k]) {
						  entry.buffer.type = WGPUBufferBindingType_Uniform;
					  } else {
						  bool is_readonly = wgsl_ssbo_readonly.has(k) ? wgsl_ssbo_readonly[k] : !u.writable;
						  entry.buffer.type = is_readonly ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
						  if (!is_readonly) { entry.visibility = entry.visibility & ~(WGPUShaderStage)WGPUShaderStage_Vertex; }
					  } }
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
					bge.layout_entry = entry;
				} break;

				// WebGPU has no texel buffers (TBOs); emulate as uniform or storage buffers based on WGSL.
				case RDD::UNIFORM_TYPE_TEXTURE_BUFFER:
				case RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					// NAGA may convert TBOs to var<uniform> or var<storage,read> depending on usage.
					{ uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					  if (wgsl_is_uniform.has(k) && wgsl_is_uniform[k]) {
						  entry.buffer.type = WGPUBufferBindingType_Uniform;
					  } else if (wgsl_ssbo_readonly.has(k)) {
						  entry.buffer.type = wgsl_ssbo_readonly[k] ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
					  } else {
						  entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage; // default fallback
					  } }
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
					bge.layout_entry = entry;
				} break;

				case RDD::UNIFORM_TYPE_IMAGE_BUFFER: {
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					uint32_t k = ((uint32_t)set << 16) | (u.binding * 2);
					// NAGA may convert image buffers to texture_storage_*.
					if (wgsl_storage_tex_format.has(k)) {
						WGPUTextureFormat fmt = wgsl_storage_tex_format[k];
						WGPUStorageTextureAccess access = wgsl_storage_tex_access.has(k)
							? wgsl_storage_tex_access[k]
							: (u.writable ? WGPUStorageTextureAccess_WriteOnly : WGPUStorageTextureAccess_ReadOnly);
						entry.storageTexture.access = access;
						entry.storageTexture.format = fmt;
						entry.storageTexture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D;
					} else {
						entry.buffer.type = WGPUBufferBindingType_Storage;
						entry.buffer.hasDynamicOffset = false;
						entry.buffer.minBindingSize = 0;
					}
					bge.layout_entry = entry;
				} break;

				default:
					WARN_PRINT_ONCE(vformat("WebGPU: unhandled uniform type %d in bind group layout.", (int)u.type));
					WGPUBindGroupLayoutEntry &entry = entries[e_idx++];
					entry = {};
					entry.binding = u.binding * 2; // NAGA doubles all non-combined bindings.
					entry.visibility = vis;
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					bge.layout_entry = entry;
					break;
			}
		}

		// Add BGL entries for depth alias variables.
		// NAGA splits mixed-usage depth textures: original→Depth at binding B,
		// clone→Float at binding B+1 (named "*_depth_alias").
		for (const KeyValue<uint32_t, uint32_t> &kv : wgsl_depth_alias_bindings) {
			uint32_t alias_key = kv.key;
			uint32_t alias_grp = alias_key >> 16;
			uint32_t alias_bnd = alias_key & 0xFFFF;
			if (alias_grp != set) {
				continue;
			}
			WGPUBindGroupLayoutEntry alias_entry = {};
			alias_entry.binding = alias_bnd;
			alias_entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
			alias_entry.texture.sampleType = WGPUTextureSampleType_Float;
			alias_entry.texture.viewDimension = wgsl_tex_dims.has(alias_key) ? wgsl_tex_dims[alias_key] : WGPUTextureViewDimension_2D;
			alias_entry.texture.multisampled = false;
			entries.push_back(alias_entry);
		}

		// Log any duplicate binding indices for debugging.
		for (uint32_t a = 0; a < entries.size(); a++) {
			for (uint32_t b = a + 1; b < entries.size(); b++) {
				if (entries[a].binding == entries[b].binding) {
					EM_ASM({ console.error('[BGL-DUP] set=' + $0 + ' binding=' + $1 + ' idx_a=' + $2 + ' idx_b=' + $3); },
							(int)set, (int)entries[a].binding, (int)a, (int)b);
				}
			}
		}

		WGPUBindGroupLayoutDescriptor layout_desc = {};
		layout_desc.entryCount = entries.size();
		layout_desc.entries = entries.size() > 0 ? entries.ptr() : nullptr;

		shader->bind_group_layouts[set] = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		ERR_FAIL_COND_V_MSG(shader->bind_group_layouts[set] == nullptr, ShaderID(), "WebGPU: wgpuDeviceCreateBindGroupLayout failed.");
	}

	// Store depth alias bindings on the shader for use during uniform_set_create.
	shader->depth_alias_bindings = wgsl_depth_alias_bindings;

	// --- Build WGPUPipelineLayout ---
	// The number of bind groups in the pipeline layout must cover:
	//   - All descriptor sets (0 .. set_count - 1)
	//   - The push constant bind group slot (if any)
	const bool has_pc = wg_container->has_push_constants();
	const uint32_t pc_group = has_pc ? wg_container->get_push_constant_bind_group() : UINT32_MAX;
	const uint32_t total_groups = has_pc ? MAX(set_count, pc_group + 1) : set_count;

	LocalVector<WGPUBindGroupLayout> all_layouts;
	all_layouts.resize(total_groups);

	// Create empty layouts for any gaps between sets and push constant slot.
	WGPUBindGroupLayout empty_layout = nullptr;
	if (total_groups > set_count) {
		WGPUBindGroupLayoutDescriptor empty_desc = {};
		empty_desc.entryCount = 0;
		empty_layout = wgpuDeviceCreateBindGroupLayout(device, &empty_desc);
	}

	for (uint32_t i = 0; i < total_groups; i++) {
		if (has_pc && i == pc_group) {
			// Build a merged layout: the shader's own group entries (material uniforms,
			// textures, etc.) PLUS the PC ring-buffer entry at binding 0 with hasDynamicOffset.
			// This is needed because calling setBindGroup() twice for the same group index
			// overrides the first binding — we must combine both into one layout/bind group.
			bool has_existing = (i < set_count && i < (uint32_t)shader->bind_group_infos.size() &&
					!shader->bind_group_infos[i].entries.is_empty());
			if (has_existing) {
				// Rebuild all layout entries for this group (both sampler AND texture for
				// combined types — bge.layout_entry only stores the texture entry).
				LocalVector<WGPUBindGroupLayoutEntry> merged_entries;
				const Vector<RenderingDeviceCommons::ShaderUniform> &pc_uniforms = shader_refl.uniform_sets[i];
				for (int u_idx2 = 0; u_idx2 < pc_uniforms.size(); u_idx2++) {
					const RenderingDeviceCommons::ShaderUniform &pu = pc_uniforms[u_idx2];
					WGPUShaderStage pvis = _stages_to_wgpu_visibility((uint32_t)pu.stages);
					if (pu.type == RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
						WGPUBindGroupLayoutEntry se = {}, te = {};
						se.binding = pu.binding * 2 + 0; se.visibility = pvis;
						se.sampler.type = WGPUSamplerBindingType_Filtering;
						te.binding = pu.binding * 2 + 1; te.visibility = pvis;
						te.texture.sampleType = WGPUTextureSampleType_Float;
						{ uint32_t k = ((uint32_t)i << 16) | (pu.binding * 2 + 1);
						  te.texture.viewDimension = wgsl_tex_dims.has(k) ? wgsl_tex_dims[k] : WGPUTextureViewDimension_2D; }
						merged_entries.push_back(se);
						merged_entries.push_back(te);
					} else {
						// For all other types, just copy the existing bge.layout_entry.
						merged_entries.push_back(shader->bind_group_infos[i].entries[u_idx2].layout_entry);
					}
				}
				// Add the PC ring-buffer entry: PUSH_CONSTANT_RING_BINDING, read-only storage, hasDynamicOffset=true.
				// Safety: skip if any existing entry is already at PUSH_CONSTANT_RING_BINDING.
				bool has_pc_binding = false;
				for (const auto &me : merged_entries) { if (me.binding == PUSH_CONSTANT_RING_BINDING) { has_pc_binding = true; break; } }
				WGPUBindGroupLayoutEntry pc_entry = {};
				pc_entry.binding = PUSH_CONSTANT_RING_BINDING;
				pc_entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
				pc_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
				pc_entry.buffer.hasDynamicOffset = true;
				pc_entry.buffer.minBindingSize = 0;
				if (!has_pc_binding) {
					merged_entries.push_back(pc_entry);
				}

				// Add depth alias entries to the merged layout.
				for (const KeyValue<uint32_t, uint32_t> &kv : wgsl_depth_alias_bindings) {
					uint32_t alias_grp = kv.key >> 16;
					uint32_t alias_bnd = kv.key & 0xFFFF;
					if (alias_grp != i) { continue; }
					WGPUBindGroupLayoutEntry ae = {};
					ae.binding = alias_bnd;
					ae.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
					ae.texture.sampleType = WGPUTextureSampleType_Float;
					ae.texture.viewDimension = wgsl_tex_dims.has(kv.key) ? wgsl_tex_dims[kv.key] : WGPUTextureViewDimension_2D;
					ae.texture.multisampled = false;
					merged_entries.push_back(ae);
				}

				WGPUBindGroupLayoutDescriptor merged_desc = {};
				merged_desc.entryCount = merged_entries.size();
				merged_desc.entries = merged_entries.ptr();
				shader->merged_pc_group_layout = wgpuDeviceCreateBindGroupLayout(device, &merged_desc);
				ERR_FAIL_COND_V_MSG(!shader->merged_pc_group_layout, ShaderID(),
						"WebGPU: failed to create merged PC+material bind group layout.");
				all_layouts[i] = shader->merged_pc_group_layout;
			} else {
				// No material uniforms at this group — use the universal PC-only layout.
				all_layouts[i] = push_constant_bind_group_layout;
			}
		} else if (i < set_count) {
			all_layouts[i] = shader->bind_group_layouts[i];
		} else {
			all_layouts[i] = empty_layout ? empty_layout : push_constant_bind_group_layout;
		}
	}

	WGPUPipelineLayoutDescriptor pl_desc = {};
	pl_desc.bindGroupLayoutCount = total_groups;
	pl_desc.bindGroupLayouts = all_layouts.size() > 0 ? all_layouts.ptr() : nullptr;

	shader->pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pl_desc);
	ERR_FAIL_COND_V_MSG(shader->pipeline_layout == nullptr, ShaderID(), "WebGPU: wgpuDeviceCreatePipelineLayout failed.");

	if (empty_layout) {
		wgpuBindGroupLayoutRelease(empty_layout);
	}

	return ShaderID(shader);
}

uint32_t RenderingDeviceDriverWebGPU::shader_get_layout_hash(ShaderID p_shader) {
	WGShader *shader = (WGShader *)(p_shader.id);
	if (!shader) return 0;
	// Use the pipeline layout pointer as a cheap hash identifier.
	return (uint32_t)(uint64_t)(void *)(shader->pipeline_layout);
}

void RenderingDeviceDriverWebGPU::shader_free(ShaderID p_shader) {
	WGShader *shader = (WGShader *)(p_shader.id);
	ERR_FAIL_NULL(shader);
	for (int i = 0; i < 6; i++) {
		if (shader->stage_modules[i]) {
			wgpuShaderModuleRelease(shader->stage_modules[i]);
			shader->stage_modules[i] = nullptr;
		}
	}
	if (shader->pipeline_layout) {
		wgpuPipelineLayoutRelease(shader->pipeline_layout);
	}
	for (WGPUBindGroupLayout &layout : shader->bind_group_layouts) {
		if (layout) {
			wgpuBindGroupLayoutRelease(layout);
		}
	}
	if (shader->merged_pc_group_layout) {
		wgpuBindGroupLayoutRelease(shader->merged_pc_group_layout);
	}
	delete shader;
}

void RenderingDeviceDriverWebGPU::shader_destroy_modules(ShaderID p_shader) {
	WGShader *shader = (WGShader *)(p_shader.id);
	ERR_FAIL_NULL(shader);
	for (int i = 0; i < 6; i++) {
		if (shader->stage_modules[i]) {
			wgpuShaderModuleRelease(shader->stage_modules[i]);
			shader->stage_modules[i] = nullptr;
		}
	}
	shader->module = nullptr;
}

// =============================================================================
// UNIFORM SET
// =============================================================================

RDD::UniformSetID RenderingDeviceDriverWebGPU::uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) {
	WGShader *shader = (WGShader *)(p_shader.id);
	ERR_FAIL_NULL_V(shader, UniformSetID());
	ERR_FAIL_COND_V(p_set_index >= (uint32_t)shader->bind_group_layouts.size(), UniformSetID());

	WGPUBindGroupLayout layout = shader->bind_group_layouts[p_set_index];
	ERR_FAIL_NULL_V(layout, UniformSetID());

	// If this set is also the push constant group AND the shader has a merged layout
	// (PC ring buffer + material uniforms combined), switch to the merged layout.
	// We will also inject a PC ring buffer entry into the entries array below.
	const bool is_pc_group = (shader->push_constant_size > 0 &&
			p_set_index == shader->push_constant_bind_group &&
			shader->merged_pc_group_layout != nullptr);
	if (is_pc_group) {
		layout = shader->merged_pc_group_layout;
	}

	// Each BoundUniform may expand to one or two WGPUBindGroupEntry items.
	LocalVector<WGPUBindGroupEntry> entries;
	entries.reserve(p_uniforms.size() * 2);

	// Allocate the uniform set early so texture handlers can store temp views.
	WGUniformSet *us = new WGUniformSet();
	us->set_index = p_set_index;

	// Track WGPUBuffer handles that have already been bound as STORAGE_BUFFER in this set.
	// WebGPU forbids aliased writable storage buffer bindings in a single dispatch
	// (Vulkan allows this via barriers). When the same buffer appears a second time,
	// redirect it to aliasing_stub_buffer so the bind group passes validation.
	HashMap<WGPUBuffer, uint32_t> dup_storage_seen; // buffer handle → first uniform index

	for (uint32_t i = 0; i < p_uniforms.size(); i++) {
		const BoundUniform &uniform = p_uniforms[i];
		// WebGPU has no immutable sampler concept — always provide the sampler
		// in the bind group (unlike Vulkan where it's baked into the pipeline layout).

		switch (uniform.type) {
			case UNIFORM_TYPE_SAMPLER: {
				// NAGA flattens binding arrays to single resources, so only provide
				// the first sampler. Multiple IDs at the same binding means a
				// binding array which is reduced to 1 element.
				if (uniform.ids.size() > 0) {
					WGPUBindGroupEntry entry = {};
					entry.binding = uniform.binding * 2;
					entry.sampler = (WGPUSampler)(uniform.ids[0].id);
					entries.push_back(entry);
				}
			} break;

			case UNIFORM_TYPE_TEXTURE:
			case UNIFORM_TYPE_INPUT_ATTACHMENT: {
				// NAGA flattens binding arrays to single resources.
				// Only provide the first texture for array bindings.
				WGPUTextureViewDimension expected_dim = WGPUTextureViewDimension_Undefined;
				WGPUTextureSampleType expected_sample = WGPUTextureSampleType_Undefined;
				if (p_set_index < (uint32_t)shader->bind_group_infos.size()) {
					for (const auto &bge : shader->bind_group_infos[p_set_index].entries) {
						if (bge.layout_entry.binding == uniform.binding * 2) {
							expected_dim = bge.layout_entry.texture.viewDimension;
							expected_sample = bge.layout_entry.texture.sampleType;
							break;
						}
					}
				}
				if (uniform.ids.size() > 0) {
					WGTexture *tex = (WGTexture *)(uniform.ids[0].id);
					if (tex != nullptr) {
						WGPUBindGroupEntry entry = {};
						entry.binding = uniform.binding * 2;

						// Fix depth/float mismatch: if layout expects Float but
						// texture has a depth format (common with Godot's depth
						// fallback textures), substitute a float fallback texture.
						if (expected_sample == WGPUTextureSampleType_Float &&
								_is_depth_format(tex->format) &&
								fallback_float_texture_view != nullptr) {
							// Also check if we need cube dimension for the depth fallback.
							if (expected_dim == WGPUTextureViewDimension_Cube && fallback_cube_texture_view != nullptr) {
								entry.textureView = fallback_cube_texture_view;
							} else {
								entry.textureView = fallback_float_texture_view;
							}
						} else if (expected_dim != WGPUTextureViewDimension_Undefined &&
								expected_dim != tex->view_dimension) {
							// Fix dimension mismatch: if the layout expects a different dimension
							// than the texture's default view (e.g., 2D vs Cube), create a
							// compatible view or use a fallback texture.
							if (expected_dim == WGPUTextureViewDimension_Cube && tex->layers < 6 &&
									fallback_cube_texture_view != nullptr) {
								// Can't create a cube view from a texture with < 6 layers.
								entry.textureView = fallback_cube_texture_view;
							} else if (tex->view_source != nullptr) {
								WGPUTextureViewDescriptor vd = {};
								vd.format = tex->format;
								vd.dimension = expected_dim;
								vd.baseMipLevel = 0;
								vd.mipLevelCount = tex->mipmaps;
								vd.baseArrayLayer = 0;
								vd.arrayLayerCount = (expected_dim == WGPUTextureViewDimension_2D) ? 1 : tex->layers;
								vd.aspect = WGPUTextureAspect_All;
								WGPUTextureView fixed_view = wgpuTextureCreateView(tex->view_source, &vd);
								if (fixed_view) {
									entry.textureView = fixed_view;
									us->temp_views.push_back(fixed_view);
								} else {
									entry.textureView = tex->default_view;
								}
							} else {
								entry.textureView = tex->default_view;
							}
						} else {
							entry.textureView = tex->default_view;
						}
						us->bound_textures[entry.binding] = tex;
						entries.push_back(entry);
					}
				}
			} break;

			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
				// Godot pairs sampler+texture as ids[j*2] / ids[j*2+1].
				// WebGPU needs separate sampler and texture entries.
				// Sampler at binding*2+j*2+0, texture at binding*2+j*2+1 (matches layout and SPIR-V preprocessor).
				// Look up expected texture dimension and sample type from the shader layout.
				WGPUTextureViewDimension swt_expected_dim = WGPUTextureViewDimension_Undefined;
				WGPUTextureSampleType swt_expected_sample = WGPUTextureSampleType_Undefined;
				if (p_set_index < (uint32_t)shader->bind_group_infos.size()) {
					uint32_t tex_binding = uniform.binding * 2 + 1;
					for (const auto &bge : shader->bind_group_infos[p_set_index].entries) {
						if (bge.layout_entry.binding == tex_binding) {
							swt_expected_dim = bge.layout_entry.texture.viewDimension;
							swt_expected_sample = bge.layout_entry.texture.sampleType;
							break;
						}
					}
				}
				for (uint32_t j = 0; j < uniform.ids.size() / 2; j++) {
					WGPUSampler sampler = (WGPUSampler)(uniform.ids[j * 2 + 0].id);
					WGTexture *tex = (WGTexture *)(uniform.ids[j * 2 + 1].id);
					if (sampler) {
						WGPUBindGroupEntry se = {};
						se.binding = uniform.binding * 2 + j * 2 + 0;
						se.sampler = sampler;
						entries.push_back(se);
					}
					if (tex && tex->default_view) {
						WGPUBindGroupEntry te = {};
						te.binding = uniform.binding * 2 + j * 2 + 1;
						// Fix depth/float mismatch: combined sampler+texture bindings
						// are always Float. If a depth fallback texture is provided,
						// substitute a float fallback.
						if (_is_depth_format(tex->format) && fallback_float_texture_view != nullptr) {
							if (swt_expected_dim == WGPUTextureViewDimension_Cube && fallback_cube_texture_view != nullptr) {
								te.textureView = fallback_cube_texture_view;
							} else {
								te.textureView = fallback_float_texture_view;
							}
						} else if (swt_expected_dim != WGPUTextureViewDimension_Undefined &&
								swt_expected_dim != tex->view_dimension) {
							// Fix dimension mismatch (e.g., Cube↔2D with fallback textures).
							if (swt_expected_dim == WGPUTextureViewDimension_Cube && tex->layers < 6 &&
									fallback_cube_texture_view != nullptr) {
								te.textureView = fallback_cube_texture_view;
							} else if (tex->view_source != nullptr) {
								WGPUTextureViewDescriptor vd = {};
								vd.format = tex->format;
								vd.dimension = swt_expected_dim;
								vd.baseMipLevel = 0;
								vd.mipLevelCount = tex->mipmaps;
								vd.baseArrayLayer = 0;
								vd.arrayLayerCount = (swt_expected_dim == WGPUTextureViewDimension_2D) ? 1 : tex->layers;
								vd.aspect = WGPUTextureAspect_All;
								WGPUTextureView fixed_view = wgpuTextureCreateView(tex->view_source, &vd);
								if (fixed_view) {
									te.textureView = fixed_view;
									us->temp_views.push_back(fixed_view);
								} else {
									te.textureView = tex->default_view;
								}
							} else {
								te.textureView = tex->default_view;
							}
						} else {
							te.textureView = tex->default_view;
						}
						us->bound_textures[te.binding] = tex;
						entries.push_back(te);
					}
				}
			} break;

			case UNIFORM_TYPE_IMAGE: {
				// NAGA flattens binding arrays to single resources.
				// Only provide the first image for array bindings.
				if (uniform.ids.size() > 0) {
					WGTexture *tex = (WGTexture *)(uniform.ids[0].id);
					if (tex != nullptr) {
						WGPUBindGroupEntry entry = {};
						entry.binding = uniform.binding * 2;
						entry.textureView = tex->default_view;
						us->bound_textures[entry.binding] = tex;
						entries.push_back(entry);
					}
				}
			} break;

			case UNIFORM_TYPE_UNIFORM_BUFFER: {
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in uniform set.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding * 2; // NAGA doubles all non-combined bindings.
				entry.buffer = buf->handle;
				entry.offset = 0;
				entry.size = buf->size;
				entries.push_back(entry);
			} break;

			case UNIFORM_TYPE_STORAGE_BUFFER: {
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in storage uniform.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding * 2; // NAGA doubles all non-combined bindings.
				entry.offset = 0;
				// Alias detection: if this exact WGPUBuffer handle was already added as a
				// storage binding in this set, redirect the duplicate to the stub buffer.
				// This avoids the WebGPU "writable storage buffer aliasing" validation error
				// that fires when e.g. the particle system passes the same buffer for both
				// SourceEmission (binding 2) and DestEmission (binding 3).
				if (aliasing_stub_buffer && dup_storage_seen.has(buf->handle)) {
					static bool alias_stub_logged = false;
					if (!alias_stub_logged) {
						alias_stub_logged = true;
						EM_ASM({ console.warn('[ALIAS-STUB] Writable storage buffer aliasing at set=' + $0 + ' binding=' + $1 + ', redirected to stub buffer'); },
								(int)p_set_index, (int)uniform.binding);
					}
					entry.buffer = aliasing_stub_buffer;
					entry.size = ALIASING_STUB_BUFFER_SIZE;
				} else {
					dup_storage_seen[buf->handle] = i;
					entry.buffer = buf->handle;
					entry.size = buf->size;
				}
				entries.push_back(entry);
			} break;

			case UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in dynamic uniform.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding * 2;
				entry.buffer = buf->handle;
				entry.offset = 0;
				entry.size = buf->size;
				entries.push_back(entry);
			} break;

			case UNIFORM_TYPE_TEXTURE_BUFFER:
			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER: {
				// NAGA converts texture buffers to uniform/storage buffers. Bind as buffer.
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in texture buffer uniform.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding * 2;
				entry.buffer = buf->handle;
				entry.offset = 0;
				entry.size = buf->size;
				entries.push_back(entry);
			} break;

			case UNIFORM_TYPE_IMAGE_BUFFER: {
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in image buffer uniform.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding * 2;
				entry.buffer = buf->handle;
				entry.offset = 0;
				entry.size = buf->size;
				entries.push_back(entry);
			} break;

			default: {
				WARN_PRINT_ONCE(vformat("WebGPU: unhandled uniform type %d in uniform_set_create.", (int)uniform.type));
			} break;
		}
	}

	// Add depth alias bind group entries: for each depth alias binding, provide
	// the same texture view as the paired depth texture at (alias_binding - 1).
	for (const KeyValue<uint32_t, uint32_t> &kv : shader->depth_alias_bindings) {
		uint32_t alias_grp = kv.key >> 16;
		uint32_t alias_bnd = kv.key & 0xFFFF;
		uint32_t depth_bnd = kv.value;
		if (alias_grp != p_set_index) {
			continue;
		}
		// Find the texture view from the depth texture entry.
		WGPUTextureView alias_view = nullptr;
		for (const auto &e : entries) {
			if (e.binding == depth_bnd && e.textureView != nullptr) {
				alias_view = e.textureView;
				break;
			}
		}
		if (alias_view) {
			WGPUBindGroupEntry alias_entry = {};
			alias_entry.binding = alias_bnd;
			// The alias is supposed to sample depth as Float, but alias_view may
			// point at a Depth-format texture. Substitute the float fallback.
			if (alias_view == fallback_float_texture_view) {
				alias_entry.textureView = alias_view; // Already substituted.
			} else {
				// Check if the source texture view came from a depth-format texture.
				// We can't query the view's format, so check if the alias BGL expects Float.
				// For safety, always use fallback for depth alias entries since
				// they always represent Float sampling of depth data.
				if (fallback_float_texture_view != nullptr) {
					alias_entry.textureView = fallback_float_texture_view;
				} else {
					alias_entry.textureView = alias_view;
				}
			}
			entries.push_back(alias_entry);
		}
	}

	// If this is the merged PC group, inject the push constant ring buffer at PUSH_CONSTANT_RING_BINDING.
	// Dynamic offset is 0 here; _flush_push_constants will rebind with the correct offset.
	if (is_pc_group) {
		WGPUBindGroupEntry pc_entry = {};
		pc_entry.binding = PUSH_CONSTANT_RING_BINDING;
		pc_entry.buffer = push_constant_ring_buffer;
		pc_entry.offset = 0;
		pc_entry.size = PUSH_CONSTANT_SLOT_ALIGNMENT;
		entries.push_back(pc_entry);
	}

	// Log any duplicate binding indices in bind group entries.
	for (uint32_t a = 0; a < entries.size(); a++) {
		for (uint32_t b = a + 1; b < entries.size(); b++) {
			if (entries[a].binding == entries[b].binding) {
				EM_ASM({ console.error('[BG-DUP] set=' + $0 + ' binding=' + $1 + ' idx_a=' + $2 + ' idx_b=' + $3); },
						(int)p_set_index, (int)entries[a].binding, (int)a, (int)b);
			}
		}
	}

	WGPUBindGroupDescriptor bg_desc = {};
	bg_desc.layout = layout;
	bg_desc.entryCount = entries.size();
	bg_desc.entries = entries.size() > 0 ? entries.ptr() : nullptr;

	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device, &bg_desc);
	if (bg == nullptr) {
		delete us;
		ERR_FAIL_V_MSG(UniformSetID(), "WebGPU: wgpuDeviceCreateBindGroup failed.");
	}

	us->handle = bg;
	// Cache entries and source shader for potential BGL rebinding.
	us->cached_entries.resize(entries.size());
	for (uint32_t i = 0; i < entries.size(); i++) {
		us->cached_entries[i] = entries[i];
	}
	us->source_shader = shader;
	return UniformSetID(us);
}

WGPUBindGroup RenderingDeviceDriverWebGPU::_get_compatible_bind_group(WGUniformSet *p_us, WGShader *p_target_shader, uint32_t p_set_idx) {
	// Fast path: same shader or no shader info.
	if (!p_target_shader || p_us->source_shader == p_target_shader) {
		return p_us->handle;
	}
	if (p_set_idx >= (uint32_t)p_target_shader->bind_group_layouts.size()) {
		return p_us->handle;
	}

	// Check if the pipeline shader uses the same BGL or a merged PC layout.
	WGPUBindGroupLayout target_layout = p_target_shader->bind_group_layouts[p_set_idx];
	if (p_target_shader->merged_pc_group_layout && p_set_idx == p_target_shader->push_constant_bind_group) {
		target_layout = p_target_shader->merged_pc_group_layout;
	}

	// If source shader uses the same BGL, no rebind needed.
	WGPUBindGroupLayout source_layout = nullptr;
	if (p_us->source_shader && p_set_idx < (uint32_t)p_us->source_shader->bind_group_layouts.size()) {
		source_layout = p_us->source_shader->bind_group_layouts[p_set_idx];
		if (p_us->source_shader->merged_pc_group_layout && p_set_idx == p_us->source_shader->push_constant_bind_group) {
			source_layout = p_us->source_shader->merged_pc_group_layout;
		}
	}
	if (source_layout == target_layout) {
		return p_us->handle;
	}

	// Check rebind cache.
	if (p_us->rebind_cache.has(target_layout)) {
		return p_us->rebind_cache[target_layout];
	}

	// Build adapted entries: copy cached entries and fix sampler type mismatches.
	LocalVector<WGPUBindGroupEntry> adapted;
	adapted.resize(p_us->cached_entries.size());
	for (uint32_t i = 0; i < p_us->cached_entries.size(); i++) {
		adapted[i] = p_us->cached_entries[i];
	}

	// Check sampler and texture entries for type mismatches between source and target BGLs.
	if (p_set_idx < (uint32_t)p_target_shader->bind_group_infos.size()) {
		for (auto &entry : adapted) {
			// --- Sampler adaptation ---
			if (entry.sampler != nullptr) {
				WGPUSamplerBindingType target_samp_type = WGPUSamplerBindingType_BindingNotUsed;
				for (const auto &bge : p_target_shader->bind_group_infos[p_set_idx].entries) {
					if (bge.layout_entry.binding == entry.binding &&
							bge.layout_entry.sampler.type != WGPUSamplerBindingType_BindingNotUsed) {
						target_samp_type = bge.layout_entry.sampler.type;
						break;
					}
				}
				// Fallback: check source shader for SWT sampler info.
				if (target_samp_type == WGPUSamplerBindingType_BindingNotUsed &&
						p_us->source_shader && p_set_idx < (uint32_t)p_us->source_shader->bind_group_infos.size()) {
					for (const auto &bge : p_us->source_shader->bind_group_infos[p_set_idx].entries) {
						if (bge.layout_entry.binding == entry.binding &&
								bge.layout_entry.sampler.type != WGPUSamplerBindingType_BindingNotUsed) {
							target_samp_type = bge.layout_entry.sampler.type;
							break;
						}
					}
				}

				WGPUSamplerBindingType source_samp_type = WGPUSamplerBindingType_BindingNotUsed;
				if (p_us->source_shader && p_set_idx < (uint32_t)p_us->source_shader->bind_group_infos.size()) {
					for (const auto &bge : p_us->source_shader->bind_group_infos[p_set_idx].entries) {
						if (bge.layout_entry.binding == entry.binding &&
								bge.layout_entry.sampler.type != WGPUSamplerBindingType_BindingNotUsed) {
							source_samp_type = bge.layout_entry.sampler.type;
							break;
						}
					}
				}

				if ((target_samp_type == WGPUSamplerBindingType_Filtering ||
						target_samp_type == WGPUSamplerBindingType_NonFiltering) &&
						source_samp_type == WGPUSamplerBindingType_Comparison && dummy_filtering_sampler) {
					entry.sampler = dummy_filtering_sampler;
				} else if (target_samp_type == WGPUSamplerBindingType_Comparison &&
						source_samp_type != WGPUSamplerBindingType_Comparison && dummy_comparison_sampler) {
					entry.sampler = dummy_comparison_sampler;
				}
			}

			// --- Texture view adaptation ---
			// If the target BGL expects Float but the entry has a depth-format texture view,
			// substitute the fallback float texture view.
			if (entry.textureView != nullptr) {
				WGPUTextureSampleType target_tex_sample = WGPUTextureSampleType_BindingNotUsed;
				for (const auto &bge : p_target_shader->bind_group_infos[p_set_idx].entries) {
					if (bge.layout_entry.binding == entry.binding &&
							bge.layout_entry.texture.sampleType != WGPUTextureSampleType_BindingNotUsed) {
						target_tex_sample = bge.layout_entry.texture.sampleType;
						break;
					}
				}
				// Check source BGL to detect if the texture was originally depth.
				WGPUTextureSampleType source_tex_sample = WGPUTextureSampleType_BindingNotUsed;
				if (p_us->source_shader && p_set_idx < (uint32_t)p_us->source_shader->bind_group_infos.size()) {
					for (const auto &bge : p_us->source_shader->bind_group_infos[p_set_idx].entries) {
						if (bge.layout_entry.binding == entry.binding &&
								bge.layout_entry.texture.sampleType != WGPUTextureSampleType_BindingNotUsed) {
							source_tex_sample = bge.layout_entry.texture.sampleType;
							break;
						}
					}
				}
				// Source was Depth, target expects Float → substitute fallback.
				if (source_tex_sample == WGPUTextureSampleType_Depth &&
						target_tex_sample == WGPUTextureSampleType_Float &&
						fallback_float_texture_view != nullptr) {
					entry.textureView = fallback_float_texture_view;
				}

				// --- Texture view dimension adaptation ---
				// If the target BGL expects a different dimension (e.g. 2D vs Cube),
				// create a compatible view from the original texture.
				WGPUTextureViewDimension target_dim = WGPUTextureViewDimension_Undefined;
				for (const auto &bge : p_target_shader->bind_group_infos[p_set_idx].entries) {
					if (bge.layout_entry.binding == entry.binding &&
							bge.layout_entry.texture.viewDimension != WGPUTextureViewDimension_Undefined) {
						target_dim = bge.layout_entry.texture.viewDimension;
						break;
					}
				}
				if (target_dim != WGPUTextureViewDimension_Undefined &&
						p_us->bound_textures.has(entry.binding)) {
					WGTexture *tex = p_us->bound_textures[entry.binding];
					if (tex && tex->view_dimension != target_dim && tex->view_source) {
						WGPUTextureViewDescriptor vd = {};
						vd.format = tex->format;
						vd.dimension = target_dim;
						vd.baseMipLevel = 0;
						vd.mipLevelCount = tex->mipmaps;
						vd.baseArrayLayer = 0;
						vd.arrayLayerCount = (target_dim == WGPUTextureViewDimension_2D) ? 1 : tex->layers;
						vd.aspect = WGPUTextureAspect_All;
						WGPUTextureView fixed = wgpuTextureCreateView(tex->view_source, &vd);
						if (fixed) {
							entry.textureView = fixed;
							p_us->temp_views.push_back(fixed);
						}
					}
				}
			}
		}
	}

	// Collect valid binding indices from the target BGL.
	// The target BGL includes entries from bind_group_infos (reflecting Godot uniforms)
	// plus depth alias entries and potentially a push constant entry.
	HashSet<uint32_t> target_bindings;
	if (p_set_idx < (uint32_t)p_target_shader->bind_group_infos.size()) {
		for (const auto &bge : p_target_shader->bind_group_infos[p_set_idx].entries) {
			target_bindings.insert(bge.layout_entry.binding);
			// For SWT, the layout_entry stores the texture binding; the sampler is at binding-1.
			if (bge.godot_type == RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE) {
				target_bindings.insert(bge.layout_entry.binding - 1); // Sampler binding.
			}
		}
	}
	// Add depth alias bindings.
	for (const KeyValue<uint32_t, uint32_t> &kv : p_target_shader->depth_alias_bindings) {
		uint32_t alias_grp = kv.key >> 16;
		uint32_t alias_bnd = kv.key & 0xFFFF;
		if (alias_grp == p_set_idx) {
			target_bindings.insert(alias_bnd);
		}
	}
	// Add push constant ring buffer binding if this is the PC group.
	if (p_target_shader->merged_pc_group_layout && p_set_idx == p_target_shader->push_constant_bind_group) {
		target_bindings.insert(PUSH_CONSTANT_RING_BINDING);
	}

	// Filter adapted entries: only keep entries whose binding exists in the target BGL.
	LocalVector<WGPUBindGroupEntry> filtered;
	for (const auto &e : adapted) {
		if (target_bindings.has(e.binding)) {
			filtered.push_back(e);
		}
	}

	// Create re-bound bind group with target layout.
	perf.bind_group_cache_misses++;
	WGPUBindGroupDescriptor desc = {};
	desc.layout = target_layout;
	desc.entryCount = filtered.size();
	desc.entries = filtered.size() > 0 ? filtered.ptr() : nullptr;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device, &desc);
	if (!bg) {
		EM_ASM({
			console.error('[REBIND-FAIL] set=' + $0 + ' src=' + UTF8ToString($1) + ' tgt=' + UTF8ToString($2) + ' entries=' + $3);
		}, (int)p_set_idx,
			p_us->source_shader ? p_us->source_shader->name.utf8().get_data() : "null",
			p_target_shader->name.utf8().get_data(),
			(int)filtered.size());
	}
	p_us->rebind_cache[target_layout] = bg; // Cache even if null.
	return bg ? bg : p_us->handle;
}

void RenderingDeviceDriverWebGPU::uniform_set_free(UniformSetID p_uniform_set) {
	WGUniformSet *us = (WGUniformSet *)(p_uniform_set.id);
	ERR_FAIL_NULL(us);
	for (WGPUTextureView v : us->temp_views) {
		if (v) {
			wgpuTextureViewRelease(v);
		}
	}
	// Release cached rebind bind groups.
	for (const KeyValue<WGPUBindGroupLayout, WGPUBindGroup> &kv : us->rebind_cache) {
		if (kv.value) {
			wgpuBindGroupRelease(kv.value);
		}
	}
	if (us->handle) {
		wgpuBindGroupRelease(us->handle);
	}
	delete us;
}

uint32_t RenderingDeviceDriverWebGPU::uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const {
	// TODO: Calculate and return dynamic offsets for dynamic uniform/storage buffers.
	return 0;
}

void RenderingDeviceDriverWebGPU::command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) {
	// No-op: WebGPU doesn't need explicit preparation.
}

// =============================================================================
// TRANSFER
// =============================================================================

void RenderingDeviceDriverWebGPU::command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(buf);

	cmd->end_active_encoder();

	uint64_t size = (p_size == BUFFER_WHOLE_SIZE) ? (buf->size - p_offset) : p_size;
	size = (size + 3) & ~3ULL; // Must be multiple of 4.
	wgpuCommandEncoderClearBuffer(cmd->encoder, buf->handle, p_offset, size);
}

void RenderingDeviceDriverWebGPU::command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *src = (WGBuffer *)(p_src_buffer.id);
	WGBuffer *dst = (WGBuffer *)(p_dst_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(src);
	ERR_FAIL_NULL(dst);

	// If the source buffer has a shadow map (CPU-side staging buffer), write
	// directly to the destination GPU buffer via the queue.  The GPU staging
	// buffer was never populated because WebGPU buffer mapping is async
	// and the shadow map is our CPU fallback.
	if (src->shadow_map) {
		for (uint32_t i = 0; i < p_regions.size(); i++) {
			const BufferCopyRegion &region = p_regions[i];
			uint64_t size = (region.size + 3) & ~3ULL;
			wgpuQueueWriteBuffer(queue, dst->handle, region.dst_offset, src->shadow_map + region.src_offset, size);
		}
		return;
	}

	cmd->end_active_encoder();

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferCopyRegion &region = p_regions[i];
		uint64_t size = (region.size + 3) & ~3ULL;
		wgpuCommandEncoderCopyBufferToBuffer(cmd->encoder, src->handle, region.src_offset, dst->handle, region.dst_offset, size);
	}
}

void RenderingDeviceDriverWebGPU::command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGTexture *src = (WGTexture *)(p_src_texture.id);
	WGTexture *dst = (WGTexture *)(p_dst_texture.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(src);
	ERR_FAIL_NULL(dst);

	cmd->end_active_encoder();

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const TextureCopyRegion &region = p_regions[i];

		WGPUTexelCopyTextureInfo src_copy = {};
		src_copy.texture = src->handle;
		src_copy.mipLevel = region.src_subresources.mipmap;
		src_copy.origin = { (uint32_t)region.src_offset.x, (uint32_t)region.src_offset.y, region.src_subresources.base_layer };
		src_copy.aspect = WGPUTextureAspect_All;

		WGPUTexelCopyTextureInfo dst_copy = {};
		dst_copy.texture = dst->handle;
		dst_copy.mipLevel = region.dst_subresources.mipmap;
		dst_copy.origin = { (uint32_t)region.dst_offset.x, (uint32_t)region.dst_offset.y, region.dst_subresources.base_layer };
		dst_copy.aspect = WGPUTextureAspect_All;

		WGPUExtent3D extent = { (uint32_t)region.size.x, (uint32_t)region.size.y, (uint32_t)region.size.z };

		wgpuCommandEncoderCopyTextureToTexture(cmd->encoder, &src_copy, &dst_copy, &extent);
	}
}

void RenderingDeviceDriverWebGPU::command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) {
	// TODO: Create a minimal render pass with MSAA texture as color attachment
	// and the resolve target as resolveTarget. Begin and immediately end the pass.
	WARN_PRINT_ONCE("WebGPU: command_resolve_texture not yet implemented.");
}

void RenderingDeviceDriverWebGPU::command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGTexture *tex = (WGTexture *)(p_texture.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(tex);

	cmd->end_active_encoder();

	for (uint32_t mip = p_subresources.base_mipmap; mip < p_subresources.base_mipmap + p_subresources.mipmap_count; mip++) {
		for (uint32_t layer = p_subresources.base_layer; layer < p_subresources.base_layer + p_subresources.layer_count; layer++) {
			WGPUTextureViewDescriptor view_desc = {};
			view_desc.format = tex->format;
			view_desc.dimension = WGPUTextureViewDimension_2D;
			view_desc.baseMipLevel = mip;
			view_desc.mipLevelCount = 1;
			view_desc.baseArrayLayer = layer;
			view_desc.arrayLayerCount = 1;
			view_desc.aspect = WGPUTextureAspect_All;

			WGPUTextureView view = wgpuTextureCreateView(tex->view_source, &view_desc);

			WGPURenderPassColorAttachment color_att = {};
			color_att.view = view;
			color_att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			color_att.loadOp = WGPULoadOp_Clear;
			color_att.storeOp = WGPUStoreOp_Store;
			color_att.clearValue = { p_color.r, p_color.g, p_color.b, p_color.a };

			WGPURenderPassDescriptor rp_desc = {};
			rp_desc.colorAttachmentCount = 1;
			rp_desc.colorAttachments = &color_att;

			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(cmd->encoder, &rp_desc);
			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
			wgpuTextureViewRelease(view);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGTexture *tex = (WGTexture *)(p_texture.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(tex);

	cmd->end_active_encoder();

	bool has_depth = is_depth_format_wgpu(tex->format);
	bool has_stencil = has_stencil_wgpu(tex->format);

	for (uint32_t mip = p_subresources.base_mipmap; mip < p_subresources.base_mipmap + p_subresources.mipmap_count; mip++) {
		for (uint32_t layer = p_subresources.base_layer; layer < p_subresources.base_layer + p_subresources.layer_count; layer++) {
			WGPUTextureViewDescriptor view_desc = {};
			view_desc.format = tex->format;
			view_desc.dimension = WGPUTextureViewDimension_2D;
			view_desc.baseMipLevel = mip;
			view_desc.mipLevelCount = 1;
			view_desc.baseArrayLayer = layer;
			view_desc.arrayLayerCount = 1;
			view_desc.aspect = WGPUTextureAspect_All;

			WGPUTextureView view = wgpuTextureCreateView(tex->view_source, &view_desc);

			WGPURenderPassDepthStencilAttachment ds_att = {};
			ds_att.view = view;
			if (has_depth) {
				ds_att.depthLoadOp = WGPULoadOp_Clear;
				ds_att.depthStoreOp = WGPUStoreOp_Store;
				ds_att.depthClearValue = p_depth;
			} else {
				ds_att.depthLoadOp = WGPULoadOp_Undefined;
				ds_att.depthStoreOp = WGPUStoreOp_Undefined;
				ds_att.depthReadOnly = true;
			}
			if (has_stencil) {
				ds_att.stencilLoadOp = WGPULoadOp_Clear;
				ds_att.stencilStoreOp = WGPUStoreOp_Store;
				ds_att.stencilClearValue = p_stencil;
			} else {
				ds_att.stencilLoadOp = WGPULoadOp_Undefined;
				ds_att.stencilStoreOp = WGPUStoreOp_Undefined;
				ds_att.stencilReadOnly = true;
			}

			WGPURenderPassDescriptor rp_desc = {};
			rp_desc.depthStencilAttachment = &ds_att;

			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(cmd->encoder, &rp_desc);
			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
			wgpuTextureViewRelease(view);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *src = (WGBuffer *)(p_src_buffer.id);
	WGTexture *dst = (WGTexture *)(p_dst_texture.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(src);
	ERR_FAIL_NULL(dst);

	// If the source is a staging buffer (has shadow_map), flush shadow data
	// to the GPU buffer first.  WebGPU queue writes are ordered before
	// subsequent command buffer submissions, so this is safe.
	if (src->shadow_map) {
		wgpuQueueWriteBuffer(queue, src->handle, 0, src->shadow_map, src->size);
	}

	cmd->end_active_encoder();

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];

		// WGPUTexelCopyBufferInfo combines the buffer handle + layout (Dawn API)
		WGPUTexelCopyBufferInfo src_info = {};
		src_info.buffer = src->handle;
		src_info.layout.offset = region.buffer_offset;
		src_info.layout.bytesPerRow = ((region.row_pitch + 255) / 256) * 256; // 256-byte aligned.
		src_info.layout.rowsPerImage = region.texture_region_size.y;

		WGPUTexelCopyTextureInfo dst_copy = {};
		dst_copy.texture = dst->handle;
		dst_copy.mipLevel = region.texture_subresource.mipmap;
		dst_copy.origin = { (uint32_t)region.texture_offset.x, (uint32_t)region.texture_offset.y, region.texture_subresource.layer };
		dst_copy.aspect = WGPUTextureAspect_All;

		WGPUExtent3D extent = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, (uint32_t)region.texture_region_size.z };

		wgpuCommandEncoderCopyBufferToTexture(cmd->encoder, &src_info, &dst_copy, &extent);
	}
}

void RenderingDeviceDriverWebGPU::command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGTexture *src = (WGTexture *)(p_src_texture.id);
	WGBuffer *dst = (WGBuffer *)(p_dst_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(src);
	ERR_FAIL_NULL(dst);

	cmd->end_active_encoder();

	for (uint32_t i = 0; i < p_regions.size(); i++) {
		const BufferTextureCopyRegion &region = p_regions[i];

		WGPUTexelCopyTextureInfo src_copy = {};
		src_copy.texture = src->handle;
		src_copy.mipLevel = region.texture_subresource.mipmap;
		src_copy.origin = { (uint32_t)region.texture_offset.x, (uint32_t)region.texture_offset.y, region.texture_subresource.layer };
		src_copy.aspect = WGPUTextureAspect_All;

		// WGPUTexelCopyBufferInfo combines the buffer handle + layout (Dawn API)
		WGPUTexelCopyBufferInfo dst_info = {};
		dst_info.buffer = dst->handle;
		dst_info.layout.offset = region.buffer_offset;
		dst_info.layout.bytesPerRow = ((region.row_pitch + 255) / 256) * 256;
		dst_info.layout.rowsPerImage = region.texture_region_size.y;

		WGPUExtent3D extent = { (uint32_t)region.texture_region_size.x, (uint32_t)region.texture_region_size.y, (uint32_t)region.texture_region_size.z };

		wgpuCommandEncoderCopyTextureToBuffer(cmd->encoder, &src_copy, &dst_info, &extent);
	}
}

// =============================================================================
// PIPELINE
// =============================================================================

void RenderingDeviceDriverWebGPU::pipeline_free(PipelineID p_pipeline) {
	WGPipelineWrapper *pw = (WGPipelineWrapper *)(p_pipeline.id);
	ERR_FAIL_NULL(pw);
	if (pw->type == WGPipelineWrapper::RENDER && pw->render_handle) {
		wgpuRenderPipelineRelease(pw->render_handle);
	} else if (pw->type == WGPipelineWrapper::COMPUTE && pw->compute_handle) {
		wgpuComputePipelineRelease(pw->compute_handle);
	}
	// Release any specialized shader modules owned by this pipeline.
	for (int i = 0; i < 6; i++) {
		if (pw->specialized_modules[i]) {
			wgpuShaderModuleRelease(pw->specialized_modules[i]);
		}
	}
	delete pw;
}

void RenderingDeviceDriverWebGPU::command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	uint32_t offset = p_first_index * sizeof(uint32_t);
	uint32_t size = p_data.size() * sizeof(uint32_t);
	ERR_FAIL_COND(offset + size > WGCommandBuffer::MAX_PUSH_CONSTANT_SIZE);

	memcpy(cmd->push_constant_data + offset, &p_data[0], size);
	cmd->push_constant_data_len = MAX(cmd->push_constant_data_len, offset + size);
	cmd->push_constants_dirty = true;
}

void RenderingDeviceDriverWebGPU::_flush_push_constants(WGCommandBuffer *p_cmd_buf, WGShader *p_shader) {
	if (!p_cmd_buf->push_constants_dirty || p_cmd_buf->push_constant_data_len == 0 || !p_shader) {
		perf.push_constant_skipped++;
		return;
	}
	if (p_shader->push_constant_bind_group == UINT32_MAX || !push_constant_bind_group) {
		p_cmd_buf->push_constants_dirty = false;
		perf.push_constant_skipped++;
		return; // Shader has no push constants.
	}

	perf.push_constant_writes++;

	// Write push constant data to CPU shadow buffer (batched GPU write at submit time).
	uint32_t aligned_size = (p_cmd_buf->push_constant_data_len + PUSH_CONSTANT_SLOT_ALIGNMENT - 1) & ~(PUSH_CONSTANT_SLOT_ALIGNMENT - 1);

	if (push_constant_ring_offset + aligned_size > PUSH_CONSTANT_RING_SIZE) {
		// Flush the accumulated shadow buffer before wrapping.
		if (push_constant_shadow_dirty_start < push_constant_shadow_dirty_end) {
			wgpuQueueWriteBuffer(queue, push_constant_ring_buffer, push_constant_shadow_dirty_start,
					push_constant_shadow + push_constant_shadow_dirty_start,
					push_constant_shadow_dirty_end - push_constant_shadow_dirty_start);
			push_constant_shadow_dirty_start = UINT32_MAX;
			push_constant_shadow_dirty_end = 0;
		}
		push_constant_ring_offset = 0; // Wrap around.
	}

	memcpy(push_constant_shadow + push_constant_ring_offset, p_cmd_buf->push_constant_data, p_cmd_buf->push_constant_data_len);

	// Track dirty range for batched flush.
	if (push_constant_ring_offset < push_constant_shadow_dirty_start) {
		push_constant_shadow_dirty_start = push_constant_ring_offset;
	}
	uint32_t end = push_constant_ring_offset + aligned_size;
	if (end > push_constant_shadow_dirty_end) {
		push_constant_shadow_dirty_end = end;
	}

	uint32_t dynamic_offset = push_constant_ring_offset;

	// Choose which bind group to rebind at the PC group:
	// - If the shader has a merged layout (material + PC) AND the material uniform set
	//   was previously bound, use that combined bind group.
	// - Otherwise, fall back to the universal PC-only bind group.
	WGPUBindGroup pc_bind_group_to_use;
	if (p_shader->merged_pc_group_layout && p_cmd_buf->current_pc_bind_group != nullptr) {
		pc_bind_group_to_use = p_cmd_buf->current_pc_bind_group;
	} else {
		pc_bind_group_to_use = push_constant_bind_group;
	}

	// Bind the push constant ring buffer bind group with a dynamic offset.
	if (p_cmd_buf->render_encoder) {
		wgpuRenderPassEncoderSetBindGroup(p_cmd_buf->render_encoder, p_shader->push_constant_bind_group, pc_bind_group_to_use, 1, &dynamic_offset);
	} else if (p_cmd_buf->compute_encoder) {
		wgpuComputePassEncoderSetBindGroup(p_cmd_buf->compute_encoder, p_shader->push_constant_bind_group, pc_bind_group_to_use, 1, &dynamic_offset);
	}

	push_constant_ring_offset += aligned_size;
	p_cmd_buf->push_constants_dirty = false;
}

bool RenderingDeviceDriverWebGPU::pipeline_cache_create(const Vector<uint8_t> &p_data) {
	return true; // No-op.
}

void RenderingDeviceDriverWebGPU::pipeline_cache_free() {
	// No-op.
}

size_t RenderingDeviceDriverWebGPU::pipeline_cache_query_size() {
	// We don't implement a pipeline cache, but must return non-zero so
	// RenderingDevice::update_pipeline_cache() doesn't emit an error every frame.
	return 1;
}

Vector<uint8_t> RenderingDeviceDriverWebGPU::pipeline_cache_serialize() {
	return Vector<uint8_t>();
}

// =============================================================================
// RENDERING
// =============================================================================

RDD::RenderPassID RenderingDeviceDriverWebGPU::render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) {
	WGRenderPass *rp = new WGRenderPass();
	rp->view_count = p_view_count;

	for (uint32_t i = 0; i < p_attachments.size(); i++) {
		rp->attachments.push_back(p_attachments[i]);
	}

	for (uint32_t i = 0; i < p_subpasses.size(); i++) {
		WGRenderPass::SubpassInfo sp;
		sp.color_references = p_subpasses[i].color_references;
		sp.input_references = p_subpasses[i].input_references;
		sp.depth_stencil_reference = p_subpasses[i].depth_stencil_reference;
		sp.resolve_references = p_subpasses[i].resolve_references;
		rp->subpasses.push_back(sp);
	}

	return RenderPassID(rp);
}

void RenderingDeviceDriverWebGPU::render_pass_free(RenderPassID p_render_pass) {
	WGRenderPass *rp = (WGRenderPass *)(p_render_pass.id);
	delete rp;
}

void RenderingDeviceDriverWebGPU::command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGRenderPass *rp = (WGRenderPass *)(p_render_pass.id);
	WGFramebuffer *fb = (WGFramebuffer *)(p_framebuffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(rp);
	ERR_FAIL_NULL(fb);

	perf.render_passes++;

	// End any active encoder.
	cmd->end_active_encoder();

	// Invalidate bind group state tracking (new encoder = clean state).
	cmd->invalidate_bind_groups();

	// Store render state.
	cmd->render_state.render_pass = rp;
	cmd->render_state.framebuffer = fb;
	cmd->render_state.current_subpass = 0;
	cmd->render_state.render_area_width = p_rect.size.x > 0 ? (uint32_t)p_rect.size.x : fb->width;
	cmd->render_state.render_area_height = p_rect.size.y > 0 ? (uint32_t)p_rect.size.y : fb->height;

	ERR_FAIL_COND(rp->subpasses.size() == 0);
	const WGRenderPass::SubpassInfo &subpass = rp->subpasses[0];

	// --- Helper lambdas for op mapping ---
	auto map_load_op = [](AttachmentLoadOp op) -> WGPULoadOp {
		switch (op) {
			case ATTACHMENT_LOAD_OP_LOAD: return WGPULoadOp_Load;
			case ATTACHMENT_LOAD_OP_CLEAR: return WGPULoadOp_Clear;
			default: return WGPULoadOp_Clear; // DONT_CARE → Clear (WebGPU has no DONT_CARE; Clear is safest)
		}
	};
	auto map_store_op = [](AttachmentStoreOp op) -> WGPUStoreOp {
		switch (op) {
			case ATTACHMENT_STORE_OP_STORE: return WGPUStoreOp_Store;
			default: return WGPUStoreOp_Discard; // DONT_CARE
		}
	};

	// --- Build color attachments ---
	// clear_values is indexed by attachment index, matching Vulkan/Godot convention.
	LocalVector<WGPURenderPassColorAttachment> color_attachments;
	for (uint32_t i = 0; i < subpass.color_references.size(); i++) {
		const RDD::AttachmentReference &ref = subpass.color_references[i];

		WGPURenderPassColorAttachment att = {};
		att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

		if (ref.attachment == RDD::AttachmentReference::UNUSED) {
			// Unused attachment slot: provide a dummy null entry.
			att.view = nullptr;
			att.loadOp = WGPULoadOp_Load;
			att.storeOp = WGPUStoreOp_Discard;
			color_attachments.push_back(att);
			continue;
		}

		// Attachment view from framebuffer.
		if (ref.attachment < fb->attachment_views.size()) {
			att.view = fb->attachment_views[ref.attachment];
		}

		// Resolve target (MSAA).
		if (i < subpass.resolve_references.size()) {
			const RDD::AttachmentReference &res_ref = subpass.resolve_references[i];
			if (res_ref.attachment != RDD::AttachmentReference::UNUSED &&
					res_ref.attachment < fb->attachment_views.size()) {
				att.resolveTarget = fb->attachment_views[res_ref.attachment];
			}
		}

		// Load/store ops and clear value from attachment description.
		if (ref.attachment < rp->attachments.size()) {
			const RDD::Attachment &attach_desc = rp->attachments[ref.attachment];
			att.loadOp = map_load_op(attach_desc.load_op);
			att.storeOp = map_store_op(attach_desc.store_op);

			if (att.loadOp == WGPULoadOp_Clear && ref.attachment < p_clear_values.size()) {
				const Color &c = p_clear_values[ref.attachment].color;
				att.clearValue = { c.r, c.g, c.b, c.a };
			}

			// WebGPU swap chain textures have undefined content each frame.
			// Force LOAD→CLEAR so the alpha channel starts at 1.0 (opaque).
			if (rp->is_swap_chain_pass && att.loadOp == WGPULoadOp_Load) {
				att.loadOp = WGPULoadOp_Clear;
				att.clearValue = { 0.0, 0.0, 0.0, 1.0 };
			}
		} else {
			att.loadOp = WGPULoadOp_Load;
			att.storeOp = WGPUStoreOp_Store;
		}

		color_attachments.push_back(att);
	}

	// --- Build depth/stencil attachment ---
	WGPURenderPassDepthStencilAttachment ds_att = {};
	WGPURenderPassDepthStencilAttachment *ds_att_ptr = nullptr;

	const RDD::AttachmentReference &ds_ref = subpass.depth_stencil_reference;
	if (ds_ref.attachment != RDD::AttachmentReference::UNUSED &&
			ds_ref.attachment < fb->attachment_views.size()) {
		ds_att.view = fb->attachment_views[ds_ref.attachment];

		if (ds_ref.attachment < rp->attachments.size()) {
			const RDD::Attachment &attach_desc = rp->attachments[ds_ref.attachment];
			WGPUTextureFormat wgpu_fmt = _data_format_to_wgpu(attach_desc.format);
			bool has_depth = is_depth_format_wgpu(wgpu_fmt);
			bool has_stencil = has_stencil_wgpu(wgpu_fmt);

			if (has_depth) {
				ds_att.depthLoadOp = map_load_op(attach_desc.load_op);
				ds_att.depthStoreOp = map_store_op(attach_desc.store_op);
				if (ds_att.depthLoadOp == WGPULoadOp_Clear && ds_ref.attachment < p_clear_values.size()) {
					ds_att.depthClearValue = p_clear_values[ds_ref.attachment].depth;
				} else {
					ds_att.depthClearValue = 1.0f;
				}
			} else {
				ds_att.depthLoadOp = WGPULoadOp_Undefined;
				ds_att.depthStoreOp = WGPUStoreOp_Undefined;
				ds_att.depthReadOnly = true;
			}

			if (has_stencil) {
				ds_att.stencilLoadOp = map_load_op(attach_desc.stencil_load_op);
				ds_att.stencilStoreOp = map_store_op(attach_desc.stencil_store_op);
				if (ds_att.stencilLoadOp == WGPULoadOp_Clear && ds_ref.attachment < p_clear_values.size()) {
					ds_att.stencilClearValue = p_clear_values[ds_ref.attachment].stencil;
				}
			} else {
				ds_att.stencilLoadOp = WGPULoadOp_Undefined;
				ds_att.stencilStoreOp = WGPUStoreOp_Undefined;
				ds_att.stencilReadOnly = true;
			}
		} else {
			// Fallback: load and store everything.
			ds_att.depthLoadOp = WGPULoadOp_Load;
			ds_att.depthStoreOp = WGPUStoreOp_Store;
			ds_att.depthClearValue = 1.0f;
			ds_att.stencilLoadOp = WGPULoadOp_Load;
			ds_att.stencilStoreOp = WGPUStoreOp_Store;
		}
		ds_att_ptr = &ds_att;
	}

	// --- Begin render pass ---
	// --- Diagnostic: verify swap chain view maps to correct JS object ---
	if (rp->is_swap_chain_pass && color_attachments.size() > 0) {
		static int _sc_view_log = 0;
		if (_sc_view_log < 5) {
			WGPUTextureView view = color_attachments[0].view;
			int load_op_int = (int)color_attachments[0].loadOp;
			int store_op_int = (int)color_attachments[0].storeOp;
			EM_ASM({
				var viewPtr = $0;
				var jsView = WebGPU.getJsObject(viewPtr);
				var viewType = jsView ? jsView.constructor.name : 'null';
				var viewLabel = jsView ? jsView.label : 'N/A';
				console.log('[SC-VIEW#' + $3 + '] viewPtr=' + viewPtr +
					' jsType=' + viewType +
					' label=' + viewLabel +
					' loadOp=' + $1 + ' storeOp=' + $2);
				// Check if the view's texture matches the canvas surface texture.
				var canvas = document.querySelector('#canvas');
				if (canvas) {
					var ctx = canvas.getContext('webgpu');
					if (ctx) {
						try {
							var surfTex = ctx.getCurrentTexture();
							var surfView = surfTex.createView();
							console.log('[SC-VIEW#' + $3 + '] surfaceTex=' + surfTex.width + 'x' + surfTex.height +
								' fmt=' + surfTex.format + ' usage=' + surfTex.usage);
						} catch (e) {
							console.log('[SC-VIEW#' + $3 + '] getCurrentTexture error: ' + e.message);
						}
					}
				}
			}, (int)(uintptr_t)view, load_op_int, store_op_int, _sc_view_log);
			_sc_view_log++;
		}
	}

	WGPURenderPassDescriptor pass_desc = {};
	pass_desc.colorAttachmentCount = color_attachments.size();
	pass_desc.colorAttachments = color_attachments.ptr();
	pass_desc.depthStencilAttachment = ds_att_ptr;

	cmd->render_encoder = wgpuCommandEncoderBeginRenderPass(cmd->encoder, &pass_desc);
	cmd->active_encoder = WGCommandBuffer::RENDER;
}

void RenderingDeviceDriverWebGPU::command_end_render_pass(CommandBufferID p_cmd_buffer) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	// Temporary: log render pass ends to track pass boundaries.
	static int _rp_end_log = 0;
	if (_rp_end_log < 30) {
		EM_ASM({ console.log('[RP-END#' + $0 + '] ' + $1 + 'x' + $2); },
				_rp_end_log,
				cmd->render_state.render_area_width,
				cmd->render_state.render_area_height);
		_rp_end_log++;
	}

	// Save swap chain state before ending.
	bool was_swap_chain = cmd->render_state.render_pass && cmd->render_state.render_pass->is_swap_chain_pass;
	WGFramebuffer *sc_fb = was_swap_chain ? cmd->render_state.framebuffer : nullptr;

	if (cmd->render_encoder) {
		wgpuRenderPassEncoderEnd(cmd->render_encoder);
		wgpuRenderPassEncoderRelease(cmd->render_encoder);
		cmd->render_encoder = nullptr;
	}
	cmd->active_encoder = WGCommandBuffer::NONE;
	cmd->render_state = {};
}

void RenderingDeviceDriverWebGPU::command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	// Temporary: log subpass transitions.
	static int _sp_log = 0;
	if (_sp_log < 10) {
		EM_ASM({ console.log('[SUBPASS] transition to subpass ' + $0 + ' size=' + $1 + 'x' + $2); },
				cmd->render_state.current_subpass + 1,
				cmd->render_state.render_area_width,
				cmd->render_state.render_area_height);
		_sp_log++;
	}

	// End current render pass encoder.
	if (cmd->render_encoder) {
		wgpuRenderPassEncoderEnd(cmd->render_encoder);
		wgpuRenderPassEncoderRelease(cmd->render_encoder);
		cmd->render_encoder = nullptr;
	}

	// Advance to next subpass.
	cmd->render_state.current_subpass++;

	WGRenderPass *rp = cmd->render_state.render_pass;
	WGFramebuffer *fb = cmd->render_state.framebuffer;
	ERR_FAIL_NULL(rp);
	ERR_FAIL_NULL(fb);
	ERR_FAIL_COND(cmd->render_state.current_subpass >= rp->subpasses.size());

	const WGRenderPass::SubpassInfo &subpass = rp->subpasses[cmd->render_state.current_subpass];

	// --- Build color attachments for the new subpass ---
	// For subsequent subpasses, we LOAD (not clear) color/depth since we want to preserve
	// what was drawn in previous subpasses on shared attachments.
	LocalVector<WGPURenderPassColorAttachment> color_attachments;
	for (uint32_t i = 0; i < subpass.color_references.size(); i++) {
		const RDD::AttachmentReference &ref = subpass.color_references[i];

		WGPURenderPassColorAttachment att = {};
		att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

		if (ref.attachment == RDD::AttachmentReference::UNUSED) {
			att.view = nullptr;
			att.loadOp = WGPULoadOp_Load;
			att.storeOp = WGPUStoreOp_Discard;
			color_attachments.push_back(att);
			continue;
		}

		if (ref.attachment < fb->attachment_views.size()) {
			att.view = fb->attachment_views[ref.attachment];
		}

		// Resolve target (MSAA).
		if (i < subpass.resolve_references.size()) {
			const RDD::AttachmentReference &res_ref = subpass.resolve_references[i];
			if (res_ref.attachment != RDD::AttachmentReference::UNUSED &&
					res_ref.attachment < fb->attachment_views.size()) {
				att.resolveTarget = fb->attachment_views[res_ref.attachment];
			}
		}

		// Load from previous subpass output, store for next use.
		att.loadOp = WGPULoadOp_Load;
		att.storeOp = WGPUStoreOp_Store;

		color_attachments.push_back(att);
	}

	// --- Build depth/stencil attachment ---
	WGPURenderPassDepthStencilAttachment ds_att = {};
	WGPURenderPassDepthStencilAttachment *ds_att_ptr = nullptr;

	const RDD::AttachmentReference &ds_ref = subpass.depth_stencil_reference;
	if (ds_ref.attachment != RDD::AttachmentReference::UNUSED &&
			ds_ref.attachment < fb->attachment_views.size()) {
		ds_att.view = fb->attachment_views[ds_ref.attachment];

		if (ds_ref.attachment < rp->attachments.size()) {
			const RDD::Attachment &attach_desc = rp->attachments[ds_ref.attachment];
			WGPUTextureFormat wgpu_fmt = _data_format_to_wgpu(attach_desc.format);
			bool has_depth = is_depth_format_wgpu(wgpu_fmt);
			bool has_stencil = has_stencil_wgpu(wgpu_fmt);

			if (has_depth) {
				ds_att.depthLoadOp = WGPULoadOp_Load;
				ds_att.depthStoreOp = WGPUStoreOp_Store;
				ds_att.depthClearValue = 0.0f;
			} else {
				ds_att.depthLoadOp = WGPULoadOp_Undefined;
				ds_att.depthStoreOp = WGPUStoreOp_Undefined;
				ds_att.depthReadOnly = true;
			}

			if (has_stencil) {
				ds_att.stencilLoadOp = WGPULoadOp_Load;
				ds_att.stencilStoreOp = WGPUStoreOp_Store;
			} else {
				ds_att.stencilLoadOp = WGPULoadOp_Undefined;
				ds_att.stencilStoreOp = WGPUStoreOp_Undefined;
				ds_att.stencilReadOnly = true;
			}
		} else {
			ds_att.depthLoadOp = WGPULoadOp_Load;
			ds_att.depthStoreOp = WGPUStoreOp_Store;
			ds_att.depthClearValue = 1.0f;
			ds_att.stencilLoadOp = WGPULoadOp_Load;
			ds_att.stencilStoreOp = WGPUStoreOp_Store;
		}
		ds_att_ptr = &ds_att;
	}

	// --- Begin new render pass for this subpass ---
	WGPURenderPassDescriptor pass_desc = {};
	pass_desc.colorAttachmentCount = color_attachments.size();
	pass_desc.colorAttachments = color_attachments.ptr();
	pass_desc.depthStencilAttachment = ds_att_ptr;

	cmd->render_encoder = wgpuCommandEncoderBeginRenderPass(cmd->encoder, &pass_desc);
	cmd->active_encoder = WGCommandBuffer::RENDER;

	// Reset pipeline state — new render pass requires re-binding everything.
	cmd->render_state.current_pipeline = nullptr;
}

void RenderingDeviceDriverWebGPU::command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (p_viewports.size() > 0) {
		const Rect2i &vp = p_viewports[0]; // WebGPU supports only one viewport.
		wgpuRenderPassEncoderSetViewport(cmd->render_encoder, (float)vp.position.x, (float)vp.position.y, (float)vp.size.x, (float)vp.size.y, 0.0f, 1.0f);
	}
}

void RenderingDeviceDriverWebGPU::command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (p_scissors.size() > 0) {
		const Rect2i &sr = p_scissors[0];
		uint32_t x = MAX(sr.position.x, 0);
		uint32_t y = MAX(sr.position.y, 0);
		uint32_t w = MAX(sr.size.x, 0);
		uint32_t h = MAX(sr.size.y, 0);
		// Clamp scissor to the SMALLER of (render area) and (actual framebuffer size).
		// WebGPU requires scissor to fit within attachment dimensions.
		uint32_t clamp_w = 0;
		uint32_t clamp_h = 0;
		if (cmd->render_state.render_area_width > 0) {
			clamp_w = cmd->render_state.render_area_width;
			clamp_h = cmd->render_state.render_area_height;
		}
		if (cmd->render_state.framebuffer) {
			uint32_t fb_w = cmd->render_state.framebuffer->width;
			uint32_t fb_h = cmd->render_state.framebuffer->height;
			// Use actual attachment texture dimensions when available — fb->width/height
			// may reflect the Godot-level render-area (window size) rather than the GPU
			// texture size, which is what WebGPU validates the scissor rect against.
			const WGFramebuffer *wgfb = cmd->render_state.framebuffer;
			if (!wgfb->attachments.is_empty() && wgfb->attachments[0] != nullptr) {
				fb_w = wgfb->attachments[0]->width;
				fb_h = wgfb->attachments[0]->height;
			}
			clamp_w = clamp_w > 0 ? MIN(clamp_w, fb_w) : fb_w;
			clamp_h = clamp_h > 0 ? MIN(clamp_h, fb_h) : fb_h;
		}
		if (clamp_w > 0 && clamp_h > 0) {
			if (x >= clamp_w || y >= clamp_h) {
				w = 0;
				h = 0;
				x = 0;
				y = 0;
			} else {
				w = MIN(w, clamp_w - x);
				h = MIN(h, clamp_h - y);
			}
		}
		wgpuRenderPassEncoderSetScissorRect(cmd->render_encoder, x, y, w, h);
	}
}

void RenderingDeviceDriverWebGPU::command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) {
	// WebGPU has no mid-pass clear. Options:
	// (a) End pass, do a clear pass, restart — simplest.
	// (b) Draw full-screen quad with clear color.
	// TODO: Implement option (a).
	WARN_PRINT_ONCE("WebGPU: command_render_clear_attachments not yet implemented.");
}

void RenderingDeviceDriverWebGPU::command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGPipelineWrapper *pw = (WGPipelineWrapper *)(p_pipeline.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(pw);
	ERR_FAIL_COND(!cmd->render_encoder);

	wgpuRenderPassEncoderSetPipeline(cmd->render_encoder, pw->render_handle);
	cmd->render_state.current_pipeline = pw;
}

void RenderingDeviceDriverWebGPU::command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	WGShader *pipeline_shader = cmd->render_state.current_pipeline ? cmd->render_state.current_pipeline->shader : nullptr;

	// Diagnostic: log texture bindings and push constant info on swap chain pass.
	// Invalidate bind group tracking if the pipeline shader changed.
	if (pipeline_shader != cmd->bound_shader) {
		cmd->invalidate_bind_groups();
		cmd->bound_shader = pipeline_shader;
	}

	for (uint32_t i = 0; i < p_set_count; i++) {
		WGUniformSet *us = (WGUniformSet *)(p_uniform_sets[i].id);
		if (us && us->handle) {
			uint32_t set_idx = p_first_set_index + i;

			// Get a bind group compatible with the current pipeline's BGL.
			WGPUBindGroup bg_to_bind = _get_compatible_bind_group(us, pipeline_shader, set_idx);

			// If this is the PC group of the current shader (with a merged layout),
			// track the bind group for use in _flush_push_constants and bind it
			// immediately with offset 0 (will be rebound with the real offset on flush).
			if (pipeline_shader && pipeline_shader->push_constant_size > 0 &&
					set_idx == pipeline_shader->push_constant_bind_group) {
				if (pipeline_shader->merged_pc_group_layout) {
					cmd->current_pc_bind_group = bg_to_bind;
					uint32_t zero_offset = 0;
					wgpuRenderPassEncoderSetBindGroup(cmd->render_encoder, set_idx, bg_to_bind, 1, &zero_offset);
				} else {
					cmd->current_pc_bind_group = nullptr;
					wgpuRenderPassEncoderSetBindGroup(cmd->render_encoder, set_idx, bg_to_bind, 0, nullptr);
				}
				perf.set_bind_group_calls++;
				// PC group always needs rebind (dynamic offset changes per draw).
			} else {
				// Skip redundant SetBindGroup calls for non-PC groups.
				if (set_idx < WGCommandBuffer::MAX_BIND_GROUPS && cmd->bound_bind_groups[set_idx] == bg_to_bind) {
					perf.set_bind_group_skipped++;
					continue; // Already bound — skip.
				}
				wgpuRenderPassEncoderSetBindGroup(cmd->render_encoder, set_idx, bg_to_bind, 0, nullptr);
				perf.set_bind_group_calls++;
				if (set_idx < WGCommandBuffer::MAX_BIND_GROUPS) {
					cmd->bound_bind_groups[set_idx] = bg_to_bind;
				}
			}
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

	perf.draw_calls++;
	wgpuRenderPassEncoderDraw(cmd->render_encoder, p_vertex_count, p_instance_count, p_base_vertex, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

	perf.draw_calls++;
	wgpuRenderPassEncoderDrawIndexed(cmd->render_encoder, p_index_count, p_instance_count, p_first_index, p_vertex_offset, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *indirect = (WGBuffer *)(p_indirect_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(indirect);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

	// WebGPU has no multi-draw-indirect — must loop.
	for (uint32_t i = 0; i < p_draw_count; i++) {
		wgpuRenderPassEncoderDrawIndexedIndirect(cmd->render_encoder, indirect->handle, p_offset + i * p_stride);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	// TODO: Read count from buffer (requires async readback). For now, use max count.
	command_render_draw_indexed_indirect(p_cmd_buffer, p_indirect_buffer, p_offset, p_max_draw_count, p_stride);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *indirect = (WGBuffer *)(p_indirect_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(indirect);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

	for (uint32_t i = 0; i < p_draw_count; i++) {
		wgpuRenderPassEncoderDrawIndirect(cmd->render_encoder, indirect->handle, p_offset + i * p_stride);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) {
	command_render_draw_indirect(p_cmd_buffer, p_indirect_buffer, p_offset, p_max_draw_count, p_stride);
}

void RenderingDeviceDriverWebGPU::command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	for (uint32_t i = 0; i < p_binding_count; i++) {
		WGBuffer *buf = (WGBuffer *)(p_buffers[i].id);
		if (buf) {
			wgpuRenderPassEncoderSetVertexBuffer(cmd->render_encoder, i, buf->handle, p_offsets[i], WGPU_WHOLE_SIZE);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(buf);
	ERR_FAIL_COND(!cmd->render_encoder);

	WGPUIndexFormat format = (p_format == INDEX_BUFFER_FORMAT_UINT16) ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
	wgpuRenderPassEncoderSetIndexBuffer(cmd->render_encoder, buf->handle, format, p_offset, WGPU_WHOLE_SIZE);
}

void RenderingDeviceDriverWebGPU::command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	WGPUColor color = { p_constants.r, p_constants.g, p_constants.b, p_constants.a };
	wgpuRenderPassEncoderSetBlendConstant(cmd->render_encoder, &color);
}

void RenderingDeviceDriverWebGPU::command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) {
	// No-op: WebGPU doesn't support line width > 1.0.
}

// =============================================================================
// Specialization Constant Support
// =============================================================================

// SPIR-V opcodes for specialization constant handling.
static constexpr uint16_t SPV_OP_DECORATE = 71;
static constexpr uint16_t SPV_OP_SPEC_CONSTANT_TRUE = 48;
static constexpr uint16_t SPV_OP_SPEC_CONSTANT_FALSE = 49;
static constexpr uint16_t SPV_OP_SPEC_CONSTANT = 50;
static constexpr uint16_t SPV_DECORATION_SPEC_ID = 1;

static inline uint32_t _spv_read_word(const uint8_t *p_data, uint32_t p_word_index) {
	const uint8_t *p = p_data + p_word_index * 4;
	return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static inline void _spv_write_word(uint8_t *p_data, uint32_t p_word_index, uint32_t p_value) {
	uint8_t *p = p_data + p_word_index * 4;
	p[0] = p_value & 0xFF;
	p[1] = (p_value >> 8) & 0xFF;
	p[2] = (p_value >> 16) & 0xFF;
	p[3] = (p_value >> 24) & 0xFF;
}

// Patches SPIR-V bytecode to apply specialization constant values.
// Returns a modified copy with OpSpecConstant* values updated.
static PackedByteArray _patch_spirv_spec_constants(const PackedByteArray &p_spirv, VectorView<RDD::PipelineSpecializationConstant> p_constants) {
	if (p_constants.size() == 0 || p_spirv.size() < 20) {
		return p_spirv;
	}

	// Build a map: SpecId → value (as uint32_t).
	HashMap<uint32_t, uint32_t> spec_values;
	for (uint32_t i = 0; i < p_constants.size(); i++) {
		const RDD::PipelineSpecializationConstant &c = p_constants[i];
		uint32_t val = 0;
		switch (c.type) {
			case RDD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL:
				val = c.bool_value ? 1 : 0;
				break;
			case RDD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT:
				val = (uint32_t)c.int_value;
				break;
			case RDD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT:
				memcpy(&val, &c.float_value, sizeof(float));
				break;
		}
		spec_values[c.constant_id] = val;
	}

	// First pass: scan OpDecorate for SpecId → result_id mapping.
	HashMap<uint32_t, uint32_t> spec_id_to_result_id; // SpecId → result_id
	uint32_t total_words = p_spirv.size() / 4;
	uint32_t pos = 5; // Skip SPIR-V header (5 words).
	while (pos < total_words) {
		uint32_t w0 = _spv_read_word(p_spirv.ptr(), pos);
		uint32_t wc = w0 >> 16;
		uint32_t op = w0 & 0xFFFF;
		if (wc == 0 || pos + wc > total_words) {
			break;
		}
		// OpDecorate target decoration [literal...]
		// For SpecId: OpDecorate result_id SpecId(1) literal_spec_id
		if (op == SPV_OP_DECORATE && wc >= 4) {
			uint32_t target = _spv_read_word(p_spirv.ptr(), pos + 1);
			uint32_t decoration = _spv_read_word(p_spirv.ptr(), pos + 2);
			if (decoration == SPV_DECORATION_SPEC_ID) {
				uint32_t spec_id = _spv_read_word(p_spirv.ptr(), pos + 3);
				spec_id_to_result_id[spec_id] = target;
			}
		}
		pos += wc;
	}

	// Build result_id → value map.
	HashMap<uint32_t, uint32_t> result_to_value;
	for (const KeyValue<uint32_t, uint32_t> &kv : spec_id_to_result_id) {
		if (spec_values.has(kv.key)) {
			result_to_value[kv.value] = spec_values[kv.key];
		}
	}

	if (result_to_value.is_empty()) {
		return p_spirv; // No matching spec constants to patch.
	}

	// Second pass: patch the SPIR-V.
	PackedByteArray out = p_spirv;
	pos = 5;
	while (pos < total_words) {
		uint32_t w0 = _spv_read_word(out.ptr(), pos);
		uint32_t wc = w0 >> 16;
		uint32_t op = w0 & 0xFFFF;
		if (wc == 0 || pos + wc > total_words) {
			break;
		}

		if (op == SPV_OP_SPEC_CONSTANT_TRUE || op == SPV_OP_SPEC_CONSTANT_FALSE) {
			// OpSpecConstantTrue/False: wc=3, [type_id, result_id]
			if (wc >= 3) {
				uint32_t result_id = _spv_read_word(out.ptr(), pos + 2);
				if (result_to_value.has(result_id)) {
					uint32_t val = result_to_value[result_id];
					uint16_t new_op = val ? SPV_OP_SPEC_CONSTANT_TRUE : SPV_OP_SPEC_CONSTANT_FALSE;
					_spv_write_word(out.ptrw(), pos, (wc << 16) | new_op);
				}
			}
		} else if (op == SPV_OP_SPEC_CONSTANT) {
			// OpSpecConstant: wc>=4, [type_id, result_id, value...]
			if (wc >= 4) {
				uint32_t result_id = _spv_read_word(out.ptr(), pos + 2);
				if (result_to_value.has(result_id)) {
					_spv_write_word(out.ptrw(), pos + 3, result_to_value[result_id]);
				}
			}
		}

		pos += wc;
	}

	return out;
}

// Creates a WGPUShaderModule from SPIR-V with specialization constants applied.
// Returns nullptr on failure.
WGPUShaderModule RenderingDeviceDriverWebGPU::_create_module_with_spec_constants(
		const PackedByteArray &p_spirv,
		VectorView<PipelineSpecializationConstant> p_constants,
		ShaderStage p_stage) {
	PackedByteArray patched = _patch_spirv_spec_constants(p_spirv, p_constants);

	char *wgsl_str = (char *)(uintptr_t)MAIN_THREAD_EM_ASM_PTR(
			{
				try {
					if (typeof window.nagaSpirvToWgsl !== 'function') {
						console.error('naga SPIR-V→WGSL converter not loaded!');
						return 0;
					}
					var spirvBytes = new Uint8Array(HEAPU8.buffer, $0, $1);
					var wgsl = window.nagaSpirvToWgsl(spirvBytes);
					if (!wgsl) {
						return 0;
					}
					var len = lengthBytesUTF8(wgsl) + 1;
					var ptr = _malloc(len);
					stringToUTF8(wgsl, ptr, len);
					return ptr;
				} catch (e) {
					console.error('[SHADER-SPEC] NAGA conversion exception:', e.message || e);
					return 0;
				}
			},
			patched.ptr(), (int)patched.size());

	if (!wgsl_str) {
		ERR_PRINT("WebGPU: SPIR-V→WGSL conversion failed for specialized shader module.");
		return nullptr;
	}

	// Demote read_write storage to read for vertex/fragment stages.
	if (p_stage == SHADER_STAGE_VERTEX || p_stage == SHADER_STAGE_FRAGMENT) {
		char *q = wgsl_str;
		while ((q = strstr(q, "var<storage, read_write>")) != nullptr) {
			memcpy(q, "var<storage, read>      ", 24);
			q += 24;
		}
	}

	WGPUShaderSourceWGSL wgsl_source = {};
	wgsl_source.chain.sType = WGPUSType_ShaderSourceWGSL;
	wgsl_source.code = { wgsl_str, WGPU_STRLEN };

	WGPUShaderModuleDescriptor desc = {};
	desc.nextInChain = &wgsl_source.chain;

	WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &desc);
	free(wgsl_str);

	return mod;
}

RDD::PipelineID RenderingDeviceDriverWebGPU::render_pipeline_create(
		ShaderID p_shader,
		VertexFormatID p_vertex_format,
		RenderPrimitive p_render_primitive,
		PipelineRasterizationState p_rasterization_state,
		PipelineMultisampleState p_multisample_state,
		PipelineDepthStencilState p_depth_stencil_state,
		PipelineColorBlendState p_blend_state,
		VectorView<int32_t> p_color_attachments,
		BitField<PipelineDynamicStateFlags> p_dynamic_state,
		RenderPassID p_render_pass,
		uint32_t p_render_subpass,
		VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	WGShader *shader = (WGShader *)(p_shader.id);
	ERR_FAIL_COND_V(!shader, PipelineID());
	WGVertexFormat *vf = p_vertex_format.id ? (WGVertexFormat *)(p_vertex_format.id) : nullptr;
	WGRenderPass *rp = (WGRenderPass *)(p_render_pass.id);
	ERR_FAIL_COND_V(!rp, PipelineID());
	ERR_FAIL_COND_V(p_render_subpass >= (uint32_t)rp->subpasses.size(), PipelineID());
	const WGRenderPass::SubpassInfo &subpass = rp->subpasses[p_render_subpass];

	// --- Specialization constants ---
	// The default shader modules have spec constants frozen with default values.
	// If pipeline-specific values are provided, create specialized modules.
	WGPUShaderModule vertex_module = shader->stage_modules[SHADER_STAGE_VERTEX];
	WGPUShaderModule fragment_module = shader->stage_modules[SHADER_STAGE_FRAGMENT];
	WGPUShaderModule specialized_vertex = nullptr;
	WGPUShaderModule specialized_fragment = nullptr;
	if (p_specialization_constants.size() > 0) {
		if (!shader->stage_spirv[SHADER_STAGE_VERTEX].is_empty()) {
			specialized_vertex = _create_module_with_spec_constants(
					shader->stage_spirv[SHADER_STAGE_VERTEX], p_specialization_constants, SHADER_STAGE_VERTEX);
			if (specialized_vertex) {
				vertex_module = specialized_vertex;
			}
		}
		if (!shader->stage_spirv[SHADER_STAGE_FRAGMENT].is_empty()) {
			specialized_fragment = _create_module_with_spec_constants(
					shader->stage_spirv[SHADER_STAGE_FRAGMENT], p_specialization_constants, SHADER_STAGE_FRAGMENT);
			if (specialized_fragment) {
				fragment_module = specialized_fragment;
			}
		}
	}

	// --- Vertex state ---
	LocalVector<WGPUVertexBufferLayout> vb_layouts;
	LocalVector<WGPUVertexAttribute> vb_attrs;
	if (vf && !vf->attributes.is_empty()) {
		// Group attributes by binding, counting totals first.
		HashMap<uint32_t, uint32_t> binding_attr_count;
		for (const WGVertexFormat::Attribute &a : vf->attributes) {
			binding_attr_count[a.binding]++;
		}
		uint32_t total_attrs = vf->attributes.size();
		vb_attrs.resize(total_attrs); // Pre-allocate — no reallocation after this.

		// Determine start index per binding.
		HashMap<uint32_t, uint32_t> binding_start;
		uint32_t pos = 0;
		for (uint32_t b = 0; b < vf->bindings.size(); b++) {
			if (binding_attr_count.has(b)) {
				binding_start[b] = pos;
				pos += binding_attr_count[b];
			}
		}

		// Fill attrs per binding.
		HashMap<uint32_t, uint32_t> fill_pos;
		for (const WGVertexFormat::Attribute &a : vf->attributes) {
			uint32_t idx = binding_start[a.binding] + (fill_pos.has(a.binding) ? fill_pos[a.binding]++ : (fill_pos[a.binding] = 1, 0));
			vb_attrs[idx].format = a.format;
			vb_attrs[idx].offset = a.offset;
			vb_attrs[idx].shaderLocation = a.location;
		}

		// Build layouts (one per binding that has attributes).
		for (uint32_t b = 0; b < vf->bindings.size(); b++) {
			if (!binding_attr_count.has(b)) {
				continue;
			}
			WGPUVertexBufferLayout layout = {};
			layout.arrayStride = vf->bindings[b].stride;
			layout.stepMode = vf->bindings[b].step_mode;
			layout.attributeCount = binding_attr_count[b];
			layout.attributes = vb_attrs.ptr() + binding_start[b];
			vb_layouts.push_back(layout);
		}
	}

	WGPUVertexState vertex_state = {};
	vertex_state.module = vertex_module;
	vertex_state.entryPoint = { "main", WGPU_STRLEN };
	vertex_state.bufferCount = vb_layouts.size();
	vertex_state.buffers = vb_layouts.ptr();

	// --- Primitive state ---
	WGPUPrimitiveState primitive = {};
	bool is_strip = false;
	switch (p_render_primitive) {
		case RENDER_PRIMITIVE_POINTS:
			primitive.topology = WGPUPrimitiveTopology_PointList;
			break;
		case RENDER_PRIMITIVE_LINES:
		case RENDER_PRIMITIVE_LINES_WITH_ADJACENCY:
			primitive.topology = WGPUPrimitiveTopology_LineList;
			break;
		case RENDER_PRIMITIVE_LINESTRIPS:
		case RENDER_PRIMITIVE_LINESTRIPS_WITH_ADJACENCY:
			primitive.topology = WGPUPrimitiveTopology_LineStrip;
			is_strip = true;
			break;
		case RENDER_PRIMITIVE_TRIANGLES:
		case RENDER_PRIMITIVE_TRIANGLES_WITH_ADJACENCY:
			primitive.topology = WGPUPrimitiveTopology_TriangleList;
			break;
		case RENDER_PRIMITIVE_TRIANGLE_STRIPS:
		case RENDER_PRIMITIVE_TRIANGLE_STRIPS_WITH_AJACENCY:
		case RENDER_PRIMITIVE_TRIANGLE_STRIPS_WITH_RESTART_INDEX:
			primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
			is_strip = true;
			break;
		default:
			primitive.topology = WGPUPrimitiveTopology_TriangleList;
			break;
	}
	if (is_strip) {
		primitive.stripIndexFormat = WGPUIndexFormat_Uint32;
	}
	switch (p_rasterization_state.cull_mode) {
		case POLYGON_CULL_FRONT:
			primitive.cullMode = WGPUCullMode_Front;
			break;
		case POLYGON_CULL_BACK:
			primitive.cullMode = WGPUCullMode_Back;
			break;
		default:
			primitive.cullMode = WGPUCullMode_None;
			break;
	}
	primitive.frontFace = (p_rasterization_state.front_face == POLYGON_FRONT_FACE_CLOCKWISE)
			? WGPUFrontFace_CW
			: WGPUFrontFace_CCW;

	// --- Multisample state ---
	WGPUMultisampleState multisample = {};
	// TextureSamples enum: 0=1x, 1=2x, 2=4x, 3=8x ... → 1 << sample_count
	multisample.count = (uint32_t)(1u << (uint32_t)p_multisample_state.sample_count);
	multisample.mask = 0xFFFFFFFFu;
	multisample.alphaToCoverageEnabled = p_multisample_state.enable_alpha_to_coverage;

	// --- Depth/stencil helpers ---
	auto map_compare = [](CompareOperator op) -> WGPUCompareFunction {
		switch (op) {
			case COMPARE_OP_NEVER: return WGPUCompareFunction_Never;
			case COMPARE_OP_LESS: return WGPUCompareFunction_Less;
			case COMPARE_OP_EQUAL: return WGPUCompareFunction_Equal;
			case COMPARE_OP_LESS_OR_EQUAL: return WGPUCompareFunction_LessEqual;
			case COMPARE_OP_GREATER: return WGPUCompareFunction_Greater;
			case COMPARE_OP_NOT_EQUAL: return WGPUCompareFunction_NotEqual;
			case COMPARE_OP_GREATER_OR_EQUAL: return WGPUCompareFunction_GreaterEqual;
			default: return WGPUCompareFunction_Always;
		}
	};
	auto map_stencil_op = [](StencilOperation op) -> WGPUStencilOperation {
		switch (op) {
			case STENCIL_OP_KEEP: return WGPUStencilOperation_Keep;
			case STENCIL_OP_ZERO: return WGPUStencilOperation_Zero;
			case STENCIL_OP_REPLACE: return WGPUStencilOperation_Replace;
			case STENCIL_OP_INCREMENT_AND_CLAMP: return WGPUStencilOperation_IncrementClamp;
			case STENCIL_OP_DECREMENT_AND_CLAMP: return WGPUStencilOperation_DecrementClamp;
			case STENCIL_OP_INVERT: return WGPUStencilOperation_Invert;
			case STENCIL_OP_INCREMENT_AND_WRAP: return WGPUStencilOperation_IncrementWrap;
			case STENCIL_OP_DECREMENT_AND_WRAP: return WGPUStencilOperation_DecrementWrap;
			default: return WGPUStencilOperation_Keep;
		}
	};

	// --- Depth/stencil state ---
	WGPUDepthStencilState ds = {};
	WGPUDepthStencilState *ds_ptr = nullptr;
	if (subpass.depth_stencil_reference.attachment != ATTACHMENT_UNUSED) {
		uint32_t ds_att = (uint32_t)subpass.depth_stencil_reference.attachment;
		ds.format = (ds_att < (uint32_t)rp->attachments.size())
				? _data_format_to_wgpu(rp->attachments[ds_att].format)
				: WGPUTextureFormat_Depth24Plus;

		ds.depthWriteEnabled = p_depth_stencil_state.enable_depth_write
				? WGPUOptionalBool_True
				: WGPUOptionalBool_False;
		ds.depthCompare = p_depth_stencil_state.enable_depth_test
				? map_compare(p_depth_stencil_state.depth_compare_operator)
				: WGPUCompareFunction_Always;

		if (p_depth_stencil_state.enable_stencil) {
			ds.stencilFront.compare = map_compare(p_depth_stencil_state.front_op.compare);
			ds.stencilFront.failOp = map_stencil_op(p_depth_stencil_state.front_op.fail);
			ds.stencilFront.depthFailOp = map_stencil_op(p_depth_stencil_state.front_op.depth_fail);
			ds.stencilFront.passOp = map_stencil_op(p_depth_stencil_state.front_op.pass);
			ds.stencilBack.compare = map_compare(p_depth_stencil_state.back_op.compare);
			ds.stencilBack.failOp = map_stencil_op(p_depth_stencil_state.back_op.fail);
			ds.stencilBack.depthFailOp = map_stencil_op(p_depth_stencil_state.back_op.depth_fail);
			ds.stencilBack.passOp = map_stencil_op(p_depth_stencil_state.back_op.pass);
			ds.stencilReadMask = p_depth_stencil_state.front_op.compare_mask;
			ds.stencilWriteMask = p_depth_stencil_state.front_op.write_mask;
		} else {
			ds.stencilFront = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep };
			ds.stencilBack = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep };
			ds.stencilReadMask = 0xFF;
			ds.stencilWriteMask = 0xFF;
		}
		if (p_rasterization_state.depth_bias_enabled) {
			ds.depthBias = (int32_t)p_rasterization_state.depth_bias_constant_factor;
			ds.depthBiasSlopeScale = p_rasterization_state.depth_bias_slope_factor;
			ds.depthBiasClamp = p_rasterization_state.depth_bias_clamp;
		}
		ds_ptr = &ds;
	}

	// --- Blend factor/op helpers ---
	auto blend_factor = [](BlendFactor f) -> WGPUBlendFactor {
		switch (f) {
			case BLEND_FACTOR_ZERO: return WGPUBlendFactor_Zero;
			case BLEND_FACTOR_ONE: return WGPUBlendFactor_One;
			case BLEND_FACTOR_SRC_COLOR: return WGPUBlendFactor_Src;
			case BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return WGPUBlendFactor_OneMinusSrc;
			case BLEND_FACTOR_DST_COLOR: return WGPUBlendFactor_Dst;
			case BLEND_FACTOR_ONE_MINUS_DST_COLOR: return WGPUBlendFactor_OneMinusDst;
			case BLEND_FACTOR_SRC_ALPHA: return WGPUBlendFactor_SrcAlpha;
			case BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return WGPUBlendFactor_OneMinusSrcAlpha;
			case BLEND_FACTOR_DST_ALPHA: return WGPUBlendFactor_DstAlpha;
			case BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return WGPUBlendFactor_OneMinusDstAlpha;
			case BLEND_FACTOR_CONSTANT_COLOR: return WGPUBlendFactor_Constant;
			case BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return WGPUBlendFactor_OneMinusConstant;
			case BLEND_FACTOR_CONSTANT_ALPHA: return WGPUBlendFactor_Constant;
			case BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return WGPUBlendFactor_OneMinusConstant;
			case BLEND_FACTOR_SRC_ALPHA_SATURATE: return WGPUBlendFactor_SrcAlphaSaturated;
			default: return WGPUBlendFactor_Zero;
		}
	};
	auto blend_op = [](BlendOperation op) -> WGPUBlendOperation {
		switch (op) {
			case BLEND_OP_ADD: return WGPUBlendOperation_Add;
			case BLEND_OP_SUBTRACT: return WGPUBlendOperation_Subtract;
			case BLEND_OP_REVERSE_SUBTRACT: return WGPUBlendOperation_ReverseSubtract;
			case BLEND_OP_MINIMUM: return WGPUBlendOperation_Min;
			case BLEND_OP_MAXIMUM: return WGPUBlendOperation_Max;
			default: return WGPUBlendOperation_Add;
		}
	};

	// --- Color targets (one per fragment shader @location()) ---
	uint32_t color_target_count = p_color_attachments.size();
	LocalVector<WGPUColorTargetState> color_targets;
	LocalVector<WGPUBlendState> blend_states;
	color_targets.resize(color_target_count);
	blend_states.resize(color_target_count);
	memset(color_targets.ptr(), 0, sizeof(WGPUColorTargetState) * color_target_count);
	memset(blend_states.ptr(), 0, sizeof(WGPUBlendState) * color_target_count);

	for (uint32_t i = 0; i < color_target_count; i++) {
		// Get texture format from subpass color reference i.
		WGPUTextureFormat fmt = WGPUTextureFormat_RGBA8Unorm; // Placeholder for unused slots.
		if (i < (uint32_t)subpass.color_references.size()) {
			int32_t att_idx = subpass.color_references[i].attachment;
			if (att_idx != ATTACHMENT_UNUSED && (uint32_t)att_idx < (uint32_t)rp->attachments.size()) {
				fmt = _data_format_to_wgpu(rp->attachments[att_idx].format);
				if (fmt == WGPUTextureFormat_Undefined) {
					fmt = WGPUTextureFormat_RGBA8Unorm;
				}
			}
		}
		color_targets[i].format = fmt;

		if (p_color_attachments[i] == ATTACHMENT_UNUSED) {
			color_targets[i].writeMask = WGPUColorWriteMask_None;
			continue;
		}

		// Blend state from p_blend_state.attachments[i].
		if (i < (uint32_t)p_blend_state.attachments.size()) {
			const PipelineColorBlendState::Attachment &ba = p_blend_state.attachments[i];
			WGPUColorWriteMask mask = WGPUColorWriteMask_None;
			if (ba.write_r) { mask |= WGPUColorWriteMask_Red; }
			if (ba.write_g) { mask |= WGPUColorWriteMask_Green; }
			if (ba.write_b) { mask |= WGPUColorWriteMask_Blue; }
			if (ba.write_a) { mask |= WGPUColorWriteMask_Alpha; }
			// Strip alpha writes for ALL pipelines targeting the swap chain format.
			// Chrome ignores CompositeAlphaMode_Opaque and composites alpha=0
			// against a gray/white background. The swap chain (BGRA8Unorm) is the
			// only BGRA render target — internal targets use RGBA formats.
			// Stripping alpha for blended pipelines too ensures the clear value's
			// alpha=1 is never overwritten by shader output.
			if (fmt == WGPUTextureFormat_BGRA8Unorm) {
				mask &= ~WGPUColorWriteMask_Alpha;
				static int _alpha_strip_log = 0;
				if (_alpha_strip_log < 10) {
					const char *sname = (p_shader.id) ? ((WGShader *)(p_shader.id))->name.utf8().get_data() : "?";
					EM_ASM({ console.log('[ALPHA-STRIP] Pipeline #' + $0 + ' fmt=BGRA8Unorm mask=' + $1 + ' blend=' + $2 + ' shader=' + UTF8ToString($3)); },
							_alpha_strip_log, (int)mask, ba.enable_blend ? 1 : 0, sname);
					_alpha_strip_log++;
				}
			}
			color_targets[i].writeMask = mask;
			if (ba.enable_blend) {
				blend_states[i].color = { blend_op(ba.color_blend_op), blend_factor(ba.src_color_blend_factor), blend_factor(ba.dst_color_blend_factor) };
				blend_states[i].alpha = { blend_op(ba.alpha_blend_op), blend_factor(ba.src_alpha_blend_factor), blend_factor(ba.dst_alpha_blend_factor) };
				color_targets[i].blend = &blend_states[i];
			}
		} else {
			color_targets[i].writeMask = WGPUColorWriteMask_All;
		}
	}

	// --- Fragment state ---
	WGPUFragmentState frag = {};
	frag.module = fragment_module;
	frag.entryPoint = { "main", WGPU_STRLEN };
	frag.targetCount = color_target_count;
	frag.targets = color_targets.ptr();

	// --- Build and create render pipeline ---
	WGPURenderPipelineDescriptor desc = {};
	desc.layout = shader->pipeline_layout;
	desc.vertex = vertex_state;
	desc.primitive = primitive;
	desc.depthStencil = ds_ptr;
	desc.multisample = multisample;
	if (frag.module) {
		desc.fragment = &frag;
	}

	// Push a validation error scope so we can capture the exact creation error message.
	static int _pcreate_id = 0;
	int _pid = _pcreate_id++;
	const char *_shader_name = shader->name.utf8().get_data();
	EM_ASM({ Module['preinitializedWebGPUDevice'].pushErrorScope('validation'); });
	WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
	EM_ASM({
		var pid = $0;
		var sname = UTF8ToString($1);
		Module['preinitializedWebGPUDevice'].popErrorScope().then(function(err) {
			if (err) {
				console.error('[PCREATE-FAIL] id=' + pid + ' shader=' + sname + ' | ' + err.message.substring(0, 1600));
			}
		});
	}, _pid, _shader_name);
	ERR_FAIL_COND_V_MSG(!pipeline, PipelineID(), "WebGPU: Failed to create render pipeline.");

	WGPipelineWrapper *pw = new WGPipelineWrapper();
	pw->type = WGPipelineWrapper::RENDER;
	pw->render_handle = pipeline;
	pw->shader = shader;
	pw->specialized_modules[SHADER_STAGE_VERTEX] = specialized_vertex;
	pw->specialized_modules[SHADER_STAGE_FRAGMENT] = specialized_fragment;
	return PipelineID(pw);
}

// =============================================================================
// COMPUTE
// =============================================================================

void RenderingDeviceDriverWebGPU::command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGPipelineWrapper *pw = (WGPipelineWrapper *)(p_pipeline.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(pw);

	// Ensure we're in a compute pass (end any active render pass).
	if (cmd->active_encoder == WGCommandBuffer::RENDER) {
		cmd->end_active_encoder();
	}
	if (cmd->active_encoder != WGCommandBuffer::COMPUTE) {
		WGPUComputePassDescriptor pass_desc = {};
		cmd->compute_encoder = wgpuCommandEncoderBeginComputePass(cmd->encoder, &pass_desc);
		cmd->active_encoder = WGCommandBuffer::COMPUTE;
	}

	wgpuComputePassEncoderSetPipeline(cmd->compute_encoder, pw->compute_handle);
	cmd->render_state.current_pipeline = pw;
}

void RenderingDeviceDriverWebGPU::command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->compute_encoder);

	WGShader *pipeline_shader = cmd->render_state.current_pipeline ? cmd->render_state.current_pipeline->shader : nullptr;

	for (uint32_t i = 0; i < p_set_count; i++) {
		WGUniformSet *us = (WGUniformSet *)(p_uniform_sets[i].id);
		if (us && us->handle) {
			uint32_t set_idx = p_first_set_index + i;
			WGPUBindGroup bg_to_bind = _get_compatible_bind_group(us, pipeline_shader, set_idx);

			// Mirror the render path: if this is the PC group with a merged layout,
			// the BGL has 1 dynamic buffer — must pass 1 dynamic offset (0 initially;
			// _flush_push_constants will rebind with the real offset before dispatch).
			if (pipeline_shader && pipeline_shader->push_constant_size > 0 &&
					set_idx == pipeline_shader->push_constant_bind_group) {
				if (pipeline_shader->merged_pc_group_layout) {
					cmd->current_pc_bind_group = bg_to_bind;
					uint32_t zero_offset = 0;
					wgpuComputePassEncoderSetBindGroup(cmd->compute_encoder, set_idx, bg_to_bind, 1, &zero_offset);
				} else {
					cmd->current_pc_bind_group = nullptr;
					wgpuComputePassEncoderSetBindGroup(cmd->compute_encoder, set_idx, bg_to_bind, 0, nullptr);
				}
			} else {
				wgpuComputePassEncoderSetBindGroup(cmd->compute_encoder, set_idx, bg_to_bind, 0, nullptr);
			}
		}
	}
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->compute_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

	wgpuComputePassEncoderDispatchWorkgroups(cmd->compute_encoder, p_x_groups, p_y_groups, p_z_groups);
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	WGBuffer *indirect = (WGBuffer *)(p_indirect_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_NULL(indirect);
	ERR_FAIL_COND(!cmd->compute_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

	wgpuComputePassEncoderDispatchWorkgroupsIndirect(cmd->compute_encoder, indirect->handle, p_offset);
}

RDD::PipelineID RenderingDeviceDriverWebGPU::compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) {
	WGShader *shader = (WGShader *)(p_shader.id);
	ERR_FAIL_COND_V(!shader, PipelineID());
	ERR_FAIL_COND_V_MSG(!shader->stage_modules[SHADER_STAGE_COMPUTE], PipelineID(),
			"WebGPU: compute_pipeline_create called with a shader that has no compute stage.");

	// Specialization constants: create a specialized module if values are provided.
	WGPUShaderModule compute_module = shader->stage_modules[SHADER_STAGE_COMPUTE];
	WGPUShaderModule specialized_compute = nullptr;
	if (p_specialization_constants.size() > 0 && !shader->stage_spirv[SHADER_STAGE_COMPUTE].is_empty()) {
		specialized_compute = _create_module_with_spec_constants(
				shader->stage_spirv[SHADER_STAGE_COMPUTE], p_specialization_constants, SHADER_STAGE_COMPUTE);
		if (specialized_compute) {
			compute_module = specialized_compute;
		}
	}

	WGPUComputePipelineDescriptor desc = {};
	desc.layout = shader->pipeline_layout;
	desc.compute.module = compute_module;
	desc.compute.entryPoint = { "main", WGPU_STRLEN };

	WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device, &desc);
	ERR_FAIL_COND_V_MSG(!pipeline, PipelineID(), "WebGPU: Failed to create compute pipeline.");

	WGPipelineWrapper *pw = new WGPipelineWrapper();
	pw->type = WGPipelineWrapper::COMPUTE;
	pw->compute_handle = pipeline;
	pw->shader = shader;
	pw->specialized_modules[SHADER_STAGE_COMPUTE] = specialized_compute;
	return PipelineID(pw);
}

// =============================================================================
// QUERIES
// =============================================================================

RDD::QueryPoolID RenderingDeviceDriverWebGPU::timestamp_query_pool_create(uint32_t p_query_count) {
	WGQueryPool *pool = new WGQueryPool();
	pool->count = p_query_count;
	pool->cpu_results = (uint64_t *)memalloc(sizeof(uint64_t) * p_query_count);
	memset(pool->cpu_results, 0, sizeof(uint64_t) * p_query_count);

	if (!timestamp_supported) {
		// No hardware timestamp support — return a dummy pool that returns zeros.
		pool->is_real = false;
		return QueryPoolID(pool);
	}

	// Create query set.
	WGPUQuerySetDescriptor qs_desc = {};
	qs_desc.type = WGPUQueryType_Timestamp;
	qs_desc.count = p_query_count;
	pool->handle = wgpuDeviceCreateQuerySet(device, &qs_desc);
	if (!pool->handle) {
		WARN_PRINT("WebGPU: Failed to create timestamp query set, falling back to dummy timestamps.");
		pool->is_real = false;
		return QueryPoolID(pool);
	}

	// Create resolve buffer (GPU-side, receives resolved query results).
	uint64_t buf_size = sizeof(uint64_t) * p_query_count;
	{
		WGPUBufferDescriptor bd = {};
		bd.size = buf_size;
		bd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
		pool->resolve_buffer = wgpuDeviceCreateBuffer(device, &bd);
	}

	// Create readback buffer (CPU-readable staging buffer).
	{
		WGPUBufferDescriptor bd = {};
		bd.size = buf_size;
		bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
		pool->readback_buffer = wgpuDeviceCreateBuffer(device, &bd);
	}

	if (!pool->resolve_buffer || !pool->readback_buffer) {
		WARN_PRINT("WebGPU: Failed to create timestamp resolve/readback buffers.");
		if (pool->handle) { wgpuQuerySetRelease(pool->handle); pool->handle = nullptr; }
		if (pool->resolve_buffer) { wgpuBufferRelease(pool->resolve_buffer); pool->resolve_buffer = nullptr; }
		if (pool->readback_buffer) { wgpuBufferRelease(pool->readback_buffer); pool->readback_buffer = nullptr; }
		pool->is_real = false;
		return QueryPoolID(pool);
	}

	pool->is_real = true;
	return QueryPoolID(pool);
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_free(QueryPoolID p_pool_id) {
	WGQueryPool *pool = (WGQueryPool *)(p_pool_id.id);
	if (pool) {
		if (pool->handle) {
			wgpuQuerySetRelease(pool->handle);
		}
		if (pool->resolve_buffer) {
			wgpuBufferRelease(pool->resolve_buffer);
		}
		if (pool->readback_buffer) {
			wgpuBufferRelease(pool->readback_buffer);
		}
		if (pool->cpu_results) {
			memfree(pool->cpu_results);
		}
		delete pool;
	}
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) {
	WGQueryPool *pool = (WGQueryPool *)(p_pool_id.id);
	if (!pool || !pool->is_real) {
		memset(r_results, 0, sizeof(uint64_t) * p_query_count);
		return;
	}

	// Copy from the CPU shadow buffer (populated by the async readback callback).
	uint32_t copy_count = MIN(p_query_count, pool->count);
	memcpy(r_results, pool->cpu_results, sizeof(uint64_t) * copy_count);
	if (p_query_count > copy_count) {
		memset(r_results + copy_count, 0, sizeof(uint64_t) * (p_query_count - copy_count));
	}
}

uint64_t RenderingDeviceDriverWebGPU::timestamp_query_result_to_time(uint64_t p_result) {
	return p_result; // WebGPU timestamps are already in nanoseconds.
}

void RenderingDeviceDriverWebGPU::command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) {
	// No-op: WebGPU query sets don't need reset. Resolve happens in command_buffer_end().
}

// Forward declared at the top of the file; defined here.
static void _timestamp_readback_callback(WGPUMapAsyncStatus p_status, WGPUStringView p_message, void *p_userdata1, void *p_userdata2) {
	WGQueryPool *pool = (WGQueryPool *)p_userdata1;
	if (!pool) {
		return;
	}
	if (p_status == WGPUMapAsyncStatus_Success) {
		const void *data = wgpuBufferGetConstMappedRange(pool->readback_buffer, 0, sizeof(uint64_t) * pool->count);
		if (data) {
			memcpy(pool->cpu_results, data, sizeof(uint64_t) * pool->count);
		}
		wgpuBufferUnmap(pool->readback_buffer);
	}
	pool->readback_pending = false;
}

void RenderingDeviceDriverWebGPU::command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) {
	WGQueryPool *pool = (WGQueryPool *)(p_pool_id.id);
	if (!pool || !pool->is_real || p_index >= pool->count) {
		return;
	}

	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	// wgpuCommandEncoderWriteTimestamp requires no active render/compute pass.
	cmd->end_active_encoder();

	wgpuCommandEncoderWriteTimestamp(cmd->encoder, pool->handle, p_index);

	// Track this pool so we resolve it in command_buffer_end.
	bool already_tracked = false;
	for (uint32_t i = 0; i < cmd->written_query_pools.size(); i++) {
		if (cmd->written_query_pools[i] == pool) {
			already_tracked = true;
			break;
		}
	}
	if (!already_tracked) {
		cmd->written_query_pools.push_back(pool);
	}
}

// =============================================================================
// LABELS & DEBUG
// =============================================================================

void RenderingDeviceDriverWebGPU::command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	if (cmd->render_encoder) {
		wgpuRenderPassEncoderPushDebugGroup(cmd->render_encoder, WGPUStringView{ p_label_name, WGPU_STRLEN });
	} else if (cmd->compute_encoder) {
		wgpuComputePassEncoderPushDebugGroup(cmd->compute_encoder, WGPUStringView{ p_label_name, WGPU_STRLEN });
	} else if (cmd->encoder) {
		wgpuCommandEncoderPushDebugGroup(cmd->encoder, WGPUStringView{ p_label_name, WGPU_STRLEN });
	}
}

void RenderingDeviceDriverWebGPU::command_end_label(CommandBufferID p_cmd_buffer) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	if (cmd->render_encoder) {
		wgpuRenderPassEncoderPopDebugGroup(cmd->render_encoder);
	} else if (cmd->compute_encoder) {
		wgpuComputePassEncoderPopDebugGroup(cmd->compute_encoder);
	} else if (cmd->encoder) {
		wgpuCommandEncoderPopDebugGroup(cmd->encoder);
	}
}

void RenderingDeviceDriverWebGPU::command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);

	char marker[32];
	snprintf(marker, sizeof(marker), "breadcrumb_%u", p_data);

	if (cmd->render_encoder) {
		wgpuRenderPassEncoderInsertDebugMarker(cmd->render_encoder, WGPUStringView{ marker, WGPU_STRLEN });
	} else if (cmd->compute_encoder) {
		wgpuComputePassEncoderInsertDebugMarker(cmd->compute_encoder, WGPUStringView{ marker, WGPU_STRLEN });
	} else if (cmd->encoder) {
		wgpuCommandEncoderInsertDebugMarker(cmd->encoder, WGPUStringView{ marker, WGPU_STRLEN });
	}
}

// =============================================================================
// SUBMISSION
// =============================================================================

void RenderingDeviceDriverWebGPU::begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) {
	frame_index = p_frame_index;
	frames_drawn = p_frames_drawn;

	// Log performance counters once per second.
	perf.frames_since_log++;
	double now = EM_ASM_DOUBLE({ return performance.now(); });
	if (perf.last_log_time == 0) {
		perf.last_log_time = now;
	} else if (now - perf.last_log_time >= 1000.0) {
		double elapsed = (now - perf.last_log_time) / 1000.0;
		uint32_t fps = (uint32_t)(perf.frames_since_log / elapsed);
		EM_ASM({
			console.log('[PERF] fps=' + $0 + ' draws=' + $1 + ' SetBG=' + $2 + ' SetBG_skip=' + $3 + ' PC_write=' + $4 + ' PC_skip=' + $5 + ' RP=' + $6 + ' BG_miss=' + $7);
		}, fps, perf.draw_calls, perf.set_bind_group_calls, perf.set_bind_group_skipped,
				perf.push_constant_writes, perf.push_constant_skipped, perf.render_passes, perf.bind_group_cache_misses);
		perf.reset();
		perf.frames_since_log = 0;
		perf.last_log_time = now;
	}

	// Reset push constant ring buffer offset and shadow buffer tracking at the start of each frame.
	push_constant_ring_offset = 0;
	push_constant_shadow_dirty_start = UINT32_MAX;
	push_constant_shadow_dirty_end = 0;
}

void RenderingDeviceDriverWebGPU::end_segment() {
	// Nothing to do.
}

// =============================================================================
// MISC
// =============================================================================

void RenderingDeviceDriverWebGPU::set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) {
	// TODO: Call wgpuBufferSetLabel() / wgpuTextureSetLabel() etc. based on type.
}

uint64_t RenderingDeviceDriverWebGPU::get_resource_native_handle(DriverResource p_type, ID p_driver_id) {
	return p_driver_id.id;
}

uint64_t RenderingDeviceDriverWebGPU::get_total_memory_used() {
	return 0; // TODO: Track internally.
}

uint64_t RenderingDeviceDriverWebGPU::get_lazily_memory_used() {
	return 0;
}

uint64_t RenderingDeviceDriverWebGPU::limit_get(Limit p_limit) {
	switch (p_limit) {
		case LIMIT_MAX_BOUND_UNIFORM_SETS: return device_limits.maxBindGroups;
		case LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS: return device_limits.maxColorAttachments;
		case LIMIT_MAX_TEXTURES_PER_UNIFORM_SET: return device_limits.maxSampledTexturesPerShaderStage;
		case LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET: return device_limits.maxSamplersPerShaderStage;
		case LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET: return device_limits.maxStorageBuffersPerShaderStage;
		case LIMIT_MAX_STORAGE_IMAGES_PER_UNIFORM_SET: return device_limits.maxStorageTexturesPerShaderStage;
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_UNIFORM_SET: return device_limits.maxUniformBuffersPerShaderStage;
		case LIMIT_MAX_PUSH_CONSTANT_SIZE: return 128; // Emulated via ring buffer.
		case LIMIT_MAX_UNIFORM_BUFFER_SIZE: return device_limits.maxUniformBufferBindingSize;
		case LIMIT_MAX_TEXTURES_PER_SHADER_STAGE: return device_limits.maxSampledTexturesPerShaderStage;
		case LIMIT_MAX_TEXTURE_ARRAY_LAYERS: return device_limits.maxTextureArrayLayers;
		case LIMIT_MAX_TEXTURE_SIZE_1D: return device_limits.maxTextureDimension1D;
		case LIMIT_MAX_TEXTURE_SIZE_2D: return device_limits.maxTextureDimension2D;
		case LIMIT_MAX_TEXTURE_SIZE_3D: return device_limits.maxTextureDimension3D;
		case LIMIT_MAX_TEXTURE_SIZE_CUBE: return device_limits.maxTextureDimension2D; // Cube faces are 2D.
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTE_OFFSET: return device_limits.maxVertexBufferArrayStride;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTES: return device_limits.maxVertexAttributes;
		case LIMIT_MAX_VERTEX_INPUT_BINDINGS: return device_limits.maxVertexBuffers;
		case LIMIT_MAX_COMPUTE_SHARED_MEMORY_SIZE: return device_limits.maxComputeWorkgroupStorageSize;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X: return device_limits.maxComputeWorkgroupsPerDimension;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y: return device_limits.maxComputeWorkgroupsPerDimension;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Z: return device_limits.maxComputeWorkgroupsPerDimension;
		case LIMIT_MAX_COMPUTE_WORKGROUP_INVOCATIONS: return device_limits.maxComputeInvocationsPerWorkgroup;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X: return device_limits.maxComputeWorkgroupSizeX;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Y: return device_limits.maxComputeWorkgroupSizeY;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Z: return device_limits.maxComputeWorkgroupSizeZ;
		case LIMIT_MAX_SHADER_VARYINGS: return device_limits.maxInterStageShaderVariables;
		case LIMIT_SUBGROUP_SIZE: return 0; // Subgroups not available in WebGPU.
		case LIMIT_SUBGROUP_MIN_SIZE: return 0;
		case LIMIT_SUBGROUP_MAX_SIZE: return 0;
		case LIMIT_SUBGROUP_IN_SHADERS: return 0;
		case LIMIT_SUBGROUP_OPERATIONS: return 0;
		default: return 0;
	}
}

uint64_t RenderingDeviceDriverWebGPU::api_trait_get(ApiTrait p_trait) {
	switch (p_trait) {
		case API_TRAIT_HONORS_PIPELINE_BARRIERS: return 0; // No barriers in WebGPU.
		case API_TRAIT_SHADER_CHANGE_INVALIDATION: return SHADER_CHANGE_INVALIDATION_ALL_BOUND_UNIFORM_SETS;
		case API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT: return 256;
		case API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP: return 256;
		case API_TRAIT_SECONDARY_VIEWPORT_SCISSOR: return 0;
		case API_TRAIT_CLEARS_WITH_COPY_ENGINE: return 0;
		case API_TRAIT_USE_GENERAL_IN_COPY_QUEUES: return 0;
		case API_TRAIT_BUFFERS_REQUIRE_TRANSITIONS: return 0;
		case API_TRAIT_TEXTURE_OUTPUTS_REQUIRE_CLEARS: return 0;
		default: return RenderingDeviceDriver::api_trait_get(p_trait);
	}
}

bool RenderingDeviceDriverWebGPU::has_feature(Features p_feature) {
	switch (p_feature) {
		case SUPPORTS_HALF_FLOAT:
			return false; // WebGPU shader-f16 extension not reliably available; avoid f16 in generated SPIR-V.
		case SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS:
			return true; // WebGPU render passes work with no color attachments.
		case SUPPORTS_IMAGE_ATOMIC_32_BIT:
			return false; // WebGPU has no image atomics support.
		case SUPPORTS_MULTIVIEW:
			return false; // Not available in WebGPU.
		case SUPPORTS_ATTACHMENT_VRS:
			return false; // Not available in WebGPU.
		case SUPPORTS_METALFX_SPATIAL:
		case SUPPORTS_METALFX_TEMPORAL:
			return false; // Metal-only features.
		case SUPPORTS_BUFFER_DEVICE_ADDRESS:
			return false; // Not available in WebGPU.
		case SUPPORTS_VULKAN_MEMORY_MODEL:
			return false; // Not available in WebGPU.
		case SUPPORTS_FRAMEBUFFER_DEPTH_RESOLVE:
			return false; // Not available in WebGPU.
		case SUPPORTS_POINT_SIZE:
			return false; // Point size not controllable in WebGPU.
		default:
			return false;
	}
}

const RDD::MultiviewCapabilities &RenderingDeviceDriverWebGPU::get_multiview_capabilities() {
	return multiview_capabilities;
}

const RDD::FragmentShadingRateCapabilities &RenderingDeviceDriverWebGPU::get_fragment_shading_rate_capabilities() {
	return fsr_capabilities;
}

const RDD::FragmentDensityMapCapabilities &RenderingDeviceDriverWebGPU::get_fragment_density_map_capabilities() {
	return fdm_capabilities;
}

String RenderingDeviceDriverWebGPU::get_api_name() const {
	return "WebGPU";
}

String RenderingDeviceDriverWebGPU::get_api_version() const {
	return "1.0";
}

String RenderingDeviceDriverWebGPU::get_pipeline_cache_uuid() const {
	return "";
}

const RDD::Capabilities &RenderingDeviceDriverWebGPU::get_capabilities() const {
	return capabilities;
}

const RenderingShaderContainerFormat &RenderingDeviceDriverWebGPU::get_shader_container_format() const {
	return *shader_container_format;
}

#endif // WEBGPU_ENABLED
