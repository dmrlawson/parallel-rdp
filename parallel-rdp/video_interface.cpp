/* Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "video_interface.hpp"
#include "luts.hpp"

#ifndef PARALLEL_RDP_SHADER_DIR
#include "shaders/slangmosh.hpp"
#endif

namespace RDP
{
void VideoInterface::set_device(Vulkan::Device *device_)
{
	device = device_;
	init_gamma_table();

	if (const char *env = getenv("VI_DEBUG"))
		debug_channel = strtol(env, nullptr, 0) != 0;
	if (const char *env = getenv("VI_DEBUG_X"))
		filter_debug_channel_x = strtol(env, nullptr, 0);
	if (const char *env = getenv("VI_DEBUG_Y"))
		filter_debug_channel_y = strtol(env, nullptr, 0);
}

int VideoInterface::resolve_shader_define(const char *name, const char *define) const
{
	if (strcmp(define, "DEBUG_ENABLE") == 0)
		return int(debug_channel);
	else
		return 0;
}

void VideoInterface::message(const std::string &tag, uint32_t code, uint32_t x, uint32_t y, uint32_t, uint32_t num_words,
                             const Vulkan::DebugChannelInterface::Word *words)
{
	if (filter_debug_channel_x >= 0 && x != uint32_t(filter_debug_channel_x))
		return;
	if (filter_debug_channel_y >= 0 && y != uint32_t(filter_debug_channel_y))
		return;

	switch (num_words)
	{
	case 1:
		LOGI("(%u, %u), line %d.\n", x, y, words[0].s32);
		break;

	case 2:
		LOGI("(%u, %u), line %d: (%d).\n", x, y, words[0].s32, words[1].s32);
		break;

	case 3:
		LOGI("(%u, %u), line %d: (%d, %d).\n", x, y, words[0].s32, words[1].s32, words[2].s32);
		break;

	case 4:
		LOGI("(%u, %u), line %d: (%d, %d, %d).\n", x, y,
		     words[0].s32, words[1].s32, words[2].s32, words[3].s32);
		break;

	default:
		LOGE("Unknown number of generic parameters: %u\n", num_words);
		break;
	}
}

void VideoInterface::init_gamma_table()
{
	Vulkan::BufferCreateInfo info = {};
	info.domain = Vulkan::BufferDomain::Device;
	info.size = sizeof(gamma_table);
	info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;

	gamma_lut = device->create_buffer(info, gamma_table);

	Vulkan::BufferViewCreateInfo view = {};
	view.buffer = gamma_lut.get();
	view.range = sizeof(gamma_table);
	view.format = VK_FORMAT_R8_UINT;
	gamma_lut_view = device->create_buffer_view(view);
}

void VideoInterface::set_vi_register(VIRegister reg, uint32_t value)
{
	vi_registers[unsigned(reg)] = value;
}

void VideoInterface::set_rdram(const Vulkan::Buffer *rdram_)
{
	rdram = rdram_;
}

void VideoInterface::set_hidden_rdram(const Vulkan::Buffer *hidden_rdram_)
{
	hidden_rdram = hidden_rdram_;
}

void VideoInterface::set_shader_bank(const ShaderBank *bank)
{
	shader_bank = bank;
}

Vulkan::ImageHandle VideoInterface::scanout(VkImageLayout target_layout)
{
	Vulkan::ImageHandle scanout;

	int v_start = (vi_registers[unsigned(VIRegister::VStart)] >> 16) & 0x3ff;
	int h_start = (vi_registers[unsigned(VIRegister::HStart)] >> 16) & 0x3ff;

	int v_end = vi_registers[unsigned(VIRegister::VStart)] & 0x3ff;
	int h_end = vi_registers[unsigned(VIRegister::HStart)] & 0x3ff;

	int h_res = h_end - h_start;
	int v_res = (v_end - v_start) >> 1;

	int x_add = vi_registers[unsigned(VIRegister::XScale)] & 0xfff;
	int x_start = (vi_registers[unsigned(VIRegister::XScale)] >> 16) & 0xfff;
	int y_add = vi_registers[unsigned(VIRegister::YScale)] & 0xfff;
	int y_start = (vi_registers[unsigned(VIRegister::YScale)] >> 16) & 0xfff;

	int v_sync = vi_registers[unsigned(VIRegister::VSync)] & 0x3ff;
	unsigned v_current_line = vi_registers[unsigned(VIRegister::VCurrentLine)] & 1;

	int vi_width = vi_registers[unsigned(VIRegister::Width)] & 0xfff;
	int vi_offset = vi_registers[unsigned(VIRegister::Origin)] & 0xffffff;

	if (vi_offset == 0)
		return scanout;

	int status = vi_registers[unsigned(VIRegister::Control)];

	bool is_blank = (status & VI_CONTROL_TYPE_RGBA5551_BIT) == 0;
	if (is_blank && previous_frame_blank)
	{
		frame_count++;
		return scanout;
	}

	status |= VI_CONTROL_TYPE_RGBA5551_BIT;
	previous_frame_blank = is_blank;

	bool serrate = (status & VI_CONTROL_SERRATE_BIT) != 0;

	bool is_pal = unsigned(v_sync) > (VI_V_SYNC_NTSC + 25);
	h_start -= is_pal ? VI_H_OFFSET_PAL : VI_H_OFFSET_NTSC;

	int v_start_offset = is_pal ? VI_V_OFFSET_PAL : VI_V_OFFSET_NTSC;
	v_start = (v_start - v_start_offset) / 2;

	int v_active_lines = v_sync - v_start_offset;
	if (v_active_lines >= 0)
		v_active_lines >>= int(!serrate);

	bool divot = (status & VI_CONTROL_DIVOT_ENABLE_BIT) != 0;

	bool left_clamp = false;
	bool right_clamp = false;

	if (h_start < 0)
	{
		x_start -= x_add * h_start;
		h_res += h_start;
		h_start = 0;
		left_clamp = true;
	}

	if (h_start + h_res > 640)
	{
		h_res = 640 - h_start;
		right_clamp = true;
	}

	if (v_start < 0)
	{
		y_start -= y_add * v_start;
		v_start = 0;
	}

	if (h_res <= 0 || h_start >= 640)
	{
		frame_count++;
		return scanout;
	}

	bool degenerate = h_res <= 0 || v_res <= 0;

	// First we copy data out of VRAM into a texture which we will then perform our post-AA on.
	// We do this on the async queue so we don't have to stall async queue on graphics work to deal with WAR hazards.
	// After the copy, we can immediately begin rendering new frames while we do post in parallel.
	Vulkan::ImageHandle vram_image;
	if (!degenerate)
	{
		auto async_cmd = device->request_command_buffer(Vulkan::CommandBuffer::Type::AsyncCompute);
		int max_x = (x_start + h_res * x_add) >> 10;
		int max_y = (y_start + v_res * y_add) >> 10;

		// Need to sample a 2-pixel border to have room for AA filter and divot.
		int aa_width = max_x + 2 + 4 + int(divot) * 2;
		// 1 pixel border on top and bottom.
		int aa_height = max_y + 1 + 4;

		Vulkan::ImageCreateInfo rt_info = Vulkan::ImageCreateInfo::render_target(aa_width, aa_height, VK_FORMAT_R8G8B8A8_UINT);
		rt_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		rt_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		rt_info.misc = Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_GRAPHICS_BIT |
		               Vulkan::IMAGE_MISC_CONCURRENT_QUEUE_ASYNC_COMPUTE_BIT;
		vram_image = device->create_image(rt_info);
		vram_image->set_layout(Vulkan::Layout::General);

		async_cmd->image_barrier(*vram_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
		                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
		                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);

#ifdef PARALLEL_RDP_SHADER_DIR
		async_cmd->set_program("rdp://extract_vram.comp");
#else
		async_cmd->set_program(shader_bank->extract_vram);
#endif
		async_cmd->set_storage_texture(0, 0, vram_image->get_view());
		async_cmd->set_storage_buffer(0, 1, *rdram);
		async_cmd->set_storage_buffer(0, 2, *hidden_rdram);

		struct Push
		{
			uint32_t fb_offset;
			uint32_t fb_width;
			int32_t x_offset;
			int32_t y_offset;
			int32_t x_res;
			int32_t y_res;
		} push = {};

		if ((status & VI_CONTROL_TYPE_MASK) == VI_CONTROL_TYPE_RGBA8888_BIT)
			push.fb_offset = vi_offset >> 2;
		else
			push.fb_offset = vi_offset >> 1;

		push.fb_width = vi_width;
		push.x_offset = divot ? -3 : -2;
		push.y_offset = -2;
		push.x_res = aa_width;
		push.y_res = aa_height;

		async_cmd->set_specialization_constant_mask(3);
		async_cmd->set_specialization_constant(0, uint32_t(rdram->get_create_info().size));
		async_cmd->set_specialization_constant(1, status & (VI_CONTROL_TYPE_MASK | VI_CONTROL_AA_MODE_MASK));

		async_cmd->push_constants(&push, 0, sizeof(push));
		async_cmd->dispatch((aa_width + 15) / 16, (aa_height + 15) / 16, 1);
		// Just enforce an execution barrier here for rendering work in next frame.
		async_cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);

		Vulkan::Semaphore sem;
		device->submit(async_cmd, nullptr, 1, &sem);
		device->add_wait_semaphore(Vulkan::CommandBuffer::Type::Generic, std::move(sem),
		                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, true);
	}

	auto cmd = device->request_command_buffer();

	if (debug_channel)
		cmd->begin_debug_channel(this, "VI", 32 * 1024 * 1024);

	// In the first pass, we need to read from VRAM and apply the fetch filter.
	// This is either the AA filter if coverage < 7, or the dither reconstruction filter if coverage == 7 and enabled.
	// Following that, post-AA filter, we have the divot filter.
	// In this filter, we need to find the median value of three horizontal pixels, post AA if any of them have coverage < 7.
	// Finally, we lerp the result based on x_add and y_add, and then, apply gamma/dither on top as desired.

	Vulkan::ImageHandle aa_image;

	// If we risk sampling same Y coordinate for two scanlines we can trigger this case, so add workaround paths for it.
	bool fetch_bug = y_add < 1024;

	// AA -> divot could probably be done with compute and shared memory, but ideally this is done in fragment shaders in this implementation
	// so that we can run higher-priority compute shading workload async in the async queue.
	// We also get to take advantage of framebuffer compression FWIW.

	if (!degenerate)
	{
		// For the AA pass, we need to figure out how many pixels we might need to read.
		int max_x = (x_start + h_res * x_add) >> 10;
		int max_y = (y_start + v_res * y_add) >> 10;
		int aa_width = max_x + 3 + int(divot) * 2;
		int aa_height = max_y + 2;

		Vulkan::ImageCreateInfo rt_info = Vulkan::ImageCreateInfo::render_target(aa_width, aa_height, VK_FORMAT_R8G8B8A8_UINT);
		rt_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		rt_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		rt_info.layers = fetch_bug ? 2 : 1;
		rt_info.misc = Vulkan::IMAGE_MISC_FORCE_ARRAY_BIT;
		aa_image = device->create_image(rt_info);

		Vulkan::ImageViewCreateInfo view_info = {};
		view_info.image = aa_image.get();
		view_info.view_type = VK_IMAGE_VIEW_TYPE_2D;
		view_info.layers = 1;

		Vulkan::ImageViewHandle aa_primary, aa_secondary;
		view_info.base_layer = 0;
		aa_primary = device->create_image_view(view_info);

		if (fetch_bug)
		{
			view_info.base_layer = 1;
			aa_secondary = device->create_image_view(view_info);
		}

		Vulkan::RenderPassInfo rp;
		rp.color_attachments[0] = aa_primary.get();
		rp.clear_attachments = 0;

		if (fetch_bug)
		{
			rp.color_attachments[1] = aa_secondary.get();
			rp.num_color_attachments = 2;
			rp.store_attachments = 3;
		}
		else
		{
			rp.num_color_attachments = 1;
			rp.store_attachments = 1;
		}

		cmd->image_barrier(*aa_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

		cmd->begin_render_pass(rp);
		cmd->set_opaque_state();

#ifdef PARALLEL_RDP_SHADER_DIR
		cmd->set_program("rdp://fullscreen.vert", "rdp://vi_fetch.frag",
		                 {
				                 { "DEBUG_ENABLE", debug_channel ? 1 : 0 },
				                 { "FETCH_BUG", fetch_bug ? 1 : 0 },
		                 });
#else
		cmd->set_program(device->request_program(shader_bank->fullscreen, shader_bank->vi_fetch[int(fetch_bug)]));
#endif

		struct Push
		{
			int32_t x_offset;
			int32_t y_offset;
		} push = {};

		push.x_offset = 2;
		push.y_offset = 2;

		cmd->push_constants(&push, 0, sizeof(push));

		cmd->set_specialization_constant_mask(3);
		cmd->set_specialization_constant(0, uint32_t(rdram->get_create_info().size));
		cmd->set_specialization_constant(1,
		                                 status & (VI_CONTROL_AA_MODE_MASK | VI_CONTROL_DITHER_FILTER_ENABLE_BIT));

		cmd->set_texture(0, 0, vram_image->get_view());
		cmd->draw(3);
		cmd->end_render_pass();

		cmd->image_barrier(*aa_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}

	// Divot pass
	Vulkan::ImageHandle divot_image;

	if (divot && !degenerate)
	{
		// For the divot pass, we need to figure out how many pixels we might need to read.
		int max_x = (x_start + h_res * x_add) >> 10;
		int max_y = (y_start + v_res * y_add) >> 10;
		int divot_width = max_x + 2;
		int divot_height = max_y + 2;

		Vulkan::ImageCreateInfo rt_info = Vulkan::ImageCreateInfo::render_target(divot_width, divot_height, VK_FORMAT_R8G8B8A8_UINT);
		rt_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		rt_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		rt_info.layers = fetch_bug ? 2 : 1;
		rt_info.misc = Vulkan::IMAGE_MISC_FORCE_ARRAY_BIT;
		divot_image = device->create_image(rt_info);

		Vulkan::ImageViewCreateInfo view_info = {};
		view_info.image = divot_image.get();
		view_info.view_type = VK_IMAGE_VIEW_TYPE_2D;
		view_info.layers = 1;

		Vulkan::ImageViewHandle divot_primary, divot_secondary;
		view_info.base_layer = 0;
		divot_primary = device->create_image_view(view_info);

		if (fetch_bug)
		{
			view_info.base_layer = 1;
			divot_secondary = device->create_image_view(view_info);
		}

		Vulkan::RenderPassInfo rp;
		rp.color_attachments[0] = divot_primary.get();
		rp.clear_attachments = 0;

		if (fetch_bug)
		{
			rp.color_attachments[1] = divot_secondary.get();
			rp.num_color_attachments = 2;
			rp.store_attachments = 3;
		}
		else
		{
			rp.num_color_attachments = 1;
			rp.store_attachments = 1;
		}

		cmd->image_barrier(*divot_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

		cmd->begin_render_pass(rp);
		cmd->set_opaque_state();

#ifdef PARALLEL_RDP_SHADER_DIR
		cmd->set_program("rdp://fullscreen.vert", "rdp://vi_divot.frag", {
				{ "DEBUG_ENABLE", debug_channel ? 1 : 0 },
				{ "FETCH_BUG", fetch_bug ? 1 : 0 },
		});
#else
		cmd->set_program(device->request_program(shader_bank->fullscreen, shader_bank->vi_divot[int(fetch_bug)]));
#endif

		cmd->set_texture(0, 0, aa_image->get_view());
		cmd->draw(3);
		cmd->end_render_pass();

		cmd->image_barrier(*divot_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	else
		divot_image = std::move(aa_image);

	// Scale pass
	Vulkan::ImageHandle scale_image;
	{
		Vulkan::ImageCreateInfo rt_info = Vulkan::ImageCreateInfo::render_target(
				640, (is_pal ? VI_V_RES_PAL: VI_V_RES_NTSC) >> 1, VK_FORMAT_R8G8B8A8_UNORM);
		rt_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		rt_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		rt_info.misc = Vulkan::IMAGE_MISC_MUTABLE_SRGB_BIT;
		scale_image = device->create_image(rt_info);

		Vulkan::RenderPassInfo rp;
		rp.color_attachments[0] = &scale_image->get_view();
		rp.num_color_attachments = 1;
		rp.clear_attachments = 1;
		rp.store_attachments = 1;

		cmd->image_barrier(*scale_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

		cmd->begin_render_pass(rp);

		cmd->set_specialization_constant_mask((1 << 1) | (1 << 2));
		cmd->set_specialization_constant(1,
		                                 status & (VI_CONTROL_GAMMA_ENABLE_BIT |
		                                           VI_CONTROL_GAMMA_DITHER_ENABLE_BIT |
		                                           VI_CONTROL_AA_MODE_MASK));
		cmd->set_specialization_constant(2, uint32_t(fetch_bug));

		struct Push
		{
			int32_t x_offset, y_offset;
			int32_t h_offset, v_offset;
			uint32_t x_add;
			uint32_t y_add;
			uint32_t frame_count;
		} push = {};
		push.x_offset = x_start;
		push.y_offset = y_start;
		push.h_offset = h_start;
		push.v_offset = v_start;
		push.x_add = x_add;
		push.y_add = y_add;
		push.frame_count = frame_count;

		cmd->set_opaque_state();
#ifdef PARALLEL_RDP_SHADER_DIR
		cmd->set_program("rdp://fullscreen.vert", "rdp://vi_scale.frag", {
				{ "DEBUG_ENABLE", debug_channel ? 1 : 0 },
		});
#else
		cmd->set_program(device->request_program(shader_bank->fullscreen, shader_bank->vi_scale));
#endif
		cmd->set_buffer_view(1, 0, *gamma_lut_view);

		if (!left_clamp)
		{
			h_start += 8;
			h_res -= 8;
		}

		if (!right_clamp)
			h_res -= 7;

		cmd->push_constants(&push, 0, sizeof(push));

		if (!degenerate && h_res > 0 && v_res > 0)
		{
			cmd->set_texture(0, 0, divot_image->get_view());
			cmd->set_scissor({{ h_start, v_start }, { uint32_t(h_res), uint32_t(v_res) }});
			cmd->draw(3);
		}

		cmd->end_render_pass();
	}

	if (target_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		cmd->image_barrier(*scale_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	else if (target_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
	{
		cmd->image_barrier(*scale_image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	}

	device->submit(cmd);
	scanout = std::move(scale_image);
	frame_count++;
	return scanout;
}

}
