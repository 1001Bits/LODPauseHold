#pragma once
#include "PCH.h"

namespace slh::Scanner
{
    // Pattern syntax: hex bytes separated by spaces, "??" = wildcard.
    //   Example: "48 89 5c 24 08 57 48 83 ec 20 48 8b 05 ?? ?? ?? ??"
    // Returns the first match in the main module's .text section, or 0.
    std::uintptr_t FindUnique(std::string_view pattern,
                              std::string_view label,
                              bool& outAmbiguous);

    // Read a RIP-relative MOV/LEA target. `instrSite` is the address of the
    // first opcode byte; `dispOffsetInInstr` is the offset to the disp32 inside
    // the instruction; `instrLen` is the total instruction length.
    // E.g. for `48 8b 05 <d32>` (mov rax, [rip+d32]): dispOffsetInInstr=3, instrLen=7.
    std::uintptr_t DecodeRipRelativeTarget(std::uintptr_t instrSite,
                                           std::size_t dispOffsetInInstr,
                                           std::size_t instrLen) noexcept;
}
