#pragma once

#include <Windows.h>
#include <ShlObj.h>
// IDXGIAdapter3::QueryVideoMemoryInfo — the OS's view of our VRAM budget and
// usage, which the engine's own tracker cannot see. dxgi.dll is loaded by name
// at runtime, so no import is added to the plugin.
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <MinHook.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace slh
{
    // Project namespace ("starfield lod hold")
    inline constexpr std::string_view kPluginName = "StarfieldPauseLODHold";
    // Packed per REL::Version (major<<24 | minor<<16 | patch<<4 | build).
    // v0.8.0 → exact PauseMenu-scoped state freeze. Promotion and degradation
    // states are redirected through the engine's own Reset transition while
    // PauseMenu is open; no budget or LOD work is carried across unpause.
    // v0.9.0 → same fix, made provable: per-pause recovery report, in-pause
    // telemetry, the OS's own VRAM view, fresh log per launch, and a fix for
    // the publication order that could drop a post-pause discard.
    // v1.0.0 — first public release. The pause-menu LOD freeze, plus the
    // teardown guard: the engine's own quit-requested flag (Main::Singleton
    // +0x28) makes the freeze stand down the moment a quit is requested, which
    // is what stopped it redirecting into the engine during shutdown.
    // v1.1.0 → VRAM pressure gate. The freeze redirects every LOD state through
    // Reset, which also zeroes totalDegrade, so a paused engine could not demote
    // at all — its only route back from VRAM pressure. Harmless with headroom,
    // compounding without it. The freeze now stands down for the rest of a pause
    // whenever the engine needs to shed.
    // v1.1.0 adds, over 1.0.0:
    //  - VRAM pressure gate: the freeze stands down for the rest of a pause
    //    whenever the engine needs to shed textures, because redirecting every
    //    LOD state through Reset also zeroes totalDegrade — the engine's only
    //    route back from VRAM pressure.
    //  - Dormancy on quit: both worker threads exit the moment the engine's
    //    quit flag is set, so nothing of ours touches the engine, the graphics
    //    driver or the disk during shutdown.
    //  - Fast quit to desktop: the exit teardown re-serializes the whole D3D12
    //    pipeline cache and walks its heap (up to a minute). TerminateProcess
    //    skips it, gated on the engine's own pipeline-dirty byte so a session
    //    that compiled new shaders still persists them that once.
    //  - Audit fixes: detached worker threads (a joinable static std::thread
    //    would call std::terminate at process detach), freeze refuses to
    //    install when prologue verification fails, no stray release Reset
    //    after a stood-down pause, and honest diagnostic counters.
    inline constexpr std::uint32_t kPluginVersion = (1u << 24) | (1u << 16) | (0u << 4) | 0u;
    inline constexpr const char* kPluginNameC = "StarfieldPauseLODHold";
}
