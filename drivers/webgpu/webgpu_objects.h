/**************************************************************************/
/*  webgpu_objects.h                                                      */
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

#pragma once

#ifdef WEBGPU_ENABLED

#include "servers/rendering/rendering_device_driver.h"

#include <webgpu/webgpu.h>

using RDD = RenderingDeviceDriver;

// =============================================================================
// Buffer
// =============================================================================

struct WGBuffer {
	WGPUBuffer handle = nullptr;
	uint64_t size = 0;
	WGPUBufferUsage usage = 0;
	uint8_t *shadow_map = nullptr; // For CPU-side mapping emulation.
	bool map_dirty = false;
	bool is_readback = false;      // True for staging buffers that need GPU→CPU readback.
	bool map_complete = false;     // Set by async map callback.

	// Dynamic persistent rotation (Task 7.5).
	// `frame_idx` is UINT32_MAX for non-dynamic buffers. For BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT
	// buffers, buffer_create sets frame_idx=0 and per_frame_size to the aligned single-frame
	// slice. The physical buffer holds frame_count slices; each frame the CPU writes to one
	// slice and the GPU reads from it via a dynamic offset of frame_idx * per_frame_size.
	uint32_t frame_idx = UINT32_MAX;
	uint64_t per_frame_size = 0;
	bool is_dynamic() const { return frame_idx != UINT32_MAX; }
};

// =============================================================================
// Texture
// =============================================================================

struct WGTexture {
	WGPUTexture handle = nullptr; // null for shared/sliced views (they don't own the GPU texture)
	WGPUTexture view_source = nullptr; // always the owning WGPUTexture; inherited by shared/sliced textures
	WGPUTextureView default_view = nullptr;
	uint32_t debug_create_id = UINT32_MAX; // Monotonic counter for correlating [WGTEX] log lines.
	WGPUTextureFormat format = WGPUTextureFormat_Undefined;
	// Godot-side DataFormat remembered at creation so readback / allocation-size
	// paths can look up true bytes-per-pixel instead of hardcoding bpp=4.
	RDD::DataFormat rd_format = RDD::DATA_FORMAT_MAX;
	WGPUTextureDimension dimension = WGPUTextureDimension_2D;
	WGPUTextureViewDimension view_dimension = WGPUTextureViewDimension_2D;
	uint32_t width = 0, height = 0, depth = 1;
	uint32_t mipmaps = 1;
	uint32_t layers = 1;
	uint32_t sample_count = 1;
	// For slice views (texture_create_shared_from_slice), the base offset of
	// this view into the parent texture. 0/0 for non-slice textures.
	uint32_t base_mipmap = 0;
	uint32_t base_layer = 0;
	WGPUTextureUsage usage = 0;
	bool is_from_swap_chain = false;
};

// =============================================================================
// Sampler (stored directly as WGPUSampler handle in SamplerID)
// =============================================================================

// =============================================================================
// Vertex Format
// =============================================================================

struct WGVertexFormat {
	struct Attribute {
		uint32_t location = 0;
		WGPUVertexFormat format = (WGPUVertexFormat)0; // 0 = no valid format (Undefined was removed in Dawn)
		uint64_t offset = 0;
		uint32_t binding = 0;
	};
	struct Binding {
		uint32_t stride = 0;
		WGPUVertexStepMode step_mode = WGPUVertexStepMode_Vertex;
	};
	LocalVector<Attribute> attributes;
	LocalVector<Binding> bindings;
};

// =============================================================================
// Shader
// =============================================================================

struct WGShader {
	// Per-stage shader modules. Indexed by RDD::ShaderStage enum value.
	WGPUShaderModule stage_modules[6] = {}; // SHADER_STAGE_MAX = 6 (vertex/frag/tess×2/compute/max)
	WGPUShaderModule module = nullptr; // Legacy alias — points to first non-null module.

