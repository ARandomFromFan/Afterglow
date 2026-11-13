// Offline test of full shader pipeline: LowerToMaxwell -> SPIR-V emission -> spirv-val -> VkPipeline (RADV).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

namespace {

constexpr u32 HeaderSize = sizeof(Shader::ProgramHeader);

static const uint32_t dummy_vs_spv[166] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000014u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00030003u, 0x00000002u,
    0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060005u, 0x0000000bu, 0x505f6c67u,
    0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u, 0x7469736fu,
    0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u, 0x00000000u,
    0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u, 0x0065636eu, 0x00070006u,
    0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u, 0x0000000du,
    0x00000000u, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu,
    0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu,
    0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u,
    0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u,
    0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u,
    0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u,
    0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu,
    0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu,
    0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x0004002bu, 0x00000006u,
    0x00000010u, 0x00000000u, 0x0007002cu, 0x00000007u, 0x00000011u, 0x00000010u, 0x00000010u, 0x00000010u,
    0x00000010u, 0x00040020u, 0x00000012u, 0x00000003u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u,
    0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x00050041u, 0x00000012u, 0x00000013u, 0x0000000du,
    0x0000000fu, 0x0003003eu, 0x00000013u, 0x00000011u, 0x000100fdu, 0x00010038u,
};

static const uint32_t dummy_ms_spv[96] = {
    0x07230203u, 0x00010600u, 0x0008000bu, 0x0000000bu, 0x00000000u, 0x00020011u, 0x000014a3u, 0x0006000au,
    0x5f565053u, 0x5f545845u, 0x6873656du, 0x6168735fu, 0x00726564u, 0x0006000bu, 0x00000001u, 0x4c534c47u,
    0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0005000fu, 0x000014f5u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x0006014bu, 0x00000004u, 0x00000026u, 0x00000007u, 0x00000007u,
    0x00000007u, 0x00040010u, 0x00000004u, 0x0000001au, 0x00000003u, 0x00040010u, 0x00000004u, 0x00001496u,
    0x00000001u, 0x00030010u, 0x00000004u, 0x000014b2u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00060004u,
    0x455f4c47u, 0x6d5f5458u, 0x5f687365u, 0x64616873u, 0x00007265u, 0x00040005u, 0x00000004u, 0x6e69616du,
    0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u, 0x00000006u,
    0x00000020u, 0x00000000u, 0x0004002bu, 0x00000006u, 0x00000007u, 0x00000001u, 0x0004002bu, 0x00000006u,
    0x00000008u, 0x00000000u, 0x00040017u, 0x00000009u, 0x00000006u, 0x00000003u, 0x0006002cu, 0x00000009u,
    0x0000000au, 0x00000007u, 0x00000007u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
    0x00000003u, 0x000200f8u, 0x00000005u, 0x000314afu, 0x00000008u, 0x00000008u, 0x000100fdu, 0x00010038u,
};

