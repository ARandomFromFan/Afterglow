// (afterglow) Every Vulkan command that does GPU work, or orders it, counts in work_epoch. full_barrier (vk_texture.cpp) leaves out a
// barrier recorded right after another one on the same command buffer with nothing in between: the second orders nothing the first
// did not. Include this header AFTER the file's other includes: it turns the calls below into a count and the call (a header
// included after it that names one of them with a '(' after it would be changed too).
#pragma once

#include <atomic>

#include "vk_loader.h"

namespace agvk {
extern std::atomic<uint64_t> work_epoch;   // (vk_texture.cpp)
inline void note_work() {
    // (an add, not a load and a store: a thread preempted between the two would store a stale count over another thread's, which could
    // make the count equal the one a barrier was recorded at with work recorded since, and the next barrier is left out for it)
    work_epoch.fetch_add(1, std::memory_order_relaxed);
}
} // namespace agvk

#define vkCmdPipelineBarrier2(...) (::agvk::note_work(), ::vkx::vkCmdPipelineBarrier2(__VA_ARGS__))
#define vkCmdBeginRendering(...) (::agvk::note_work(), ::vkx::vkCmdBeginRendering(__VA_ARGS__))
#define vkCmdEndRendering(...) (::agvk::note_work(), ::vkx::vkCmdEndRendering(__VA_ARGS__))
#define vkCmdDraw(...) (::agvk::note_work(), ::vkx::vkCmdDraw(__VA_ARGS__))
#define vkCmdDrawIndexed(...) (::agvk::note_work(), ::vkx::vkCmdDrawIndexed(__VA_ARGS__))
#define vkCmdDrawIndirect(...) (::agvk::note_work(), ::vkx::vkCmdDrawIndirect(__VA_ARGS__))
#define vkCmdDrawIndexedIndirect(...) (::agvk::note_work(), ::vkx::vkCmdDrawIndexedIndirect(__VA_ARGS__))
#define vkCmdDrawIndirectCount(...) (::agvk::note_work(), ::vkx::vkCmdDrawIndirectCount(__VA_ARGS__))
#define vkCmdDrawIndexedIndirectCount(...) (::agvk::note_work(), ::vkx::vkCmdDrawIndexedIndirectCount(__VA_ARGS__))
#define vkCmdDrawMeshTasksEXT(...) (::agvk::note_work(), ::vkx::vkCmdDrawMeshTasksEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectEXT(...) (::agvk::note_work(), ::vkx::vkCmdDrawMeshTasksIndirectEXT(__VA_ARGS__))
#define vkCmdDrawMeshTasksIndirectCountEXT(...) (::agvk::note_work(), ::vkx::vkCmdDrawMeshTasksIndirectCountEXT(__VA_ARGS__))
#define vkCmdDispatch(...) (::agvk::note_work(), ::vkx::vkCmdDispatch(__VA_ARGS__))
#define vkCmdDispatchIndirect(...) (::agvk::note_work(), ::vkx::vkCmdDispatchIndirect(__VA_ARGS__))
#define vkCmdCopyBuffer(...) (::agvk::note_work(), ::vkx::vkCmdCopyBuffer(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (::agvk::note_work(), ::vkx::vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (::agvk::note_work(), ::vkx::vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdCopyImage(...) (::agvk::note_work(), ::vkx::vkCmdCopyImage(__VA_ARGS__))
#define vkCmdBlitImage(...) (::agvk::note_work(), ::vkx::vkCmdBlitImage(__VA_ARGS__))
#define vkCmdResolveImage(...) (::agvk::note_work(), ::vkx::vkCmdResolveImage(__VA_ARGS__))
#define vkCmdClearColorImage(...) (::agvk::note_work(), ::vkx::vkCmdClearColorImage(__VA_ARGS__))
#define vkCmdClearDepthStencilImage(...) (::agvk::note_work(), ::vkx::vkCmdClearDepthStencilImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (::agvk::note_work(), ::vkx::vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdFillBuffer(...) (::agvk::note_work(), ::vkx::vkCmdFillBuffer(__VA_ARGS__))
#define vkCmdUpdateBuffer(...) (::agvk::note_work(), ::vkx::vkCmdUpdateBuffer(__VA_ARGS__))
#define vkCmdBeginQuery(...) (::agvk::note_work(), ::vkx::vkCmdBeginQuery(__VA_ARGS__))
#define vkCmdEndQuery(...) (::agvk::note_work(), ::vkx::vkCmdEndQuery(__VA_ARGS__))