	// Per-stage raw SPIR-V bytes. Stored for deferred specialization constant patching.
	PackedByteArray stage_spirv[6];

	WGPUPipelineLayout pipeline_layout = nullptr;
	LocalVector<WGPUBindGroupLayout> bind_group_layouts; // One per descriptor set.

	// Push constant emulation.
	uint32_t push_constant_size = 0;
	uint32_t push_constant_bind_group = UINT32_MAX; // UINT32_MAX = no push constants.
	uint32_t push_constant_binding = 0;
	BitField<RDD::ShaderStage> push_constant_stages;
	// Merged bind group layout for the PC group when it also has material uniforms.
	// nullptr = PC group has no other uniforms; pipeline uses the universal PC-only layout.
	WGPUBindGroupLayout merged_pc_group_layout = nullptr;

	// Reflection data for uniform set creation:
	//   bind_group_infos[set_index].entries[binding_index] → layout entry + Godot type.
	struct BindGroupEntry {
		WGPUBindGroupLayoutEntry layout_entry = {};
		RDD::UniformType godot_type = RDD::UNIFORM_TYPE_MAX;
		uint32_t array_length = 0; // >1 for binding_array (e.g., texture arrays)
	};
	struct BindGroupInfo {
		LocalVector<BindGroupEntry> entries;
	};
	LocalVector<BindGroupInfo> bind_group_infos;

	// Depth alias bindings: maps (set << 16 | alias_binding) → depth_binding.
	// Used by uniform_set_create to add extra bind group entries for split depth textures.
	HashMap<uint32_t, uint32_t> depth_alias_bindings;

	String name;
};

// =============================================================================
// Render Pass (metadata — no WebGPU object)
// =============================================================================

struct WGRenderPass {
	struct SubpassInfo {
		LocalVector<RDD::AttachmentReference> input_references;
		LocalVector<RDD::AttachmentReference> color_references;
		RDD::AttachmentReference depth_stencil_reference;
		LocalVector<RDD::AttachmentReference> resolve_references;
	};
	LocalVector<RDD::Attachment> attachments;
	LocalVector<SubpassInfo> subpasses;
	uint32_t view_count = 0;
	bool is_swap_chain_pass = false; // True for swap chain render passes — used to strip alpha writes.
};

// =============================================================================
// Framebuffer (metadata — no WebGPU object)
// =============================================================================

struct WGFramebuffer {
	LocalVector<WGTexture *> attachments;
	LocalVector<WGPUTextureView> attachment_views;
	uint32_t width = 0, height = 0;
	WGRenderPass *render_pass = nullptr;
};

// =============================================================================
// Pipeline
// =============================================================================

struct WGPipelineWrapper {
	enum Type { RENDER, COMPUTE };
	Type type = RENDER;
	union {
		WGPURenderPipeline render_handle;
		WGPUComputePipeline compute_handle;
	};
	WGShader *shader = nullptr;
	// Specialized shader modules created with pipeline-specific specialization constants.
	// If non-null, these are owned by this pipeline and must be released.
	WGPUShaderModule specialized_modules[6] = {};

	WGPipelineWrapper() : render_handle(nullptr) {}
};

// =============================================================================
// Uniform Set (Bind Group)
// =============================================================================

struct WGUniformSet {
	WGPUBindGroup handle = nullptr;
	uint32_t set_index = 0;
	LocalVector<WGPUTextureView> temp_views; // Re-dimensioned views for Cube↔2D fixups.

	// Rebind cache: when a bind group is created with shader A's BGL but
	// needs to be used with shader B's pipeline (different BGL), we
	// re-create the bind group with the correct BGL and cache it.
	LocalVector<WGPUBindGroupEntry> cached_entries;
	WGShader *source_shader = nullptr;
	HashMap<WGPUBindGroupLayout, WGPUBindGroup> rebind_cache;

	// Maps binding index → WGTexture* for texture entries, used during rebind
	// to create compatible views when the target BGL has a different dimension.
	HashMap<uint32_t, WGTexture *> bound_textures;