static const uint32_t dummy_tcs_spv[171] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000001au, 0x00000000u, 0x00020011u, 0x00000003u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000001u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000bu, 0x00000018u, 0x00040010u,
    0x00000004u, 0x0000001au, 0x00000003u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u,
    0x6e69616du, 0x00000000u, 0x00070005u, 0x0000000bu, 0x545f6c67u, 0x4c737365u, 0x6c657665u, 0x6574754fu,
    0x00000072u, 0x00070005u, 0x00000018u, 0x545f6c67u, 0x4c737365u, 0x6c657665u, 0x656e6e49u, 0x00000072u,
    0x00040047u, 0x0000000bu, 0x0000000bu, 0x0000000bu, 0x00030047u, 0x0000000bu, 0x0000000fu, 0x00040047u,
    0x00000018u, 0x0000000bu, 0x0000000cu, 0x00030047u, 0x00000018u, 0x0000000fu, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040015u, 0x00000007u,
    0x00000020u, 0x00000000u, 0x0004002bu, 0x00000007u, 0x00000008u, 0x00000004u, 0x0004001cu, 0x00000009u,
    0x00000006u, 0x00000008u, 0x00040020u, 0x0000000au, 0x00000003u, 0x00000009u, 0x0004003bu, 0x0000000au,
    0x0000000bu, 0x00000003u, 0x00040015u, 0x0000000cu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000cu,
    0x0000000du, 0x00000000u, 0x0004002bu, 0x00000006u, 0x0000000eu, 0x3f800000u, 0x00040020u, 0x0000000fu,
    0x00000003u, 0x00000006u, 0x0004002bu, 0x0000000cu, 0x00000011u, 0x00000001u, 0x0004002bu, 0x0000000cu,
    0x00000013u, 0x00000002u, 0x0004002bu, 0x00000007u, 0x00000015u, 0x00000002u, 0x0004001cu, 0x00000016u,
    0x00000006u, 0x00000015u, 0x00040020u, 0x00000017u, 0x00000003u, 0x00000016u, 0x0004003bu, 0x00000017u,
    0x00000018u, 0x00000003u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x00050041u, 0x0000000fu, 0x00000010u, 0x0000000bu, 0x0000000du, 0x0003003eu, 0x00000010u,
    0x0000000eu, 0x00050041u, 0x0000000fu, 0x00000012u, 0x0000000bu, 0x00000011u, 0x0003003eu, 0x00000012u,
    0x0000000eu, 0x00050041u, 0x0000000fu, 0x00000014u, 0x0000000bu, 0x00000013u, 0x0003003eu, 0x00000014u,
    0x0000000eu, 0x00050041u, 0x0000000fu, 0x00000019u, 0x00000018u, 0x0000000du, 0x0003003eu, 0x00000019u,
    0x0000000eu, 0x000100fdu, 0x00010038u,
};

static const uint32_t dummy_tes_spv[175] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000014u, 0x00000000u, 0x00020011u, 0x00000003u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000002u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x00030010u, 0x00000004u,
    0x00000016u, 0x00030010u, 0x00000004u, 0x00000001u, 0x00030010u, 0x00000004u, 0x00000005u, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060005u, 0x0000000bu,
    0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u,
    0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
    0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u, 0x0065636eu,
    0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u,
    0x0000000du, 0x00000000u, 0x00030047u, 0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u,
    0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u, 0x0000000bu, 0x00000001u, 0x00050048u,
    0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u, 0x0000000bu,
    0x00000004u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u,
    0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u,
    0x00000009u, 0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u,
    0x0000000cu, 0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u,
    0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x0004002bu,
    0x00000006u, 0x00000010u, 0x00000000u, 0x0007002cu, 0x00000007u, 0x00000011u, 0x00000010u, 0x00000010u,
    0x00000010u, 0x00000010u, 0x00040020u, 0x00000012u, 0x00000003u, 0x00000007u, 0x00050036u, 0x00000002u,
    0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x00050041u, 0x00000012u, 0x00000013u,
    0x0000000du, 0x0000000fu, 0x0003003eu, 0x00000013u, 0x00000011u, 0x000100fdu, 0x00010038u,
};

