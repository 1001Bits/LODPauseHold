#include "PCH.h"
#include "AddressLib.h"

namespace slh::AddressLib
{
    namespace
    {
        std::vector<std::uint8_t>   g_binData;
        std::uint64_t               g_entryCount = 0;
        std::size_t                 g_bodyOffset = 0;
        Version                     g_version{};
        std::string                 g_binPath;
        bool                        g_loaded = false;

        std::filesystem::path DllDir()
        {
            wchar_t modPath[MAX_PATH] = {};
            HMODULE hMod = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&DllDir), &hMod);
            ::GetModuleFileNameW(hMod, modPath, MAX_PATH);
            return std::filesystem::path(modPath).parent_path();
        }

        bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
        {
            std::error_code ec;
            auto size = std::filesystem::file_size(path, ec);
            if (ec) return false;
            std::ifstream fs(path, std::ios::binary);
            if (!fs.is_open()) return false;
            out.resize(size);
            fs.read(reinterpret_cast<char*>(out.data()), size);
            return static_cast<std::size_t>(fs.gcount()) == size;
        }
    }

    std::optional<Version> DetectModuleVersion(HMODULE module)
    {
        if (!module) module = ::GetModuleHandleW(nullptr);
        if (!module) return std::nullopt;

        wchar_t exePath[MAX_PATH] = {};
        if (!::GetModuleFileNameW(module, exePath, MAX_PATH)) return std::nullopt;

        DWORD handle = 0;
        DWORD size = ::GetFileVersionInfoSizeW(exePath, &handle);
        if (size == 0) return std::nullopt;
        std::vector<std::uint8_t> buf(size);
        if (!::GetFileVersionInfoW(exePath, handle, size, buf.data())) return std::nullopt;

        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len)) return std::nullopt;
        if (!ffi || len < sizeof(VS_FIXEDFILEINFO)) return std::nullopt;

        Version v;
        v.v0 = HIWORD(ffi->dwFileVersionMS);
        v.v1 = LOWORD(ffi->dwFileVersionMS);
        v.v2 = HIWORD(ffi->dwFileVersionLS);
        v.v3 = LOWORD(ffi->dwFileVersionLS);
        return v;
    }

    bool LoadForVersion(const Version& v)
    {
        g_loaded = false;
        g_version = v;

        // The bin lives next to our DLL (= Data/SFSE/Plugins/). Filename pattern:
        //   versionlib-<v0>-<v1>-<v2>-<v3>.bin
        char fname[64];
        std::snprintf(fname, sizeof(fname),
                      "versionlib-%u-%u-%u-%u.bin", v.v0, v.v1, v.v2, v.v3);

        auto dir = DllDir();
        auto path = dir / fname;
        g_binPath = path.string();

        if (!std::filesystem::exists(path)) {
            spdlog::warn("[addrlib] no bin at {} for runtime version {}", g_binPath, v.str());
            return false;
        }

        std::vector<std::uint8_t> data;
        if (!ReadFile(path, data)) {
            spdlog::warn("[addrlib] read failed: {}", g_binPath);
            return false;
        }

        if (data.size() < 96) {
            spdlog::warn("[addrlib] {} too small ({} bytes)", g_binPath, data.size());
            return false;
        }

        // Validate header
        std::uint32_t format = *reinterpret_cast<const std::uint32_t*>(&data[0]);
        if (format != 5) {
            spdlog::warn("[addrlib] {} format {} not supported (expected 5)", g_binPath, format);
            return false;
        }

        // Versions in header are 32-bit little-endian
        std::uint32_t hv0 = *reinterpret_cast<const std::uint32_t*>(&data[4]);
        std::uint32_t hv1 = *reinterpret_cast<const std::uint32_t*>(&data[8]);
        std::uint32_t hv2 = *reinterpret_cast<const std::uint32_t*>(&data[12]);
        std::uint32_t hv3 = *reinterpret_cast<const std::uint32_t*>(&data[16]);
        if (hv0 != v.v0 || hv1 != v.v1 || hv2 != v.v2 || hv3 != v.v3) {
            spdlog::warn("[addrlib] {} header says version {}.{}.{}.{} but runtime is {} — using anyway",
                         g_binPath, hv0, hv1, hv2, hv3, v.str());
        }

        std::uint32_t ptrSize = *reinterpret_cast<const std::uint32_t*>(&data[84]);
        std::uint32_t count   = *reinterpret_cast<const std::uint32_t*>(&data[92]);

        // Validate sizes: flat format expects header(96) + 4*count bytes.
        std::size_t expectedSize = 96 + static_cast<std::size_t>(count) * 4;
        if (data.size() < expectedSize) {
            spdlog::warn("[addrlib] {} size {} smaller than expected {} (count={}); using anyway",
                         g_binPath, data.size(), expectedSize, count);
        }

        g_binData = std::move(data);
        g_entryCount = count;
        g_bodyOffset = 96;
        g_loaded = true;

        spdlog::info("[addrlib] loaded {} — version {}, ptrSize={}, entries={}",
                     g_binPath, v.str(), ptrSize, count);
        return true;
    }

    std::uintptr_t LookupRVA(std::uint64_t id)
    {
        if (!g_loaded) return 0;
        if (id >= g_entryCount) return 0;
        std::size_t offset = g_bodyOffset + static_cast<std::size_t>(id) * 4;
        if (offset + 4 > g_binData.size()) return 0;
        std::uint32_t rva = *reinterpret_cast<const std::uint32_t*>(&g_binData[offset]);
        return static_cast<std::uintptr_t>(rva);
    }

    Info GetInfo()
    {
        Info i;
        i.version = g_version;
        i.entryCount = g_entryCount;
        i.binPath = g_binPath;
        i.loaded = g_loaded;
        return i;
    }
}
