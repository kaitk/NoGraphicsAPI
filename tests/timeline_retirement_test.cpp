#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <stdio.h>
#include <string.h>

namespace
{
bool check_retirement(gpu::Device* device, bool shared_timeline, bool poll) noexcept
{
    bool valid = true;
    for (uint32 iteration = 0; iteration < 64; ++iteration)
    {
        gpu::TimelineSemaphore* timelines[3]{};
        gpu::TimelinePoint points[3]{};
        gpu::CommandPool* pools[3]{};
        gpu::CommandBuffer* commands[3]{};
        for (uint32 queue = 0; queue < 3; ++queue)
        {
            timelines[queue] = shared_timeline && queue ? timelines[0] : gpu::create_timeline_semaphore(device, 7);
            points[queue] = {.semaphore = timelines[queue], .value = shared_timeline ? 8u + queue : 8u};
            pools[queue] = gpu::create_command_pool(device, queue);
            commands[queue] = gpu::begin_commands(pools[queue]);
            valid = gpu::timeline_completed_value(timelines[queue]) == 7 && valid;
        }
        const gpu::GpuHeap upload = gpu::create_gpu_heap(device, 256);
        const gpu::GpuHeap first = gpu::create_gpu_heap(device, 256, gpu::MemoryType::gpu_only);
        const gpu::GpuHeap second = gpu::create_gpu_heap(device, 256, gpu::MemoryType::gpu_only);
        const gpu::GpuHeap readback = gpu::create_gpu_heap(device, 256, gpu::MemoryType::readback);
        uint32 expected[64]{};
        for (uint32 index = 0; index < 64; ++index) expected[index] = (iteration << 16) | index;
        memcpy(upload.range.cpu, expected, sizeof(expected));
        gpu::copy_memory(commands[0], gpu::gpu_range(upload), gpu::gpu_range(first));
        gpu::copy_memory(commands[1], gpu::gpu_range(first), gpu::gpu_range(second));
        gpu::copy_memory(commands[2], gpu::gpu_range(second), gpu::gpu_range(readback));
        gpu::barrier(commands[2], gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
        for (gpu::CommandBuffer* command : commands) gpu::end_commands(command);
        gpu::CommandBuffer* successor = gpu::begin_commands(pools[2]);
        gpu::end_commands(successor);
        const gpu::TimelinePoint completion{.semaphore = timelines[2], .value = points[2].value + 1};
        // Submit consumers first to exercise waits whose producers have not been submitted yet.
        gpu::submit(device, {.commands = {commands[2]}, .waits = {points[1]}, .completion = points[2]}, 2);
        gpu::submit(device, {.commands = {successor}, .completion = completion}, 2);
        gpu::submit(device, {.commands = {commands[1]}, .waits = {points[0]}, .completion = points[1]}, 1);
        gpu::submit(device, {.commands = {commands[0]}, .completion = points[0]}, 0);
        if (poll)
            while (gpu::timeline_completed_value(completion.semaphore) < completion.value) {}
        else
            gpu::wait_timeline(completion);
        valid = memcmp(readback.range.cpu, expected, sizeof(expected)) == 0 && valid;
        for (uint32 queue = 0; queue < 3; ++queue)
        {
            valid = gpu::timeline_completed_value(points[queue].semaphore) >= points[queue].value && valid;
            gpu::destroy_command_pool(pools[queue]);
        }
        gpu::destroy_gpu_heap(upload);
        gpu::destroy_gpu_heap(first);
        gpu::destroy_gpu_heap(second);
        gpu::destroy_gpu_heap(readback);
        for (uint32 queue = 0; queue < (shared_timeline ? 1u : 3u); ++queue) gpu::destroy_timeline_semaphore(timelines[queue]);
    }
    printf("Transitive retirement, %s timeline, %s: %s\n", shared_timeline ? "shared" : "separate", poll ? "poll" : "wait", valid ? "passed" : "failed");
    return valid;
}
}

int main()
{
    const gpu::DeviceInit initialized = gpu::create_device({.desired_queue_count = 3, .timestamp_query_count = 0});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    if (gpu::get_device_caps(initialized.device).queue_count < 3)
    {
        gpu::destroy_device(initialized.device);
        return 77;
    }
    bool valid = true;
    for (uint32 mode = 0; mode < 4; ++mode)
        valid = check_retirement(initialized.device, (mode & 1) != 0, (mode & 2) != 0) && valid;
    gpu::wait_idle(initialized.device);
    gpu::destroy_device(initialized.device);
    return valid ? 0 : 1;
}
