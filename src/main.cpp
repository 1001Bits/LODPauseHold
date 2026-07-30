#include "PCH.h"
#include "Hook.h"
#include "MenuWatcher.h"
#include "AddressLib.h"

// SFSE plugin entries are x64-cdecl (the only convention available, but spelt
// for parity with the SFSE SDK headers).
#define SFSEAPI __cdecl

// ─── Minimal SFSE plugin export shim ────────────────────────────────────────
// We don't depend on the full SFSE/CommonLibSF SDK — only the layout of
// SFSEPluginVersionData (stable since SFSE 0.2) and the load callback
// signature. This avoids a hard dependency on a specific SFSE SDK version.

extern "C" {

// SFSE plugin version structure — layout from CommonLibSF/SFSE/Interfaces.h.
// Field order + offsets are stable since SFSE 0.2.0; the flag bits below come
// from CommonLibSF's PluginVersionData setter methods and match what SFSE
// 0.2.20 (the 1.16.244-era loader) actually checks.
struct SFSE_PluginVersionData
{
    enum : std::uint32_t { kVersion = 1 };

    // addressIndependence — bit values per CommonLibSF Interfaces.h
    enum : std::uint32_t {
        kAddressIndependence_Signatures     = 1 << 0,
        kAddressIndependence_AddressLibrary = 1 << 2,  // Address Library v2
    };

    // structureCompatibility — bit values per CommonLibSF Interfaces.h
    enum : std::uint32_t {
        kStructureCompatibility_NoStructUse     = 1 << 0,  // plugin doesn't read engine structs
        kStructureCompatibility_LayoutDependent = 1 << 3,  // depends on layouts >= 1.14.70
    };

    std::uint32_t dataVersion;                   // = kVersion
    std::uint32_t pluginVersion;                 // packed (major<<24 | minor<<16 | patch<<4 | build)
    char          name[256];
    char          author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureCompatibility;
    std::uint32_t compatibleVersions[16];        // zero-terminated; empty = any (when address-independent)
    std::uint32_t xseMinimum;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
};

__declspec(dllexport) SFSE_PluginVersionData SFSEPlugin_Version =
{
    SFSE_PluginVersionData::kVersion,
    slh::kPluginVersion,
    "StarfieldPauseLODHold",
    "tester-build",

    // Address Library (primary) + signature scan (fallback). Both paths are
    // wired in Hook::Install; either one resolves CheckBudget::Func4 correctly.
    SFSE_PluginVersionData::kAddressIndependence_AddressLibrary
        | SFSE_PluginVersionData::kAddressIndependence_Signatures,

    // We read raw engine struct offsets (+0xf8 budget tracker, +0x38b8 degrade
    // budget, etc.) — that's "layout dependent" in SFSE's vocabulary, with a
    // minimum runtime of 1.14.70 (the layout era we tested against).
    SFSE_PluginVersionData::kStructureCompatibility_LayoutDependent,

    // Empty version whitelist — with address-independence flags set, SFSE
    // accepts any runtime and lets the plugin self-validate at load time.
    { 0 },

    0,  // any SFSE
    0, 0
};

// SFSE load interface — minimal subset (we don't actually need any SFSE
// callbacks; loading-time is enough to install our hook).
struct SFSE_LoadInterface
{
    enum : std::uint32_t { kVersion = 1 };
    std::uint32_t sfseVersion;
    std::uint32_t runtimeVersion;
    std::uint32_t editorVersion;   // 0 if game runtime
    std::uint32_t isEditor;        // 0 if game runtime
    // … further entries unused by us.
};

}  // extern "C"

// ─── Logging ────────────────────────────────────────────────────────────────

