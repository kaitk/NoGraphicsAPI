#include "shader_code.h"
#include <stdio.h>
#include <string.h>

int main(int argc, const char* const* argv)
{
    const bool direct_only = argc == 2 && strcmp(argv[1], "--direct-only") == 0;
    const gpu::DeviceInit initialized = gpu::create_device();
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    gpu::Device* device = initialized.device;
    const bool test_indirect = !direct_only && gpu::get_device_caps(device).indirect_mesh_draw;
    if (!direct_only && !test_indirect) printf("Native indirect mesh draws unavailable; testing direct mesh draws.\n");
    const gpu::Span<byte> code = load_test_shader(NOGRAPHICSAPI_METAL_MESH_SHADER);
    if (!code.data) { gpu::destroy_device(device); return 1; }
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    gpu::PSO* pipeline = gpu::create_mesh_pso(device, {
        .mesh = {.code = {code.data, code.size}, .entry_point = "meshMain",
                 .threadgroup_size = {.x = 32, .y = 1, .z = 1}},
        .fragment = {.code = {code.data, code.size}, .entry_point = "fragmentMain"},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
        .rasterization = {.cull = gpu::CullMode::counter_clockwise},
    });
    free(code.data);
    if (!pipeline)
    {
        gpu::destroy_timeline_semaphore(timeline);
        gpu::destroy_device(device);
        return 1;
    }
    const gpu::TextureDesc desc{
        .extent = {.x = 8, .y = 8, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, gpu::get_texture_size_align(device, desc).size);
    gpu::Texture* color = gpu::create_texture(device, desc, heap, 0);
    gpu::initialize_texture(commands, color);
    gpu::RenderView* view = gpu::create_render_view(color);
    const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 64);
    const gpu::GpuHeap arguments = gpu::create_gpu_heap(device, 32, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 512, gpu::MemoryType::readback);
    const uint32 indirect[8]{1, 1, 1, 0, 1, 1, 1, 0};
    memcpy(upload.range.cpu, indirect, sizeof(indirect));
    gpu::copy_memory(commands, {.gpu = upload.range.gpu, .size = 32}, gpu::gpu_range(arguments));
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::indirect, gpu::Access::indirect_read);
    for (uint32 mode = 0; mode < (test_indirect ? 2u : 1u); ++mode)
    {
        gpu::begin_render_pass(commands, {.colors = {{.render_view = view, .load = gpu::LoadOp::clear}}});
        gpu::bind_pso(commands, pipeline);
        gpu::ClearColor root{.x = mode == 0 ? 1.0f : 0.0f, .y = mode == 1 ? 1.0f : 0.0f, .z = 0.0f, .w = 1.0f};
        memcpy(upload.range.cpu + 32 + mode * sizeof(root), &root, sizeof(root));
        if (mode == 0) gpu::draw_meshlets(commands, upload.range.gpu + 32 + mode * sizeof(root), {.x = 1, .y = 1, .z = 1});
        else gpu::draw_meshlets_indirect(commands, upload.range.gpu + 32 + mode * sizeof(root), gpu::gpu_range(arguments), 2, 16);
        memset(&root, 0, sizeof(root));
        gpu::end_render_pass(commands);
        gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, color, {.gpu = readback.range.gpu + mode * 256, .size = 256});
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
    }
    gpu::end_commands(commands);
    gpu::submit(device, {.commands = {commands}, .completion = {.semaphore = timeline, .value = 1}});
    gpu::wait_timeline({.semaphore = timeline, .value = 1});
    bool valid = true;
    for (uint32 pixel = 0; pixel < (test_indirect ? 128u : 64u); ++pixel)
    {
        const byte* rgba = readback.range.cpu + pixel * 4;
        const uint32 diagonal = (pixel & 7u) + ((pixel >> 3u) & 7u);
        if (diagonal == 7) continue;
        const bool inside = diagonal < 7;
        valid &= rgba[0] == (inside && pixel < 64 ? 255 : 0) && rgba[1] == (inside && pixel >= 64 ? 255 : 0) &&
                 rgba[2] == 0 && rgba[3] == 255;
    }
    if (!valid) fprintf(stderr, "Metal mesh readback failed: direct %u %u %u %u; indirect %u %u %u %u\n",
                        readback.range.cpu[0], readback.range.cpu[1], readback.range.cpu[2], readback.range.cpu[3],
                        test_indirect ? readback.range.cpu[256] : 0, test_indirect ? readback.range.cpu[257] : 0,
                        test_indirect ? readback.range.cpu[258] : 0, test_indirect ? readback.range.cpu[259] : 0);
    gpu::wait_idle(device);
    gpu::destroy_command_pool(pool);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_gpu_heap(arguments);
    gpu::destroy_gpu_heap(upload);
    gpu::destroy_render_view(view);
    gpu::destroy_texture(color);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_pso(pipeline);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_device(device);
    return valid ? 0 : 1;
}