class FullEnv final : public Shader::Environment {
public:
    FullEnv(const std::vector<u8>& bytes, u16 st, const u8* ctl) {
        using Shader::Ampere::Instruction;
        const bool graphics = (st != 5);
        const u32 native_header = graphics ? 128 : 0;
        if (graphics && bytes.size() >= sizeof(sph)) {
            std::memcpy(&sph, bytes.data(), sizeof sph);
        }
        switch (st) {
        case 0: stage = Shader::Stage::VertexB; break;
        case 1: stage = Shader::Stage::Fragment; break;
        case 2: stage = Shader::Stage::Geometry; break;
        case 3: stage = Shader::Stage::TessellationControl; break;
        case 4: stage = Shader::Stage::TessellationEval; break;
        case 5: stage = Shader::Stage::Compute; break;
        case 6: stage = Shader::Stage::Task; break;
        case 7: stage = Shader::Stage::Mesh; break;
        default: stage = Shader::Stage::Fragment; break;
        }

        if (st == 6 || st == 7) {
            u32 w58 = 0, w60 = 0;
            if (ctl) {
                std::memcpy(&w58, ctl + 0x58, 4);
                std::memcpy(&w60, ctl + 0x60, 4);
            }
            u32 mesh_threads = w58 & 0xFFFF;
            u32 payload_bytes = w58 >> 16;
            u32 max_primitives = w60 & 0xFFFF;
            u32 max_vertices = w60 >> 16;
            u8 topology = ctl ? ctl[0x5f] : 3;
            const u32 topo = topology == 3 ? 4 : topology == 1 ? 0 : topology;
            const u32 lines = (payload_bytes + 127) / 128;
            const u32 most = std::max(max_vertices, max_primitives);
            mesh_registers = {
                (topo & 0xF) | (std::min(most, 0x100u) << 4) | (std::min(most, 0x100u) << 13),
                std::min(mesh_threads, 0x3Fu) << 20,
                (st == 7 ? 0 : std::min(mesh_threads, 0x3Fu)) | (std::min(lines, 0x1FFu) << 12) | (std::min(lines, 0x1FFu) << 22)
            };
        }

        std::vector<Instruction> native;
        Shader::Ampere::ProgramCaptureState capture;
        for (u32 i = 0; native_header + (i + 1) * sizeof(Instruction) <= bytes.size(); i++) {
            auto& insn = native.emplace_back();
            std::memcpy(&insn, bytes.data() + native_header + i * sizeof(Instruction), sizeof insn);
            const auto res = capture.Observe(insn, i);
            if (res == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget)
                throw Shader::RuntimeError("branch outside capture bounds at +{:#x}", i * 16);
            if (res == Shader::Ampere::ProgramCaptureState::Result::Complete) break;
        }
        captured = native.size();
        // same stage options as vk_shader.cpp; without them geometry and tess programs always fail
        Shader::Ampere::LoweringOptions options{};
        options.geometry = (st == 2);
        if (st == 3) options.tessellation_stage = Shader::Ampere::TessellationStage::Control;
        else if (st == 4) options.tessellation_stage = Shader::Ampere::TessellationStage::Evaluation;
        auto lowered = Shader::Ampere::LowerToMaxwell(native, options);
        if (!lowered) {
            char buf[256];
            u32 err_off = lowered.error_offset;
            u32 at = err_off / 16;
            u32 op = at < native.size() ? native[at].Bits(0, 12) : 0xFFFFFFFFu;
            std::snprintf(buf, sizeof buf, "LowerToMaxwell failed (+%#x, op %#x): %s", err_off, op, std::string(lowered.error).c_str());
            throw Shader::RuntimeError("{}", buf);
        }
        if (lowered.required_texture_buffer != ~0U) texture_bound = lowered.required_texture_buffer;
        code.resize(sizeof(sph) / sizeof(u64));
        std::memcpy(code.data(), &sph, sizeof sph);
        for (const u64 instruction : lowered.instructions) {
            if ((code.size() * sizeof(u64)) % 32 == 0) code.push_back(0);
            code.push_back(instruction);
        }
        code.push_back(0);
        Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
        start_address = (stage == Shader::Stage::Compute ? HeaderSize : 0);
    }