namespace
{
    bool PinThisModule() noexcept
    {
        HMODULE pinned = nullptr;
        return ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&PinThisModule), &pinned) != FALSE;
    }

    std::filesystem::path FindLogPath()
    {
        // Write next to the DLL — easier for testers to find and zip up.
        wchar_t modPath[MAX_PATH] = {};
        HMODULE hMod = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&FindLogPath), &hMod);
        ::GetModuleFileNameW(hMod, modPath, MAX_PATH);
        return std::filesystem::path(modPath).parent_path() / "StarfieldPauseLODHold.log";
    }

    void SetupLogging(const std::string& level)
    {
        auto path = FindLogPath();
        try {
            // Fresh log every launch — the file no longer grows across runs.
            // The run before this one survives as *.prev.log so a tester who
            // relaunches out of habit can still recover the log that mattered.
            std::error_code ec;
            if (std::filesystem::exists(path, ec) && !ec) {
                auto prev = path;
                prev.replace_extension(".prev.log");
                std::filesystem::rename(path, prev, ec);  // best effort
            }
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(),
                                                                           /*truncate=*/true);
            auto log = std::make_shared<spdlog::logger>("slh", sink);
            log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
            if      (level == "trace") log->set_level(spdlog::level::trace);
            else if (level == "debug") log->set_level(spdlog::level::debug);
            else if (level == "info")  log->set_level(spdlog::level::info);
            else if (level == "warn")  log->set_level(spdlog::level::warn);
            else if (level == "error") log->set_level(spdlog::level::err);
            log->flush_on(spdlog::level::info);
            spdlog::set_default_logger(log);
            spdlog::flush_every(std::chrono::seconds(2));
        } catch (...) {
            // can't even open log — nothing we can do; press on silently
        }
    }
}

// ─── Config loading ─────────────────────────────────────────────────────────

namespace
{
    struct AppConfig
    {
        bool enabled = true;
        slh::Hook::Config hook;
        slh::MenuWatcher::Settings menu;
        std::vector<std::string> suspendingMenus;
        std::string logLevel = "info";
        bool iniFound = false;
    };

    // The exact state freeze supersedes every legacy write. Applied on both the
    // INI and the no-INI path so the invariant cannot depend on a file existing.
    void EnforceFreezeSupersedesLegacy(AppConfig& c)
    {
        if (!c.hook.freezeLODWhilePaused) return;
        c.hook.clearRefreshAll = false;
        c.hook.holdUpgradeBudgets = false;
        c.hook.upgradeHoldAfterCloseMs = 0;
        c.hook.pressureGuardEnabled = false;
        c.hook.holdDegradeBudget = false;
        c.hook.watchdogEnabled = false;
        c.hook.forceRefreshAll = false;
    }

