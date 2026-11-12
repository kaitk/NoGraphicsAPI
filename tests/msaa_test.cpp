#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include "shader_code.h"

#include <stdio.h>
#include <string.h>

// 4x color and depth attachments, transient, resolved into a single-sample texture:
// a clear alone resolves to the clear color, and a quad whose edge halves pixel 2 covers two of its four standard sample positions.
namespace
{

constexpr uint32 width = 8;
constexpr uint32 height = 2;
constexpr uint64 pixel_bytes = width * height * 4;

uint64 align_up(uint64 value, uint64 align) noexcept
{
    return (value + align - 1) / align * align;
}

bool near(uint8 value, uint32 expected) noexcept
{
    return value + 2u >= expected && value <= expected + 2u;
}

} // namespace

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device();
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    gpu::Device* device = initialized.device;

    gpu::Span<byte> vertex = load_test_shader(NOGRAPHICSAPI_MSAA_VERTEX_SPV);
    gpu::Span<byte> fragment = load_test_shader(NOGRAPHICSAPI_MSAA_FRAGMENT_SPV);
    if (!vertex.data || !fragment.data) { free(vertex.data); free(fragment.data); return 1; }
    gpu::PSO* pso = gpu::create_graphics_pso(device, {
        .vertex = {.code = {vertex.data, vertex.size}, .entry_point = "vertexMain"},
        .fragment = {.code = {fragment.data, fragment.size}, .entry_point = "fragmentMain"},
        .color_targets = {{.format = gpu::Format::rgba8_unorm}},
        .depth_format = gpu::Format::d32_float,
        .sample_count = 4,
    });
    free(vertex.data);
    free(fragment.data);

    const gpu::TextureDesc resolve_desc{
        .extent = {.x = width, .y = height, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    const gpu::TextureDesc color_desc{
        .extent = {.x = width, .y = height, .z = 1},
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transient,
        .sample_count = 4,
    };
    const gpu::TextureDesc depth_desc{
        .extent = {.x = width, .y = height, .z = 1},
        .format = gpu::Format::d32_float,
        .usage = gpu::TextureUsage::depth_stencil_attachment | gpu::TextureUsage::transient,
        .sample_count = 4,
    };
    const uint64 align = gpu::get_device_caps(device).texture_heap_alignment;
    const uint64 color_offset = align_up(gpu::get_texture_size_align(device, resolve_desc).size, align);
    const uint64 depth_offset = color_offset + align_up(gpu::get_texture_size_align(device, color_desc).size, align);
    const uint64 heap_size = depth_offset + align_up(gpu::get_texture_size_align(device, depth_desc).size, align);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, heap_size);
    gpu::Texture* resolve = gpu::create_texture(device, resolve_desc, heap, 0);
    gpu::Texture* color = gpu::create_texture(device, color_desc, heap, color_offset);
    gpu::Texture* depth = gpu::create_texture(device, depth_desc, heap, depth_offset);
    gpu::RenderView* resolve_view = resolve ? gpu::create_render_view(resolve) : nullptr;
    gpu::RenderView* color_view = color ? gpu::create_render_view(color) : nullptr;
    gpu::RenderView* depth_view = depth ? gpu::create_render_view(depth) : nullptr;
    const gpu::GpuHeap readback = gpu::create_gpu_heap(device, pixel_bytes, gpu::MemoryType::readback);
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::TimelinePoint completion{.semaphore = gpu::create_timeline_semaphore(device)};
    bool valid = pso && heap.owner && resolve_view && color_view && depth_view && readback.range.cpu && pool && completion.semaphore;

    for (uint32 pass = 0; valid && pass < 2; ++pass)
    {
        memset(readback.range.cpu, 0xff, pixel_bytes);
        gpu::reset_command_pool(pool);
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        if (pass == 0)
        {
            gpu::initialize_texture(commands, resolve);
            gpu::initialize_texture(commands, color);
            gpu::initialize_texture(commands, depth);
        }
        else gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_read, gpu::Stage::color_output, gpu::Access::color_write);
        const gpu::ColorAttachment attachment{
            .render_view = color_view,
            .load = gpu::LoadOp::clear,
            .store = gpu::StoreOp::discard,
            .clear = pass == 0 ? gpu::ClearColor{.x = 0.25f, .y = 0.5f, .z = 0.75f} : gpu::ClearColor{},
            .resolve_view = resolve_view,
        };
        gpu::begin_render_pass(commands, {
            .colors = {&attachment, 1},
            .depth = {.render_view = depth_view, .load = gpu::LoadOp::clear, .store = gpu::StoreOp::discard},
        });
        if (pass == 1)
        {
            gpu::bind_pso(commands, pso);
            gpu::set_depth_stencil(commands, {.depth_test = true, .depth_write = true, .depth_compare = gpu::CompareOp::less});
            gpu::draw(commands, nullptr, 6);
        }
        gpu::end_render_pass(commands);
        gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, resolve, gpu::gpu_range(readback));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        gpu::end_commands(commands);
        ++completion.value;
        gpu::submit(device, {.commands = {commands}, .completion = completion});
        gpu::wait_timeline(completion);

        uint32 errors = 0;
        for (uint32 y = 0; y < height; ++y)
            for (uint32 x = 0; x < width; ++x)
            {
                const uint8* pixel = readback.range.cpu + (y * width + x) * 4;
                const uint32 white = x < 2 ? 255 : x == 2 ? 128 : 0;
                const bool ok = pass == 0 ? near(pixel[0], 64) && near(pixel[1], 128) && near(pixel[2], 191) && pixel[3] == 255
                                          : near(pixel[0], white) && near(pixel[1], white) && near(pixel[2], white) && pixel[3] == 255;
                if (!ok && errors++ < 4)
                    fprintf(stderr, "MSAA %s: pixel %u,%u is %u %u %u %u\n", pass == 0 ? "clear" : "edge", x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
            }
        valid = errors == 0;
    }

    gpu::wait_idle(device);
    gpu::destroy_command_pool(pool);
    gpu::destroy_render_view(depth_view);
    gpu::destroy_render_view(color_view);
    gpu::destroy_render_view(resolve_view);
    gpu::destroy_texture(depth);
    gpu::destroy_texture(color);
    gpu::destroy_texture(resolve);
    gpu::destroy_texture_heap(heap);
    gpu::destroy_gpu_heap(readback);
    gpu::destroy_timeline_semaphore(completion.semaphore);
    gpu::destroy_pso(pso);
    gpu::destroy_device(device);
    printf("4x MSAA transient attachments, clear and edge resolve: %s\n", valid ? "passed" : "failed");
    return valid ? 0 : 1;
}