    u64 ReadInstruction(u32 address) override {
        if (address / 8 >= code.size()) throw Shader::RuntimeError("instruction address {:#x} out of range", address);
        return code[address / 8];
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (const auto t = Shader::Ampere::NativeProgramAddress(code, address)) return *t;
        throw Shader::RuntimeError("invalid native indirect branch address {:#x}", address);
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return Shader::TexturePixelFormat::A8B8G8R8_UNORM; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return texture_bound; }
    u32 LocalMemorySize() const override { return 0x10000; }
    u32 SharedMemorySize() const override { return 0xC000; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

    std::vector<u64> code;
    size_t captured{};
    u32 texture_bound = 0;
};

Shader::Profile make_test_profile() {
    Shader::Profile p{};
    p.supported_spirv = 0x00010600;
    p.unified_descriptor_binding = true;
    p.support_descriptor_aliasing = true;
    p.support_int8 = p.support_int16 = p.support_int64 = true;
    p.support_uniform_and_storage_buffer_8bit = p.support_storage_buffer_8bit = true;
    p.support_uniform_and_storage_buffer_16bit = p.support_storage_buffer_16bit = true;
    p.support_vertex_instance_id = false;
    p.support_float_controls = true;
    p.support_separate_denorm_behavior = p.support_separate_rounding_mode = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.support_fp16_signed_zero_nan_preserve = p.support_fp32_signed_zero_nan_preserve = true;
    p.support_vote = true;
    p.supported_subgroup_stages = ~0u;
    p.support_viewport_index_layer_non_geometry = true;
    p.support_typeless_image_loads = true;
    p.support_demote_to_helper_invocation = true;
    p.support_int64_atomics = true;
    p.support_derivative_control = true;
    p.support_multi_viewport = true;
    p.support_sampled_image_array_nonuniform_indexing = true;
    p.warp_size_potentially_larger_than_guest = false;
    p.lower_left_origin_mode = false;
    p.need_declared_frag_colors = false;
    p.max_subgroup_size = 32;
    p.min_ssbo_alignment = 16;
    return p;
}

Shader::HostTranslateInfo make_test_host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = true;
    h.support_float16 = true;
    h.support_int64 = true;
    h.needs_demote_reorder = false;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = true;
    h.min_ssbo_alignment = 16;
    h.support_geometry_shader_passthrough = false;
    h.support_conditional_barrier = true;
    return h;
}

struct StageStats {
    int total = 0;
    int lower_ok = 0;
    int spirv_ok = 0;
    int spirv_val_ok = 0;
    int vk_pipeline_ok = 0;
};