    std::filesystem::path FindConfigPath()
    {
        wchar_t modPath[MAX_PATH] = {};
        HMODULE hMod = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&FindConfigPath), &hMod);
        ::GetModuleFileNameW(hMod, modPath, MAX_PATH);
        return std::filesystem::path(modPath).parent_path() / L"StarfieldPauseLODHold.ini";
    }

    inline constexpr const char* kDefaultSigCheckBudget =
        "48 89 5c 24 08 57 48 83 ec 20 "
        "48 8b 05 ?? ?? ?? ?? "
        "49 8b f9 "
        "8b 1d ?? ?? ?? ?? "
        "48 c1 e3 14 "
        "4c 8b 90 f8 00 00 00 "
        "41 0f b6 4a 28 "
        "85 c9";

    AppConfig LoadConfig()
    {
        AppConfig c{};
        c.suspendingMenus = { "PauseMenu" };
        c.hook.sigCheckBudgetFunc4 = kDefaultSigCheckBudget;

        // The INI is optional. Every default in Hook::Config and
        // MenuWatcher::Settings is already the shipping configuration — fix on,
        // diagnostics off, no engine writes — so the plugin is complete without
        // one. A file only exists to turn diagnostics on for a bug report.
        auto path = FindConfigPath();
        if (!std::filesystem::exists(path)) {
            EnforceFreezeSupersedesLegacy(c);
            return c;  // no file — defaults are the release configuration
        }
        c.iniFound = true;
        auto wpath = path.wstring();
        const wchar_t* w = wpath.c_str();

        auto readInt = [&](const wchar_t* sec, const wchar_t* key, int def) -> int {
            return static_cast<int>(::GetPrivateProfileIntW(sec, key, def, w));
        };
        auto readBool = [&](const wchar_t* sec, const wchar_t* key, bool def) {
            return readInt(sec, key, def ? 1 : 0) != 0;
        };

        c.enabled     = readBool(L"General", L"bEnabled", true);
        c.hook.dryRun = readBool(L"General", L"bDryRun",  false);

        // v0.8 primary path: skip every native mesh/texture promote/demote
        // state while exact PauseMenu is open.
        c.hook.freezeLODWhilePaused = readBool(
            L"Fix", L"bFreezeLODWhilePaused", c.hook.freezeLODWhilePaused);
        c.hook.requireValidBudgetForFreeze = readBool(
            L"Safety", L"bRequireValidBudgetForFreeze", c.hook.requireValidBudgetForFreeze);

        // Legacy budget experiments retained for controlled A/B tests.
        c.hook.clearRefreshAll        = readBool(L"Fix", L"bClearRefreshAll", c.hook.clearRefreshAll);
        c.hook.holdUpgradeBudgets     = readBool(L"Fix", L"bHoldUpgradeBudgets",
                                                 c.hook.holdUpgradeBudgets);
        int holdAfterCloseMs = readInt(L"Fix", L"iUpgradeHoldAfterCloseMs", -1);
        if (holdAfterCloseMs < 0) {
            // Backward compatibility with the v0.6 tester INI.
            holdAfterCloseMs = readInt(L"Fix", L"iRefreshHoldAfterCloseMs",
                                       static_cast<int>(c.hook.upgradeHoldAfterCloseMs));
        }
        c.hook.upgradeHoldAfterCloseMs =
            static_cast<std::uint32_t>(std::max(0, holdAfterCloseMs));
        c.hook.pressureGuardEnabled = readBool(L"Fix", L"bPressureGuard",
                                               c.hook.pressureGuardEnabled);
        c.hook.pressureGuardEnterHeadroomBytes =
            static_cast<std::uint64_t>(std::max(
                0, readInt(L"Fix", L"iPressureGuardEnterHeadroomMB",
                           static_cast<int>(c.hook.pressureGuardEnterHeadroomBytes >> 20)))) << 20;
        c.hook.pressureGuardExitHeadroomBytes =
            static_cast<std::uint64_t>(std::max(
                0, readInt(L"Fix", L"iPressureGuardExitHeadroomMB",
                           static_cast<int>(c.hook.pressureGuardExitHeadroomBytes >> 20)))) << 20;
        c.hook.holdDegradeBudget      = readBool(L"Fix", L"bHoldDegradeBudget", c.hook.holdDegradeBudget);
        c.hook.minHeadroomForDegradeHoldBytes =
            static_cast<std::uint64_t>(std::max(
                0, readInt(L"Safety", L"iMinHeadroomForDegradeHoldMB",
                           static_cast<int>(c.hook.minHeadroomForDegradeHoldBytes >> 20)))) << 20;

        // Repro harness — deliberately causes the bug. Off unless asked for.
        c.hook.forceRefreshAll = readBool(L"Repro", L"bForceRefreshAll", false);

        // Diagnostics.
        c.hook.logTimeline      = readBool(L"Logging", L"bLogTimeline", c.hook.logTimeline);
        c.hook.timelinePeriodMs = static_cast<std::uint32_t>(
            readInt(L"Logging", L"iTimelinePeriodMs", static_cast<int>(c.hook.timelinePeriodMs)));
        if (c.hook.timelinePeriodMs < 50) c.hook.timelinePeriodMs = 50;
        c.hook.summaryIntervalMs = static_cast<std::uint32_t>(std::clamp(
            readInt(L"Logging", L"iSummaryIntervalMs",
                    static_cast<int>(c.hook.summaryIntervalMs)), 1000, 300000));

        // Safety envelope — see Hook.h for what each gate protects against.
        c.hook.maxSuppressMode      = readInt (L"Safety", L"iMaxHoldMode",        c.hook.maxSuppressMode);
        c.hook.abortOnEscalation    = readBool(L"Safety", L"bAbortOnEscalation",  c.hook.abortOnEscalation);
        c.hook.maxSuppressMsPerMenu = static_cast<std::uint32_t>(
            readInt(L"Safety", L"iMaxHoldMsPerMenu", static_cast<int>(c.hook.maxSuppressMsPerMenu)));
        c.hook.watchdogEnabled      = readBool(L"Safety", L"bWatchdogEnabled",    c.hook.watchdogEnabled);
        c.hook.watchdogRatio        = static_cast<float>(
            readInt(L"Safety", L"iWatchdogPercent", 50)) / 100.0f;
        c.hook.watchdogTripMs       = static_cast<std::uint32_t>(
            readInt(L"Safety", L"iWatchdogTripMs", static_cast<int>(c.hook.watchdogTripMs)));

        // Clamp anything a hand-edited INI could put out of range. iMaxHoldMode
        // above 1 re-enables the exact write that caused the v0.4 stall, so it
        // is capped at 2 and 3 is unreachable by configuration.
        if (c.hook.maxSuppressMode < 0) c.hook.maxSuppressMode = 0;
        if (c.hook.maxSuppressMode > 2) c.hook.maxSuppressMode = 2;
        if (c.hook.watchdogRatio < 0.05f) c.hook.watchdogRatio = 0.05f;
        if (c.hook.watchdogRatio > 1.0f)  c.hook.watchdogRatio = 1.0f;
        if (c.hook.watchdogTripMs < 100)  c.hook.watchdogTripMs = 100;
        if (c.hook.pressureGuardExitHeadroomBytes
            < c.hook.pressureGuardEnterHeadroomBytes) {
            c.hook.pressureGuardExitHeadroomBytes =
                c.hook.pressureGuardEnterHeadroomBytes;
        }

        // Menu detection.
        c.menu.useEnginePauseMenu = readBool(L"Detection", L"bUseEnginePauseMenu",
                                             c.menu.useEnginePauseMenu);
        c.menu.useCursorVisible  = readBool(L"Detection", L"bUseCursorVisible",  c.menu.useCursorVisible);
        c.menu.useCursorClip     = readBool(L"Detection", L"bUseCursorClip",     c.menu.useCursorClip);
        c.menu.requireForeground = readBool(L"Detection", L"bRequireForeground", c.menu.requireForeground);
        c.menu.openDebounceMs    = static_cast<std::uint32_t>(
            readInt(L"Detection", L"iOpenDebounceMs",  static_cast<int>(c.menu.openDebounceMs)));
        c.menu.closeDebounceMs   = static_cast<std::uint32_t>(
            readInt(L"Detection", L"iCloseDebounceMs", static_cast<int>(c.menu.closeDebounceMs)));
        c.menu.pollMs = static_cast<std::uint32_t>(
            std::clamp(readInt(L"Detection", L"iPollMs", static_cast<int>(c.menu.pollMs)), 5, 100));

        c.hook.logEveryCall = readBool(L"Logging", L"bLogEveryCall", c.hook.logEveryCall);
        wchar_t level[32] = {};
        ::GetPrivateProfileStringW(L"Logging", L"sLevel", L"info", level, 32, w);
        c.logLevel.clear();
        for (const wchar_t* p = level; *p; ++p) {          // ASCII keywords only
            c.logLevel.push_back(static_cast<char>(*p & 0x7f));
        }

        // Applied after all legacy/repro/safety keys have been read, so an older
        // or hand-edited INI cannot silently combine incompatible policies. To
        // run the deliberate repro harness, disable the exact freeze first.
        EnforceFreezeSupersedesLegacy(c);

        return c;
    }
}