	// Dynamic buffers in binding order (Task 7.5). Populated by uniform_set_create
	// whenever a UNIFORM_TYPE_*_BUFFER_DYNAMIC binding resolves to a WGBuffer with
	// is_dynamic()==true. command_bind_*_uniform_sets walks this list in order to
	// unpack per-set dynamic offsets from the mask returned by
	// uniform_sets_get_dynamic_offsets and pass them to wgpuRenderPassEncoderSetBindGroup.
	LocalVector<WGBuffer *> dynamic_buffers;
};

// =============================================================================
// Swap Chain
// =============================================================================

struct WGSwapChain {
	WGPUSurface surface = nullptr;
	WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
	uint32_t width = 0, height = 0;
	WGRenderPass *render_pass = nullptr;
	uint64_t surface_id = 0; // Context driver SurfaceID — used to look up size on resize.
	WGPUTexture current_texture = nullptr; // Released after each frame.
	WGPUTextureView current_view = nullptr;
	WGFramebuffer *current_framebuffer = nullptr;
	bool configured = false;
};

// =============================================================================
// Command Queue / Pool / Buffer
// =============================================================================

struct WGCommandQueue {
	WGPUQueue queue = nullptr;
};

struct WGCommandPool {
	RDD::CommandBufferType buffer_type = RDD::COMMAND_BUFFER_TYPE_PRIMARY;
};

struct WGQueryPool; // Forward declaration for WGCommandBuffer.

struct WGCommandBuffer {
	WGPUCommandEncoder encoder = nullptr;
	WGPUCommandBuffer finished_buffer = nullptr;

	// Current active encoder states.
	WGPURenderPassEncoder render_encoder = nullptr;
	WGPUComputePassEncoder compute_encoder = nullptr;

	enum ActiveEncoder { NONE, RENDER, COMPUTE };
	ActiveEncoder active_encoder = NONE;

	// Push constant emulation state.
	static constexpr uint32_t MAX_PUSH_CONSTANT_SIZE = 128;
	uint8_t push_constant_data[MAX_PUSH_CONSTANT_SIZE] = {};
	uint32_t push_constant_data_len = 0;
	bool push_constants_dirty = false;
	// Combined bind group for the PC group (includes both material resources and PC ring buffer).
	// Updated by command_bind_render_uniform_sets when the PC group is bound.
	WGPUBindGroup current_pc_bind_group = nullptr;
	// Task 7.5: When the PC group also contains material dynamic-offset UBOs,
	// _flush_push_constants must preserve those offsets while patching in the new
	// PC ring offset. Stored in binding order; the PC ring's own slot is appended
	// at pc_group_material_dyn_count by _flush_push_constants each draw.
	static constexpr uint32_t MAX_PC_GROUP_MATERIAL_DYN = 7; // MAX_DYNAMIC_BUFFERS - 1 (PC ring takes 1 slot).
	uint32_t pc_group_material_dyn_offsets[MAX_PC_GROUP_MATERIAL_DYN] = {};
	uint32_t pc_group_material_dyn_count = 0;

