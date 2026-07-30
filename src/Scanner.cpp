#include "PCH.h"
#include "Scanner.h"

namespace slh::Scanner
{
    namespace
    {
        struct Pattern
        {
            std::vector<std::uint8_t> bytes;
            std::vector<bool>         mask;   // true = byte must match
        };

        Pattern Parse(std::string_view sig)
        {
            Pattern p;
            std::size_t i = 0;
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            while (i < sig.size()) {
                if (sig[i] == ' ' || sig[i] == '\t' || sig[i] == '\r' || sig[i] == '\n') { i++; continue; }
                if (sig[i] == '?') {
                    p.bytes.push_back(0);
                    p.mask.push_back(false);
                    i++;
                    if (i < sig.size() && sig[i] == '?') i++;
                } else {
                    if (i + 1 >= sig.size()) break;
                    int hi = hex(sig[i]);
                    int lo = hex(sig[i+1]);
                    if (hi < 0 || lo < 0) { i++; continue; }
                    p.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
                    p.mask.push_back(true);
                    i += 2;
                }
            }
            return p;
        }

        bool Matches(const std::uint8_t* mem, const Pattern& p) noexcept
        {
            for (std::size_t i = 0; i < p.bytes.size(); ++i) {
                if (p.mask[i] && mem[i] != p.bytes[i]) return false;
            }
            return true;
        }

        struct SectionRange { std::uintptr_t start; std::size_t size; const char* name; };

        std::vector<SectionRange> EnumerateTextSections() noexcept
        {
            std::vector<SectionRange> out;
            HMODULE hMod = ::GetModuleHandleW(nullptr);
            if (!hMod) return out;
            auto base = reinterpret_cast<std::uintptr_t>(hMod);
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
                SectionRange r;
                r.start = base + sec[i].VirtualAddress;
                r.size  = sec[i].Misc.VirtualSize;
                r.name  = "?";  // sec[i].Name not C-string-terminated; fine
                out.push_back(r);
            }
            return out;
        }
    }

    std::uintptr_t FindUnique(std::string_view pattern,
                              std::string_view label,
                              bool& outAmbiguous)
    {
        outAmbiguous = false;
        auto p = Parse(pattern);
        if (p.bytes.empty()) {
            spdlog::warn("[scan] {}: empty pattern", label);
            return 0;
        }

        auto sections = EnumerateTextSections();
        if (sections.empty()) {
            spdlog::error("[scan] {}: no executable sections", label);
            return 0;
        }

        std::uintptr_t found = 0;
        const std::uint8_t firstByte = p.bytes[0];
        for (auto& s : sections) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(s.start);
            const auto end = s.size >= p.bytes.size() ? s.size - p.bytes.size() : 0;
            for (std::size_t off = 0; off <= end; ++off) {
                if (begin[off] != firstByte) continue;
                if (!Matches(begin + off, p)) continue;
                auto site = reinterpret_cast<std::uintptr_t>(begin + off);
                if (found) {
                    outAmbiguous = true;
                    spdlog::warn("[scan] {}: ambiguous (at least 0x{:x} and 0x{:x}); refusing to pick",
                                 label, found, site);
                    return 0;
                }
                found = site;
            }
        }

        if (!found) {
            spdlog::warn("[scan] {}: no match in {} executable section(s) ({} bytes pattern)",
                         label, sections.size(), p.bytes.size());
        } else {
            HMODULE hMod = ::GetModuleHandleW(nullptr);
            auto base = reinterpret_cast<std::uintptr_t>(hMod);
            spdlog::info("[scan] {}: matched at 0x{:x}  (RVA 0x{:x})",
                         label, found, found - base);
        }
        return found;
    }

    std::uintptr_t DecodeRipRelativeTarget(std::uintptr_t instrSite,
                                          std::size_t dispOffsetInInstr,
                                          std::size_t instrLen) noexcept
    {
        std::int32_t disp = 0;
        __try {
            disp = *reinterpret_cast<const std::int32_t*>(instrSite + dispOffsetInInstr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        return instrSite + instrLen + static_cast<std::intptr_t>(disp);
    }
}
