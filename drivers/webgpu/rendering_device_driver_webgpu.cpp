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
		desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		desc.mappedAtCreation = false;
		push_constant_ring_buffer = wgpuDeviceCreateBuffer(device, &desc);
		ERR_FAIL_COND_V(push_constant_ring_buffer == nullptr, ERR_CANT_CREATE);
	}

	// Create a universal push constant bind group layout:
	//   binding 0, all stages, uniform buffer with dynamic offset.
	// All shaders with push constants use this same layout for their push
	// constant slot in the pipeline layout (hasDynamicOffset=true allows
	// reusing one bind group with different ring buffer offsets per draw).
	{
		WGPUBindGroupLayoutEntry pc_entry = {};
		pc_entry.binding = 0;
		pc_entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
		pc_entry.buffer.type = WGPUBufferBindingType_Uniform;
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
		bg_entry.binding = 0;
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

	// Create shader container format.
	shader_container_format = memnew(RenderingShaderContainerFormatWebGPU);

	return OK;
}

void RenderingDeviceDriverWebGPU::_check_capabilities() {
	capabilities.device_family = DEVICE_UNKNOWN; // TODO: Consider adding DEVICE_WEBGPU to enum.
	capabilities.version_major = 1;
	capabilities.version_minor = 0;

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

uint8_t *RenderingDeviceDriverWebGPU::buffer_map(BufferID p_buffer) {
	// WebGPU mapping is async and cannot be done synchronously in the browser.
	// Use a shadow CPU buffer for mapped data.
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	ERR_FAIL_NULL_V(buf, nullptr);
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
	// No persistent mapping in WebGPU.
	return nullptr;
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_dynamic_offsets(Span<BufferID> p_buffers) {
	return 0;
}

void RenderingDeviceDriverWebGPU::buffer_flush(BufferID p_buffer) {
	// If using shadow buffer, flush it.
	WGBuffer *buf = (WGBuffer *)(p_buffer.id);
	if (buf && buf->shadow_map && buf->map_dirty) {
		wgpuQueueWriteBuffer(queue, buf->handle, 0, buf->shadow_map, buf->size);
		buf->map_dirty = false;
	}
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_device_address(BufferID p_buffer) {
	return 0; // No device addresses in WebGPU.
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

	WGPUTextureDescriptor desc = {};
	desc.dimension = tex->dimension;
	desc.format = tex->format;
	desc.size.width = tex->width;
	desc.size.height = tex->height;
	desc.size.depthOrArrayLayers = (tex->dimension == WGPUTextureDimension_3D) ? tex->depth : tex->layers;
	desc.mipLevelCount = tex->mipmaps;
	desc.sampleCount = tex->sample_count;
	desc.usage = tex->usage;

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
	switch (p_format) {
		case DATA_FORMAT_R8G8B8A8_UNORM:
		case DATA_FORMAT_R8G8B8A8_SNORM:
		case DATA_FORMAT_R8G8B8A8_UINT:
		case DATA_FORMAT_R8G8B8A8_SINT:
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
		case DATA_FORMAT_R16_UINT: return WGPUTextureFormat_R16Uint;
		case DATA_FORMAT_R16_SINT: return WGPUTextureFormat_R16Sint;
		case DATA_FORMAT_R16_SFLOAT: return WGPUTextureFormat_R16Float;
		case DATA_FORMAT_R16G16_UINT: return WGPUTextureFormat_RG16Uint;
		case DATA_FORMAT_R16G16_SINT: return WGPUTextureFormat_RG16Sint;
		case DATA_FORMAT_R16G16_SFLOAT: return WGPUTextureFormat_RG16Float;
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
	// TODO: Poll with wgpuDeviceTick() or use callback-based completion.
	// For now, assume completion is immediate (single-threaded browser context).
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
		wgpuQueueSubmit(queue, wgpu_cmd_buffers.size(), wgpu_cmd_buffers.ptr());
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
			// In Emscripten, presentation happens automatically via requestAnimationFrame.
		}
	}

	// Clear finished command buffers (they are consumed by submit).
	for (uint32_t i = 0; i < p_cmd_buffers.size(); i++) {
		WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffers[i].id);
		if (cmd) {
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

	sc->width = width;
	sc->height = height;
	sc->configured = true;
	return OK;
}

RDD::FramebufferID RenderingDeviceDriverWebGPU::swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) {
	WGSwapChain *sc = (WGSwapChain *)(p_swap_chain.id);
	ERR_FAIL_NULL_V(sc, FramebufferID());
	if (!sc->configured) {
		// Not yet sized — request a resize so the RD layer calls swap_chain_resize().
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

	if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal ||
			surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
			surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
		r_resize_required = true;
		return FramebufferID();
	}
	ERR_FAIL_COND_V_MSG(
			surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal,
			FramebufferID(),
			"WebGPU: wgpuSurfaceGetCurrentTexture failed with unexpected status.");

	sc->current_texture = surface_texture.texture;

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
static WGPUShaderStage _stages_to_wgpu_visibility(uint32_t p_stage_mask) {
	WGPUShaderStage vis = WGPUShaderStage_None;
	if (p_stage_mask & (1u << RDD::SHADER_STAGE_VERTEX)) {
		vis = (WGPUShaderStage)(vis | WGPUShaderStage_Vertex);
	}
	if (p_stage_mask & (1u << RDD::SHADER_STAGE_FRAGMENT)) {
		vis = (WGPUShaderStage)(vis | WGPUShaderStage_Fragment);
	}
	if (p_stage_mask & (1u << RDD::SHADER_STAGE_COMPUTE)) {
		vis = (WGPUShaderStage)(vis | WGPUShaderStage_Compute);
	}
	// Tessellation stages map to vertex visibility as a safe fallback.
	if (p_stage_mask & ((1u << RDD::SHADER_STAGE_TESSELATION_CONTROL) | (1u << RDD::SHADER_STAGE_TESSELATION_EVALUATION))) {
		vis = (WGPUShaderStage)(vis | WGPUShaderStage_Vertex);
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

	// --- Create one WGPUShaderModule per stage ---
	Vector<RenderingShaderContainer::Shader> &stage_shaders = p_shader_container->shaders;
	for (int i = 0; i < stage_shaders.size(); i++) {
		const RenderingShaderContainer::Shader &s = stage_shaders[i];

		// The code_compressed_bytes holds raw SPIR-V (no compression — code_decompressed_size == 0).
		const PackedByteArray &spv_bytes = s.code_compressed_bytes;
		ERR_FAIL_COND_V_MSG(spv_bytes.is_empty(), ShaderID(), "WebGPU: empty SPIR-V for shader stage.");
		ERR_FAIL_COND_V_MSG(spv_bytes.size() % 4 != 0, ShaderID(), "WebGPU: SPIR-V size must be a multiple of 4.");

		WGPUShaderSourceSPIRV spirv_source = {};
		spirv_source.chain.sType = WGPUSType_ShaderSourceSPIRV;
		spirv_source.code = (const uint32_t *)(spv_bytes.ptr());
		spirv_source.codeSize = spv_bytes.size() / 4; // Number of 32-bit words.

		WGPUShaderModuleDescriptor mod_desc = {};
		mod_desc.nextInChain = (WGPUChainedStruct *)&spirv_source;

		WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &mod_desc);
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
		uint32_t entry_count = (uint32_t)set_uniforms.size();

		LocalVector<WGPUBindGroupLayoutEntry> entries;
		entries.resize(entry_count);

		WGShader::BindGroupInfo &bgi = shader->bind_group_infos[set];
		bgi.entries.resize(entry_count);

		for (uint32_t e = 0; e < entry_count; e++) {
			const RenderingDeviceCommons::ShaderUniform &u = set_uniforms[e];
			WGPUBindGroupLayoutEntry &entry = entries[e];
			WGShader::BindGroupEntry &bge = bgi.entries[e];

			entry = {};
			entry.binding = u.binding;
			entry.visibility = _stages_to_wgpu_visibility((uint32_t)u.stages);

			bge.godot_type = u.type;
			bge.layout_entry = entry; // Will be fully filled below.

			switch (u.type) {
				case RDD::UNIFORM_TYPE_SAMPLER: {
					entry.sampler.type = WGPUSamplerBindingType_Filtering;
				} break;

				case RDD::UNIFORM_TYPE_TEXTURE:
				case RDD::UNIFORM_TYPE_INPUT_ATTACHMENT: {
					entry.texture.sampleType = WGPUTextureSampleType_Float;
					entry.texture.viewDimension = WGPUTextureViewDimension_2D;
					entry.texture.multisampled = false;
				} break;

				case RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
					// Combined sampler+texture: WebGPU keeps them separate, but Godot's RD
					// presents them as a pair at the same binding slot.
					// Layout entry covers the texture side; sampler is at binding+1.
					entry.texture.sampleType = WGPUTextureSampleType_Float;
					entry.texture.viewDimension = WGPUTextureViewDimension_2D;
					entry.texture.multisampled = false;
				} break;

				case RDD::UNIFORM_TYPE_IMAGE: {
					entry.storageTexture.access = u.writable ? WGPUStorageTextureAccess_WriteOnly : WGPUStorageTextureAccess_ReadOnly;
					entry.storageTexture.format = WGPUTextureFormat_RGBA8Unorm; // Fallback.
					entry.storageTexture.viewDimension = WGPUTextureViewDimension_2D;
				} break;

				case RDD::UNIFORM_TYPE_UNIFORM_BUFFER: {
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
				} break;

				case RDD::UNIFORM_TYPE_STORAGE_BUFFER: {
					entry.buffer.type = u.writable ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
				} break;

				// Dynamic variants treated as static — dynamic offsets not yet implemented.
				case RDD::UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC: {
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
				} break;

				case RDD::UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
					entry.buffer.type = u.writable ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
				} break;

				// WebGPU has no texel buffers (TBOs); emulate as storage buffers.
				case RDD::UNIFORM_TYPE_TEXTURE_BUFFER:
				case RDD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER: {
					entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
				} break;

				case RDD::UNIFORM_TYPE_IMAGE_BUFFER: {
					entry.buffer.type = WGPUBufferBindingType_Storage;
					entry.buffer.hasDynamicOffset = false;
					entry.buffer.minBindingSize = 0;
				} break;

				default:
					WARN_PRINT_ONCE(vformat("WebGPU: unhandled uniform type %d in bind group layout.", (int)u.type));
					entry.buffer.type = WGPUBufferBindingType_Uniform;
					break;
			}
			bge.layout_entry = entry;
		}

		WGPUBindGroupLayoutDescriptor layout_desc = {};
		layout_desc.entryCount = entry_count;
		layout_desc.entries = entries.size() > 0 ? entries.ptr() : nullptr;
		shader->bind_group_layouts[set] = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
		ERR_FAIL_COND_V_MSG(shader->bind_group_layouts[set] == nullptr, ShaderID(), "WebGPU: wgpuDeviceCreateBindGroupLayout failed.");
	}

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
			all_layouts[i] = push_constant_bind_group_layout;
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

	// Each BoundUniform may expand to one or two WGPUBindGroupEntry items.
	LocalVector<WGPUBindGroupEntry> entries;
	entries.reserve(p_uniforms.size() * 2);

	for (uint32_t i = 0; i < p_uniforms.size(); i++) {
		const BoundUniform &uniform = p_uniforms[i];
		if (uniform.immutable_sampler) {
			continue; // Immutable samplers are pre-specified in the pipeline layout.
		}

		switch (uniform.type) {
			case UNIFORM_TYPE_SAMPLER: {
				for (uint32_t j = 0; j < uniform.ids.size(); j++) {
					WGPUBindGroupEntry entry = {};
					entry.binding = uniform.binding + j; // Arrays occupy consecutive bindings.
					entry.sampler = (WGPUSampler)(uniform.ids[j].id);
					entries.push_back(entry);
				}
			} break;

			case UNIFORM_TYPE_TEXTURE:
			case UNIFORM_TYPE_INPUT_ATTACHMENT: {
				for (uint32_t j = 0; j < uniform.ids.size(); j++) {
					WGTexture *tex = (WGTexture *)(uniform.ids[j].id);
					ERR_CONTINUE_MSG(tex == nullptr, "WebGPU: null texture in uniform set.");
					WGPUBindGroupEntry entry = {};
					entry.binding = uniform.binding + j;
					entry.textureView = tex->default_view;
					entries.push_back(entry);
				}
			} break;

			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE: {
				// Godot pairs sampler+texture as ids[j*2] / ids[j*2+1].
				// WebGPU needs separate sampler and texture entries.
				// We place the sampler at @binding(N) and texture at @binding(N+1).
				for (uint32_t j = 0; j < uniform.ids.size() / 2; j++) {
					WGPUSampler sampler = (WGPUSampler)(uniform.ids[j * 2 + 0].id);
					WGTexture *tex = (WGTexture *)(uniform.ids[j * 2 + 1].id);
					if (sampler) {
						WGPUBindGroupEntry se = {};
						se.binding = uniform.binding + j * 2 + 0;
						se.sampler = sampler;
						entries.push_back(se);
					}
					if (tex && tex->default_view) {
						WGPUBindGroupEntry te = {};
						te.binding = uniform.binding + j * 2 + 1;
						te.textureView = tex->default_view;
						entries.push_back(te);
					}
				}
			} break;

			case UNIFORM_TYPE_IMAGE: {
				for (uint32_t j = 0; j < uniform.ids.size(); j++) {
					WGTexture *tex = (WGTexture *)(uniform.ids[j].id);
					ERR_CONTINUE_MSG(tex == nullptr, "WebGPU: null texture in image uniform.");
					WGPUBindGroupEntry entry = {};
					entry.binding = uniform.binding + j;
					entry.textureView = tex->default_view;
					entries.push_back(entry);
				}
			} break;

			case UNIFORM_TYPE_UNIFORM_BUFFER: {
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in uniform set.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding;
				entry.buffer = buf->handle;
				entry.offset = 0;
				entry.size = buf->size;
				entries.push_back(entry);
			} break;

			case UNIFORM_TYPE_STORAGE_BUFFER: {
				WGBuffer *buf = (WGBuffer *)(uniform.ids[0].id);
				ERR_CONTINUE_MSG(buf == nullptr, "WebGPU: null buffer in storage uniform.");
				WGPUBindGroupEntry entry = {};
				entry.binding = uniform.binding;
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

	WGPUBindGroupDescriptor bg_desc = {};
	bg_desc.layout = layout;
	bg_desc.entryCount = entries.size();
	bg_desc.entries = entries.size() > 0 ? entries.ptr() : nullptr;

	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device, &bg_desc);
	ERR_FAIL_COND_V_MSG(bg == nullptr, UniformSetID(), "WebGPU: wgpuDeviceCreateBindGroup failed.");

	WGUniformSet *us = new WGUniformSet();
	us->handle = bg;
	us->set_index = p_set_index;
	return UniformSetID(us);
}

void RenderingDeviceDriverWebGPU::uniform_set_free(UniformSetID p_uniform_set) {
	WGUniformSet *us = (WGUniformSet *)(p_uniform_set.id);
	ERR_FAIL_NULL(us);
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
		return;
	}
	if (p_shader->push_constant_bind_group == UINT32_MAX || !push_constant_bind_group) {
		p_cmd_buf->push_constants_dirty = false;
		return; // Shader has no push constants.
	}

	// Write push constant data to ring buffer.
	uint32_t aligned_size = (p_cmd_buf->push_constant_data_len + PUSH_CONSTANT_SLOT_ALIGNMENT - 1) & ~(PUSH_CONSTANT_SLOT_ALIGNMENT - 1);

	if (push_constant_ring_offset + aligned_size > PUSH_CONSTANT_RING_SIZE) {
		push_constant_ring_offset = 0; // Wrap around.
	}

	wgpuQueueWriteBuffer(queue, push_constant_ring_buffer, push_constant_ring_offset, p_cmd_buf->push_constant_data, p_cmd_buf->push_constant_data_len);

	uint32_t dynamic_offset = push_constant_ring_offset;

	// Bind the push constant ring buffer bind group with a dynamic offset.
	if (p_cmd_buf->render_encoder) {
		wgpuRenderPassEncoderSetBindGroup(p_cmd_buf->render_encoder, p_shader->push_constant_bind_group, push_constant_bind_group, 1, &dynamic_offset);
	} else if (p_cmd_buf->compute_encoder) {
		wgpuComputePassEncoderSetBindGroup(p_cmd_buf->compute_encoder, p_shader->push_constant_bind_group, push_constant_bind_group, 1, &dynamic_offset);
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

	// End any active encoder.
	cmd->end_active_encoder();

	// Store render state.
	cmd->render_state.render_pass = rp;
	cmd->render_state.framebuffer = fb;
	cmd->render_state.current_subpass = 0;

	ERR_FAIL_COND(rp->subpasses.size() == 0);
	const WGRenderPass::SubpassInfo &subpass = rp->subpasses[0];

	// --- Helper lambdas for op mapping ---
	auto map_load_op = [](AttachmentLoadOp op) -> WGPULoadOp {
		switch (op) {
			case ATTACHMENT_LOAD_OP_LOAD: return WGPULoadOp_Load;
			case ATTACHMENT_LOAD_OP_CLEAR: return WGPULoadOp_Clear;
			default: return WGPULoadOp_Load; // DONT_CARE → Load (safer than Undefined)
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

	// End current render pass.
	if (cmd->render_encoder) {
		wgpuRenderPassEncoderEnd(cmd->render_encoder);
		wgpuRenderPassEncoderRelease(cmd->render_encoder);
		cmd->render_encoder = nullptr;
	}

	// Advance to next subpass.
	cmd->render_state.current_subpass++;

	// TODO: Build new WGPURenderPassDescriptor for the next subpass.
	// TODO: Handle input attachments (previous subpass outputs become texture inputs).
	// TODO: Begin new render pass encoder.

	WARN_PRINT_ONCE("WebGPU: command_next_render_subpass not yet fully implemented.");
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
		wgpuRenderPassEncoderSetScissorRect(cmd->render_encoder, sr.position.x, sr.position.y, sr.size.x, sr.size.y);
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

	for (uint32_t i = 0; i < p_set_count; i++) {
		WGUniformSet *us = (WGUniformSet *)(p_uniform_sets[i].id);
		if (us && us->handle) {
			// TODO: Handle dynamic offsets properly.
			wgpuRenderPassEncoderSetBindGroup(cmd->render_encoder, p_first_set_index + i, us->handle, 0, nullptr);
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

	wgpuRenderPassEncoderDraw(cmd->render_encoder, p_vertex_count, p_instance_count, p_base_vertex, p_first_instance);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	WGCommandBuffer *cmd = (WGCommandBuffer *)(p_cmd_buffer.id);
	ERR_FAIL_NULL(cmd);
	ERR_FAIL_COND(!cmd->render_encoder);

	if (cmd->render_state.current_pipeline) {
		_flush_push_constants(cmd, cmd->render_state.current_pipeline->shader);
	}

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
	LocalVector<WGPUConstantEntry> spec_entries;
	LocalVector<CharString> spec_keys; // Keep alive until wgpuDeviceCreateRenderPipeline().
	spec_entries.resize(p_specialization_constants.size());
	spec_keys.resize(p_specialization_constants.size());
	for (uint32_t i = 0; i < p_specialization_constants.size(); i++) {
		const PipelineSpecializationConstant &sc = p_specialization_constants[i];
		spec_keys[i] = String::num_int64(sc.constant_id).utf8();
		spec_entries[i] = {};
		spec_entries[i].key = { spec_keys[i].get_data(), (size_t)spec_keys[i].length() };
		if (sc.type == PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL) {
			spec_entries[i].value = sc.bool_value ? 1.0 : 0.0;
		} else if (sc.type == PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT) {
			spec_entries[i].value = (double)sc.float_value;
		} else {
			spec_entries[i].value = (double)sc.int_value;
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
	vertex_state.module = shader->stage_modules[SHADER_STAGE_VERTEX];
	vertex_state.entryPoint = { "main", WGPU_STRLEN };
	vertex_state.bufferCount = vb_layouts.size();
	vertex_state.buffers = vb_layouts.ptr();
	if (!spec_entries.is_empty()) {
		vertex_state.constantCount = spec_entries.size();
		vertex_state.constants = spec_entries.ptr();
	}

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
	frag.module = shader->stage_modules[SHADER_STAGE_FRAGMENT];
	frag.entryPoint = { "main", WGPU_STRLEN };
	frag.targetCount = color_target_count;
	frag.targets = color_targets.ptr();
	if (!spec_entries.is_empty()) {
		frag.constantCount = spec_entries.size();
		frag.constants = spec_entries.ptr();
	}

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

	WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
	ERR_FAIL_COND_V_MSG(!pipeline, PipelineID(), "WebGPU: Failed to create render pipeline.");

	WGPipelineWrapper *pw = new WGPipelineWrapper();
	pw->type = WGPipelineWrapper::RENDER;
	pw->render_handle = pipeline;
	pw->shader = shader;
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

	for (uint32_t i = 0; i < p_set_count; i++) {
		WGUniformSet *us = (WGUniformSet *)(p_uniform_sets[i].id);
		if (us && us->handle) {
			wgpuComputePassEncoderSetBindGroup(cmd->compute_encoder, p_first_set_index + i, us->handle, 0, nullptr);
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

	// Specialization constants.
	LocalVector<WGPUConstantEntry> spec_entries;
	LocalVector<CharString> spec_keys;
	spec_entries.resize(p_specialization_constants.size());
	spec_keys.resize(p_specialization_constants.size());
	for (uint32_t i = 0; i < p_specialization_constants.size(); i++) {
		const PipelineSpecializationConstant &sc = p_specialization_constants[i];
		spec_keys[i] = String::num_int64(sc.constant_id).utf8();
		spec_entries[i] = {};
		spec_entries[i].key = { spec_keys[i].get_data(), (size_t)spec_keys[i].length() };
		if (sc.type == PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL) {
			spec_entries[i].value = sc.bool_value ? 1.0 : 0.0;
		} else if (sc.type == PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT) {
			spec_entries[i].value = (double)sc.float_value;
		} else {
			spec_entries[i].value = (double)sc.int_value;
		}
	}

	WGPUComputePipelineDescriptor desc = {};
	desc.layout = shader->pipeline_layout;
	desc.compute.module = shader->stage_modules[SHADER_STAGE_COMPUTE];
	desc.compute.entryPoint = { "main", WGPU_STRLEN };
	if (!spec_entries.is_empty()) {
		desc.compute.constantCount = spec_entries.size();
		desc.compute.constants = spec_entries.ptr();
	}

	WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device, &desc);
	ERR_FAIL_COND_V_MSG(!pipeline, PipelineID(), "WebGPU: Failed to create compute pipeline.");

	WGPipelineWrapper *pw = new WGPipelineWrapper();
	pw->type = WGPipelineWrapper::COMPUTE;
	pw->compute_handle = pipeline;
	pw->shader = shader;
	return PipelineID(pw);
}

// =============================================================================
// QUERIES
// =============================================================================

RDD::QueryPoolID RenderingDeviceDriverWebGPU::timestamp_query_pool_create(uint32_t p_query_count) {
	// TODO: Check if timestamp-query feature is available.
	// If available: wgpuDeviceCreateQuerySet() with type=Timestamp.
	return QueryPoolID(); // Null — not supported initially.
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
		delete pool;
	}
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) {
	// TODO: Resolve query set to buffer, map buffer, copy results.
	memset(r_results, 0, sizeof(uint64_t) * p_query_count);
}

uint64_t RenderingDeviceDriverWebGPU::timestamp_query_result_to_time(uint64_t p_result) {
	return p_result; // WebGPU timestamps are already in nanoseconds.
}

void RenderingDeviceDriverWebGPU::command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) {
	// No-op: WebGPU query sets don't need reset.
}

void RenderingDeviceDriverWebGPU::command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) {
	// TODO: wgpuCommandEncoderWriteTimestamp(encoder, querySet, index) if available.
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

	// Reset push constant ring buffer offset at the start of each frame.
	push_constant_ring_offset = 0;
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
	// TODO: Query from WGPUSupportedLimits. See RESEARCH.md Appendix C for full mapping.
	switch (p_limit) {
		case LIMIT_MAX_BOUND_UNIFORM_SETS: return 4;
		case LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS: return 8;
		case LIMIT_MAX_TEXTURES_PER_UNIFORM_SET: return 16;
		case LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET: return 16;
		case LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET: return 8;
		case LIMIT_MAX_STORAGE_IMAGES_PER_UNIFORM_SET: return 4;
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_UNIFORM_SET: return 12;
		case LIMIT_MAX_PUSH_CONSTANT_SIZE: return 128;
		case LIMIT_MAX_UNIFORM_BUFFER_SIZE: return 65536;
		// WebGPU spec minimum-guaranteed texture limits.
		case LIMIT_MAX_TEXTURE_ARRAY_LAYERS: return 256;
		case LIMIT_MAX_TEXTURE_SIZE_1D: return 8192;
		case LIMIT_MAX_TEXTURE_SIZE_2D: return 8192;
		case LIMIT_MAX_TEXTURE_SIZE_3D: return 2048;
		case LIMIT_MAX_TEXTURE_SIZE_CUBE: return 8192;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTE_OFFSET: return 2048;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTES: return 16;
		case LIMIT_MAX_VERTEX_INPUT_BINDINGS: return 8;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X: return 65535;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y: return 65535;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Z: return 65535;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X: return 256;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Y: return 256;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Z: return 64;
		case LIMIT_SUBGROUP_SIZE: return 0; // Subgroups not guaranteed.
		case LIMIT_SUBGROUP_MIN_SIZE: return 0;
		case LIMIT_SUBGROUP_MAX_SIZE: return 0;
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
	// TODO: Check WebGPU device features.
	return false;
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