// ─── Entry point ────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) bool SFSEAPI SFSEPlugin_Load(const SFSE_LoadInterface* a_sfse)
{
    auto cfg = LoadConfig();
    SetupLogging(cfg.logLevel);

    spdlog::info("=== StarfieldPauseLODHold v{}.{}.{} loading ===",
                 (slh::kPluginVersion >> 24) & 0xff,
                 (slh::kPluginVersion >> 16) & 0xff,
                 slh::kPluginVersion & 0xff);
    spdlog::info("SFSE version reported = 0x{:08x}, runtime = 0x{:08x}, isEditor = {}",
                 a_sfse ? a_sfse->sfseVersion : 0,
                 a_sfse ? a_sfse->runtimeVersion : 0,
                 a_sfse ? a_sfse->isEditor : 0);

    if (a_sfse && a_sfse->isEditor) {
        spdlog::warn("loaded into the Creation Kit — skipping hook install");
        return false;
    }

    // SFSE plugins have a process-lifetime lifecycle. Pin our module before
    // starting watcher/diagnostic threads or installing trampolines so an
    // unexpected FreeLibrary cannot unload code beneath an active callback.
    // Normal process termination does not need (and must not attempt) teardown
    // under the loader lock.
    if (!PinThisModule()) {
        spdlog::error("could not pin plugin module — refusing to start hooks/threads");
        return false;
    }
    spdlog::info("[runtime] plugin module pinned for process lifetime");

    if (!cfg.enabled) {
        spdlog::warn("[General].bEnabled = 0 in config; not installing hook (logging only)");
        slh::MenuWatcher::Install(cfg.menu, cfg.suspendingMenus);
        slh::MenuWatcher::Start();
        return true;
    }

    spdlog::info("[config] ini={}  dry_run={}  timeline={}  every_call={}",
                 cfg.iniFound ? "loaded" : "absent (built-in defaults)",
                 cfg.hook.dryRun, cfg.hook.logTimeline, cfg.hook.logEveryCall);
    if (!cfg.hook.logTimeline) {
        spdlog::info("[config] detailed timeline is off. For a bug report, put "
                     "StarfieldPauseLODHold.ini beside this DLL containing:  "
                     "[Logging]  bLogTimeline = 1");
    }
    spdlog::info("[config] fix: freezeLODWhilePaused={} (exact PauseMenu only)  "
                 "teardown guard: game-window liveness (no time limit on a pause)"
                 "  requireValidBudget={}",
                 cfg.hook.freezeLODWhilePaused, cfg.hook.requireValidBudgetForFreeze);
    spdlog::info("[config] legacy: clearRefreshAll={} holdUpgradeBudgets={} "
                 "(+{}ms post-close) pressureGuard={} (enter/exit={}MB/{}MB)",
                 cfg.hook.clearRefreshAll, cfg.hook.holdUpgradeBudgets,
                 cfg.hook.upgradeHoldAfterCloseMs, cfg.hook.pressureGuardEnabled,
                 cfg.hook.pressureGuardEnterHeadroomBytes >> 20,
                 cfg.hook.pressureGuardExitHeadroomBytes >> 20);
    spdlog::info("[config] legacy: holdDegradeBudget={} minHeadroom={}MB",
                 cfg.hook.holdDegradeBudget,
                 cfg.hook.minHeadroomForDegradeHoldBytes >> 20);
    spdlog::info("[config] safety: maxHoldMode={}  abortOnEscalation={}  maxHoldMsPerMenu={}  "
                 "watchdog={} ({}% for {}ms)",
                 cfg.hook.maxSuppressMode, cfg.hook.abortOnEscalation, cfg.hook.maxSuppressMsPerMenu,
                 cfg.hook.watchdogEnabled,
                 static_cast<int>(cfg.hook.watchdogRatio * 100.0f), cfg.hook.watchdogTripMs);

    // Detect Starfield version + load its address library bin (if present).
    // This becomes the primary resolution path for the hook target — Bethesda
    // can move CheckBudget::Func4 between patches and the same plugin DLL keeps
    // working, as long as the matching versionlib-X-Y-Z-0.bin is alongside.
    if (auto v = slh::AddressLib::DetectModuleVersion()) {
        spdlog::info("[runtime] detected Starfield version: {}", v->str());
        slh::AddressLib::LoadForVersion(*v);
    } else {
        spdlog::warn("[runtime] could not detect Starfield version — Address Library disabled");
    }

    slh::MenuWatcher::Install(cfg.menu, cfg.suspendingMenus);
    slh::MenuWatcher::Start();

    if (!slh::Hook::Install(cfg.hook)) {
        spdlog::error("Hook::Install failed — plugin will be inactive");
        return true;  // SFSE plugin still loaded (so other tests work)
    }

    spdlog::info("=== StarfieldPauseLODHold load complete ===");
    return true;
}

