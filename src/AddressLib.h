#pragma once
#include "PCH.h"

namespace slh::AddressLib
{
    struct Version
    {
        std::uint16_t v0, v1, v2, v3;
        std::string str() const {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v0, v1, v2, v3);
            return buf;
        }
    };

    // Detect the running Starfield's file version via GetFileVersionInfo.
    [[nodiscard]] std::optional<Version> DetectModuleVersion(HMODULE module = nullptr);

    // Load and parse the matching versionlib bin from Data/SFSE/Plugins/
    // alongside our own DLL.
    bool LoadForVersion(const Version& v);

    // Look up an ID. Returns 0 if unloaded / out of range.
    [[nodiscard]] std::uintptr_t LookupRVA(std::uint64_t id);

    // Diagnostic info — version + entry count + bin path.
    struct Info
    {
        Version       version;
        std::uint64_t entryCount;
        std::string   binPath;
        bool          loaded;
    };
    [[nodiscard]] Info GetInfo();
}