	// Render state tracking.
	struct RenderState {
		WGRenderPass *render_pass = nullptr;
		WGFramebuffer *framebuffer = nullptr;
		uint32_t current_subpass = 0;
		WGPipelineWrapper *current_pipeline = nullptr;
		uint32_t render_area_width = 0;
		uint32_t render_area_height = 0;
		// All textures used as render attachments across the ENTIRE encoder.
		// Used to detect cross-pass sync scope violations when a new pass
		// reads a texture that was written as an attachment in any earlier pass.
		static constexpr uint32_t MAX_ATTACHMENT_TEXTURES = 64;
		WGPUTexture all_attachment_textures[MAX_ATTACHMENT_TEXTURES] = {};
		uint32_t all_attachment_count = 0;
		void reset_all_attachment_textures() { all_attachment_count = 0; }
		void add_attachment_texture(WGPUTexture t) {
			if (!t) return;
			for (uint32_t i = 0; i < all_attachment_count; i++) {
				if (all_attachment_textures[i] == t) return;
			}
			if (all_attachment_count < MAX_ATTACHMENT_TEXTURES) {
				all_attachment_textures[all_attachment_count++] = t;
			}
		}
		bool was_attachment_texture(WGPUTexture t) const {
			for (uint32_t i = 0; i < all_attachment_count; i++) {
				if (all_attachment_textures[i] == t) return true;
			}
			return false;
		}
		// Textures used as render attachments in the CURRENT pass only.
		// Used to detect intra-pass sync scope violations when a uniform
		// set binds a texture that is also an attachment of the active pass.
		WGPUTexture current_pass_attachments[MAX_ATTACHMENT_TEXTURES] = {};
		uint32_t current_pass_attachment_count = 0;
		void reset_current_pass_attachments() { current_pass_attachment_count = 0; }
		void add_current_pass_attachment(WGPUTexture t) {
			if (!t) return;
			for (uint32_t i = 0; i < current_pass_attachment_count; i++) {
				if (current_pass_attachments[i] == t) return;
			}
			if (current_pass_attachment_count < MAX_ATTACHMENT_TEXTURES) {
				current_pass_attachments[current_pass_attachment_count++] = t;
			}
		}
		bool is_current_pass_attachment(WGPUTexture t) const {
			for (uint32_t i = 0; i < current_pass_attachment_count; i++) {
				if (current_pass_attachments[i] == t) return true;
			}
			return false;
		}
	} render_state;

	// Bind group state tracking for redundancy elimination.
	// Tracks what's currently bound at each bind group slot to skip redundant SetBindGroup calls.
	static constexpr uint32_t MAX_BIND_GROUPS = 4;
	WGPUBindGroup bound_bind_groups[MAX_BIND_GROUPS] = {};
	WGShader *bound_shader = nullptr; // Shader whose BGL the bound groups were created for.

	void invalidate_bind_groups() {
		for (uint32_t i = 0; i < MAX_BIND_GROUPS; i++) {
			bound_bind_groups[i] = nullptr;
		}
		bound_shader = nullptr;
		current_pc_bind_group = nullptr;
		pc_group_material_dyn_count = 0;
	}

	// Track query pools that had timestamps written, for resolve at command buffer end.
	LocalVector<WGQueryPool *> written_query_pools;

	// Helpers.
	void end_active_encoder() {
		if (render_encoder) {
			wgpuRenderPassEncoderEnd(render_encoder);
			wgpuRenderPassEncoderRelease(render_encoder);
			render_encoder = nullptr;
		}
		if (compute_encoder) {
			wgpuComputePassEncoderEnd(compute_encoder);
			wgpuComputePassEncoderRelease(compute_encoder);
			compute_encoder = nullptr;
		}
		active_encoder = NONE;
	}
};

// =============================================================================
// Fence
// =============================================================================

struct WGFence {
	bool signaled = false;
	uint64_t submission_id = 0;
};

// =============================================================================
// Semaphore (mostly no-op on WebGPU — single queue)
// =============================================================================

struct WGSemaphore {};

// =============================================================================
// Query Pool
// =============================================================================

struct WGQueryPool {
	WGPUQuerySet handle = nullptr;
	WGPUBuffer resolve_buffer = nullptr; // GPU buffer for query set resolve (CopySrc | QueryResolve).
	WGPUBuffer readback_buffer = nullptr; // CPU-readable staging buffer (CopyDst | MapRead).
	uint32_t count = 0;
	bool is_real = false; // True if backed by actual timestamp-query hardware.

	// Shadow CPU buffer for async readback results.
	uint64_t *cpu_results = nullptr;
	bool readback_pending = false;
};

#endif // WEBGPU_ENABLED