// DllMain runs under the loader lock, where joining threads and tearing down
// trampolines is unsafe. SFSEPlugin_Load pins this module before starting any
// worker or hook, so a non-process detach cannot occur after installation.
// At process termination Windows has already stopped the other threads; only
// lock-free counter reads and final logging remain.
BOOL WINAPI DllMain(HMODULE, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_DETACH) {
        const bool processExiting = (lpReserved != nullptr);
        spdlog::info("=== DLL_PROCESS_DETACH (process_exit={}) ===", processExiting);
        if (processExiting) {
            // Lock-free reads of the atomics — safe with the other threads gone.
            const auto c = slh::Hook::SnapshotCounters();
            spdlog::info("[hook] final — totalCalls={}, pausedStateSkips={}, postPauseResets={}, "
                         "degradeHolds={}, refreshClears={}, upgradeCaps={}, passed={}  "
                         "mode histogram[0/1/2/3]={}/{}/{}/{}",
                         c.totalCalls, c.pausedStateSkips, c.postPauseResets,
                         c.suppressedCalls, c.refreshClears,
                         c.upgradeBudgetCaps, c.passedCalls,
                         c.modeHistogram[0], c.modeHistogram[1],
                         c.modeHistogram[2], c.modeHistogram[3]);
            slh::Hook::LogFinalVerdict();
        }
        spdlog::shutdown();
    }
    return TRUE;
}
