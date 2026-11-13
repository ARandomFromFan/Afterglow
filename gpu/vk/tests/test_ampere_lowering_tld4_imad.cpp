#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

int main() {
    using Shader::Ampere::Instruction;
    // Test 1: IMAD.MOV.S32 R9, RZ, RZ, -c[3][0xf0] (op 0x624, negate addend)
    std::vector<Instruction> code_imad = {
        Instruction{0xff097624, 0x80c03c00, 0x078e02ff, 0x040fc200},
        Instruction{0x0000794d, 0x00000000, 0x03800000, 0x00000000} // EXIT
    };
    auto res_imad = Shader::Ampere::LowerToMaxwell(code_imad);
    if (!res_imad) {
        std::fprintf(stderr, "IMAD lowering failed at +%#x: %s\n", res_imad.error_offset, std::string(res_imad.error).c_str());
        return 1;
    }
    std::printf("IMAD test passed: lowered %zu instructions\n", res_imad.instructions.size());

    // Test 2: TLD4 R8, R2, R9, U4 (op 0xf63, bindless with uniform handle)
    std::vector<Instruction> code_tld4 = {
        Instruction{0x02087f63, 0x30000409, 0x081e15ff, 0x000f4200},
        Instruction{0x0000794d, 0x00000000, 0x03800000, 0x00000000} // EXIT
    };
    auto res_tld4 = Shader::Ampere::LowerToMaxwell(code_tld4);
    if (!res_tld4) {
        std::fprintf(stderr, "TLD4 (0xf63) lowering failed at +%#x: %s\n", res_tld4.error_offset, std::string(res_tld4.error).c_str());
        return 1;
    }
    std::printf("TLD4 (0xf63) test passed: lowered %zu instructions\n", res_tld4.instructions.size());

    // Test 3: TLD4.B R8, R8, R10 (op 0x364, bindless with register handle)
    std::vector<Instruction> code_tld4_reg = {
        Instruction{0x08087364, 0x3800000a, 0x041e0f04, 0x000f4200},
        Instruction{0x0000794d, 0x00000000, 0x03800000, 0x00000000} // EXIT
    };
    auto res_tld4_reg = Shader::Ampere::LowerToMaxwell(code_tld4_reg);
    if (!res_tld4_reg) {
        std::fprintf(stderr, "TLD4 (0x364) lowering failed at +%#x: %s\n", res_tld4_reg.error_offset, std::string(res_tld4_reg.error).c_str());
        return 1;
    }
    assert(res_tld4_reg.instructions.size() == 9);
    assert((res_tld4_reg.instructions[3] >> 56) == 0xde); // TLD4.B opcode
    assert((res_tld4_reg.instructions[4] & 0xff) == 8);   // Dest 0: R8
    assert((res_tld4_reg.instructions[5] & 0xff) == 9);   // Dest 1: R9
    assert((res_tld4_reg.instructions[6] & 0xff) == 4);   // Dest 2: R4
    assert((res_tld4_reg.instructions[7] & 0xff) == 5);   // Dest 3: R5
    std::printf("TLD4 (0x364) test passed: lowered %zu instructions\n", res_tld4_reg.instructions.size());

    return 0;
}