VkShaderModule create_shader_module(VkDevice device, const uint32_t* code, size_t size_bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size_bytes;
    ci.pCode = code;
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: shader_pipeline_test <shaders_dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (std::system("spirv-val --version >/dev/null 2>&1") != 0) {
        std::fprintf(stderr, "spirv-val not on PATH\n");
        return 2;
    }

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "ShaderPipelineTest";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo inst_ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    inst_ci.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&inst_ci, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create Vulkan instance\n");
        return 1;
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (!gpu_count) {
        std::fprintf(stderr, "No Vulkan physical devices found\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());

    VkPhysicalDevice selected_gpu = gpus[0];
    for (auto gpu : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        if (props.vendorID == 0x1002) { // AMD RADV
            selected_gpu = gpu;
            std::printf("Selected GPU: %s (vendor 0x%04x, device 0x%04x)\n", props.deviceName, props.vendorID, props.deviceID);
            break;
        }
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queue_priority;

    const char* dev_extensions[] = {
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
    };

    VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    mesh_features.meshShader = VK_TRUE;
    mesh_features.taskShader = VK_TRUE;

    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.pNext = &mesh_features;
    v13.dynamicRendering = VK_TRUE;
    v13.maintenance4 = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &v13;
    features2.features.shaderFloat64 = VK_TRUE;
    features2.features.shaderInt64 = VK_TRUE;
    features2.features.shaderInt16 = VK_TRUE;
    features2.features.tessellationShader = VK_TRUE;
    features2.features.geometryShader = VK_TRUE;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_extensions;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(selected_gpu, &dci, nullptr, &device) != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create Vulkan device\n");
        return 1;
    }

    VkShaderModule dummy_vs = create_shader_module(device, dummy_vs_spv, sizeof dummy_vs_spv);
    VkShaderModule dummy_ms = create_shader_module(device, dummy_ms_spv, sizeof dummy_ms_spv);
    VkShaderModule dummy_tcs = create_shader_module(device, dummy_tcs_spv, sizeof dummy_tcs_spv);
    VkShaderModule dummy_tes = create_shader_module(device, dummy_tes_spv, sizeof dummy_tes_spv);

    const char* stage_names[] = {
        "Vertex (0)", "Fragment (1)", "Geometry (2)", "TessControl (3)",
        "TessEval (4)", "Compute (5)", "Task (6)", "Mesh (7)"
    };
    std::map<int, StageStats> stats;
    std::map<std::string, int> lower_failures;
    std::map<std::string, int> mesh_lower_failures;
    std::map<std::string, int> task_lower_failures;
    std::map<std::string, int> spirv_failures;
    std::map<std::string, int> spirv_val_failures;
    std::map<std::string, int> pipeline_failures;

    int title_gameface_total = 0;
    int title_gameface_pipe_ok = 0;

    std::vector<std::string> code_files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".code") {
            code_files.push_back(entry.path().string());
        }
    }
    std::sort(code_files.begin(), code_files.end());
    std::printf("Testing %zu shaders across all archives...\n\n", code_files.size());

    // spirv-val comes from PATH; temp files go next to the shaders, not /tmp
    const std::string tmp_spv = dir + "/.pipeline_test_tmp.spv";
    const std::string tmp_err = dir + "/.pipeline_test_err.txt";

    for (const auto& cf : code_files) {
        std::filesystem::path p(cf);
        std::string filename = p.filename().string();
        std::string arch = filename.substr(0, filename.find('_'));
        bool is_title_gameface = (arch == "title" || arch == "gameface");
        if (is_title_gameface) {
            title_gameface_total++;
        }

        std::string stem = p.stem().string();
        std::string base = p.parent_path().string() + "/" + stem;
        std::string ctl_file = base + ".ctl";

        u16 st = 1;
        std::vector<u8> ctl;
        std::ifstream ctl_in(ctl_file, std::ios::binary);
        if (ctl_in) {
            ctl.assign(std::istreambuf_iterator<char>(ctl_in), std::istreambuf_iterator<char>());
            if (ctl.size() > 0x56) {
                st = ctl[0x56] | (ctl[0x57] << 8);
            }
        }

        auto& s = stats[st];
        s.total++;

        std::ifstream in(cf, std::ios::binary);
        if (!in) continue;
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        // Step 1: LowerToMaxwell
        try {
            FullEnv env(bytes, st, ctl.empty() ? nullptr : ctl.data());
            s.lower_ok++;

            // Step 2: Translate Maxwell to SPIR-V
            std::vector<u32> spirv;
            Shader::IR::Program program;
            try {
                Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
                Shader::ObjectPool<Shader::IR::Inst> insts;
                Shader::ObjectPool<Shader::IR::Block> blocks;
                Shader::HostTranslateInfo host = make_test_host_info();
                const bool graphics = (st != 5);
                Shader::Maxwell::Flow::CFG cfg(env, flow_blocks, env.StartAddress() + (graphics ? HeaderSize : 0), true);
                program = Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, host);
                Shader::Profile profile = make_test_profile();

                if (graphics) {
                    Shader::RuntimeInfo runtime{};
                    Shader::Backend::Bindings bindings{};
                    spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
                } else {
                    spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
                }
                s.spirv_ok++;
            } catch (const std::exception& e) {
                spirv_failures[e.what()]++;
                continue;
            }

            // Step 3: Validate SPIR-V with spirv-val
            {
                std::ofstream out(tmp_spv, std::ios::binary);
                out.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(u32));
                out.close();

                std::string cmd = "spirv-val \"" + tmp_spv + "\" 2>\"" + tmp_err + "\"";
                int ret = std::system(cmd.c_str());
                if (ret == 0) {
                    s.spirv_val_ok++;
                } else {
                    std::ifstream errf(tmp_err);
                    std::string errline;
                    std::getline(errf, errline);
                    spirv_val_failures[errline.empty() ? "spirv-val non-zero exit" : errline]++;
                    // CRUCIAL: Skip pipeline creation if spirv-val fails!
                    continue;
                }
            }

            // Step 4: Real VkPipeline creation on RADV
            {
                VkShaderStageFlags vk_stage = 0;
                switch (st) {
                case 0: vk_stage = VK_SHADER_STAGE_VERTEX_BIT; break;
                case 1: vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
                case 2: vk_stage = VK_SHADER_STAGE_GEOMETRY_BIT; break;
                case 3: vk_stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
                case 4: vk_stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break;
                case 5: vk_stage = VK_SHADER_STAGE_COMPUTE_BIT; break;
                case 6: vk_stage = VK_SHADER_STAGE_TASK_BIT_EXT; break;
                case 7: vk_stage = VK_SHADER_STAGE_MESH_BIT_EXT; break;
                default: vk_stage = VK_SHADER_STAGE_FRAGMENT_BIT; break;
                }

                // Build descriptor set layout from program.info
                std::vector<VkDescriptorSetLayoutBinding> bindings;
                auto add_desc = [&](VkDescriptorType type, const auto& descriptors) {
                    for (const auto& d : descriptors) {
                        bindings.push_back({(uint32_t)bindings.size(), type, d.count, VK_SHADER_STAGE_ALL, nullptr});
                    }
                };
                add_desc(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, program.info.constant_buffer_descriptors);
                add_desc(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, program.info.storage_buffers_descriptors);
                add_desc(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, program.info.texture_buffer_descriptors);
                add_desc(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, program.info.image_buffer_descriptors);
                add_desc(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, program.info.texture_descriptors);
                add_desc(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, program.info.image_descriptors);

                VkDescriptorSetLayoutCreateInfo dsl_ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                dsl_ci.bindingCount = (uint32_t)bindings.size();
                dsl_ci.pBindings = bindings.empty() ? nullptr : bindings.data();
                VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
                vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &dsl);

                VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, 128};
                VkPipelineLayoutCreateInfo pl_ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pl_ci.setLayoutCount = dsl ? 1 : 0;
                pl_ci.pSetLayouts = &dsl;
                pl_ci.pushConstantRangeCount = 1;
                pl_ci.pPushConstantRanges = &pcr;
                VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
                vkCreatePipelineLayout(device, &pl_ci, nullptr, &pipe_layout);

                VkShaderModule mod = create_shader_module(device, spirv.data(), spirv.size() * sizeof(u32));
                bool pipe_created = false;

                if (st == 5) {
                    // Compute pipeline
                    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
                    cpci.layout = pipe_layout;
                    VkPipeline pipe = VK_NULL_HANDLE;
                    VkResult res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
                    if (res == VK_SUCCESS) {
                        pipe_created = true;
                        vkDestroyPipeline(device, pipe, nullptr);
                    } else {
                        char err[64];
                        std::snprintf(err, sizeof err, "vkCreateComputePipelines error %d", (int)res);
                        pipeline_failures[err]++;
                    }
                } else {
                    // Graphics pipeline
                    std::vector<VkPipelineShaderStageCreateInfo> stages;
                    if (st == 0) { // VS
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, mod, "main", nullptr});
                    } else if (st == 1) { // FS
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, dummy_vs, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mod, "main", nullptr});
                    } else if (st == 2) { // GS
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, dummy_vs, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT, mod, "main", nullptr});
                    } else if (st == 3) { // TCS
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, dummy_vs, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, mod, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, dummy_tes, "main", nullptr});
                    } else if (st == 4) { // TES
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, dummy_vs, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, dummy_tcs, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, mod, "main", nullptr});
                    } else if (st == 6) { // Task
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_TASK_BIT_EXT, mod, "main", nullptr});
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MESH_BIT_EXT, dummy_ms, "main", nullptr});
                    } else if (st == 7) { // Mesh
                        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MESH_BIT_EXT, mod, "main", nullptr});
                    }

                    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
                    ia.topology = (st == 3 || st == 4) ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

                    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
                    vp.viewportCount = 1;
                    vp.scissorCount = 1;

                    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
                    dy.dynamicStateCount = 2;
                    dy.pDynamicStates = dyn_states;

                    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                    rs.lineWidth = 1.0f;
                    rs.rasterizerDiscardEnable = (st != 1) ? VK_TRUE : VK_FALSE;

                    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
                    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

                    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
                    ts.patchControlPoints = 3;

                    VkPipelineColorBlendAttachmentState cba{};
                    cba.colorWriteMask = 0xf;
                    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
                    cb.attachmentCount = (st == 1) ? 1 : 0;
                    cb.pAttachments = (st == 1) ? &cba : nullptr;

                    VkPipelineRenderingCreateInfo dynamic_rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
                    VkFormat color_fmt = VK_FORMAT_R8G8B8A8_UNORM;
                    dynamic_rendering.colorAttachmentCount = (st == 1) ? 1 : 0;
                    dynamic_rendering.pColorAttachmentFormats = (st == 1) ? &color_fmt : nullptr;

                    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                    gpci.pNext = &dynamic_rendering;
                    gpci.stageCount = (uint32_t)stages.size();
                    gpci.pStages = stages.data();
                    gpci.pVertexInputState = (st == 6 || st == 7) ? nullptr : &vi;
                    gpci.pInputAssemblyState = (st == 6 || st == 7) ? nullptr : &ia;
                    gpci.pTessellationState = (st == 3 || st == 4) ? &ts : nullptr;
                    gpci.pViewportState = &vp;
                    gpci.pRasterizationState = &rs;
                    gpci.pMultisampleState = &ms;
                    gpci.pColorBlendState = &cb;
                    gpci.pDynamicState = &dy;
                    gpci.layout = pipe_layout;

                    VkPipeline pipe = VK_NULL_HANDLE;
                    VkResult res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe);
                    if (res == VK_SUCCESS) {
                        pipe_created = true;
                        vkDestroyPipeline(device, pipe, nullptr);
                    } else {
                        char err[64];
                        std::snprintf(err, sizeof err, "vkCreateGraphicsPipelines error %d", (int)res);
                        pipeline_failures[err]++;
                    }
                }

                if (pipe_created) {
                    s.vk_pipeline_ok++;
                    if (is_title_gameface) {
                        title_gameface_pipe_ok++;
                    }
                }

                vkDestroyShaderModule(device, mod, nullptr);
                if (pipe_layout) vkDestroyPipelineLayout(device, pipe_layout, nullptr);
                if (dsl) vkDestroyDescriptorSetLayout(device, dsl, nullptr);
            }
        } catch (const std::exception& e) {
            lower_failures[e.what()]++;
            if (st == 6) task_lower_failures[e.what()]++;
            if (st == 7) mesh_lower_failures[e.what()]++;
        }
    }

    std::remove(tmp_spv.c_str());
    std::remove(tmp_err.c_str());

    vkDestroyShaderModule(device, dummy_vs, nullptr);
    vkDestroyShaderModule(device, dummy_ms, nullptr);
    vkDestroyShaderModule(device, dummy_tcs, nullptr);
    vkDestroyShaderModule(device, dummy_tes, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    // Print summary
    std::printf("========================================================================================================\n");
    std::printf("%-18s | %5s | %10s | %10s | %10s | %10s\n",
                "Stage", "Total", "Lower OK", "SPIR-V OK", "spirv-val", "VkPipeline");
    std::printf("--------------------------------------------------------------------------------------------------------\n");
    int grand_total = 0, grand_lower = 0, grand_spirv = 0, grand_val = 0, grand_pipe = 0;
    for (int st = 0; st <= 7; st++) {
        const auto& s = stats[st];
        grand_total += s.total;
        grand_lower += s.lower_ok;
        grand_spirv += s.spirv_ok;
        grand_val += s.spirv_val_ok;
        grand_pipe += s.vk_pipeline_ok;
        std::printf("%-18s | %5d | %5d %3.0f%% | %5d %3.0f%% | %5d %3.0f%% | %5d %3.0f%%\n",
                    stage_names[st], s.total,
                    s.lower_ok, s.total ? 100.0 * s.lower_ok / s.total : 0.0,
                    s.spirv_ok, s.total ? 100.0 * s.spirv_ok / s.total : 0.0,
                    s.spirv_val_ok, s.total ? 100.0 * s.spirv_val_ok / s.total : 0.0,
                    s.vk_pipeline_ok, s.total ? 100.0 * s.vk_pipeline_ok / s.total : 0.0);
    }
    std::printf("--------------------------------------------------------------------------------------------------------\n");
    std::printf("%-18s | %5d | %5d %3.0f%% | %5d %3.0f%% | %5d %3.0f%% | %5d %3.0f%%\n",
                "Total", grand_total,
                grand_lower, grand_total ? 100.0 * grand_lower / grand_total : 0.0,
                grand_spirv, grand_total ? 100.0 * grand_spirv / grand_total : 0.0,
                grand_val, grand_total ? 100.0 * grand_val / grand_total : 0.0,
                grand_pipe, grand_total ? 100.0 * grand_pipe / grand_total : 0.0);
    std::printf("========================================================================================================\n\n");

    std::printf("Title & Gameface Shaders: %d / %d passed full VkPipeline creation\n\n",
                title_gameface_pipe_ok, title_gameface_total);

    std::printf("--- Top Mesh Shader (Stage 7) Lowering Failures (%zu unique reasons) ---\n", mesh_lower_failures.size());
    std::vector<std::pair<int, std::string>> sorted_mesh;
    for (const auto& [k, v] : mesh_lower_failures) sorted_mesh.push_back({-v, k});
    std::sort(sorted_mesh.begin(), sorted_mesh.end());
    for (size_t i = 0; i < std::min<size_t>(10, sorted_mesh.size()); i++) {
        std::printf("  %5d: %s\n", -sorted_mesh[i].first, sorted_mesh[i].second.c_str());
    }

    std::printf("\n--- Top Task Shader (Stage 6) Lowering Failures (%zu unique reasons) ---\n", task_lower_failures.size());
    std::vector<std::pair<int, std::string>> sorted_task;
    for (const auto& [k, v] : task_lower_failures) sorted_task.push_back({-v, k});
    std::sort(sorted_task.begin(), sorted_task.end());
    for (size_t i = 0; i < std::min<size_t>(10, sorted_task.size()); i++) {
        std::printf("  %5d: %s\n", -sorted_task[i].first, sorted_task[i].second.c_str());
    }

    std::printf("\n--- Top Overall Lowering Failures (%zu unique reasons) ---\n", lower_failures.size());
    std::vector<std::pair<int, std::string>> sorted_lower;
    for (const auto& [k, v] : lower_failures) sorted_lower.push_back({-v, k});
    std::sort(sorted_lower.begin(), sorted_lower.end());
    for (size_t i = 0; i < std::min<size_t>(10, sorted_lower.size()); i++) {
        std::printf("  %5d: %s\n", -sorted_lower[i].first, sorted_lower[i].second.c_str());
    }

    // Group lowering failures by the "op 0x..." tag; untagged reasons count as "other"
    std::map<std::string, int> by_op;
    for (const auto& [k, v] : lower_failures) {
        const size_t p = k.find("op 0x");
        by_op[p == std::string::npos ? "other" : k.substr(p + 3, k.find_first_of("):", p) - p - 3)] += v;
    }
    std::vector<std::pair<int, std::string>> sorted_op;
    for (const auto& [k, v] : by_op) sorted_op.push_back({-v, k});
    std::sort(sorted_op.begin(), sorted_op.end());
    std::printf("\n--- Lowering Failures by Opcode (%zu opcodes) ---\n", sorted_op.size());
    for (const auto& [n, op] : sorted_op) std::printf("  %5d: %s\n", -n, op.c_str());

    if (!spirv_failures.empty()) {
        std::printf("\n--- SPIR-V Emission Failures (%zu unique reasons) ---\n", spirv_failures.size());
        for (const auto& [k, v] : spirv_failures) std::printf("  %5d: %s\n", v, k.c_str());
    }
    if (!spirv_val_failures.empty()) {
        std::printf("\n--- spirv-val Failures (%zu unique reasons) ---\n", spirv_val_failures.size());
        for (const auto& [k, v] : spirv_val_failures) std::printf("  %5d: %s\n", v, k.c_str());
    }
    if (!pipeline_failures.empty()) {
        std::printf("\n--- VkPipeline Creation Failures (%zu unique reasons) ---\n", pipeline_failures.size());
        for (const auto& [k, v] : pipeline_failures) std::printf("  %5d: %s\n", v, k.c_str());
    }

    return 0;
}
