#pragma once
#include "PCH.h"

namespace slh::Hook
{
    struct Config
    {
        // ── Resolution chain ─────────────────────────────────────────────
        std::uint64_t idCheckBudgetFunc4   = 143698;   // verified in 1.16.236
        std::uint64_t idBudgetSingletonPtr = 944397;   // verified in 1.16.236
        std::uint64_t idUpgradeMeshesFunc4   = 143703;
        std::uint64_t idUpgradeTexturesFunc4 = 143706;
        std::uint64_t idDegradeTexturesFunc4 = 143709;
        std::uint64_t idBalanceTextureLODsFunc4 = 143711;
        std::uint64_t idDegradeMeshesFunc4   = 143714;
        std::uint64_t idResetFunc3            = 143715;

        std::string sigCheckBudgetFunc4;
        std::uintptr_t checkBudgetFunc4RvaFallback = 0x02a4bfa0;

        std::size_t singletonLoadOffset    = 10;   // offset of `48 8B 05` from entry
        std::size_t singletonLoadInstrLen  = 7;    // length of that instruction

        std::uintptr_t budgetTrackerOffset = 0xf8;
        std::uintptr_t budgetModeOffset    = 0x28;

        // Budget-tracker fields, read for the VRAM timeline. CheckBudget uses
        // them as: overage = usage - budget (mode 3 guard), and
        // totalDegrade = usage - target (mode 1/2).
        std::uintptr_t trackerBudgetOffset = 0x08;
        std::uintptr_t trackerUsageOffset  = 0x10;
        std::uintptr_t trackerTargetOffset = 0x18;

        // State-struct field offsets (param_3 of CheckBudget::Func4).
        // The three upgrade fields are not quality tiers. Decompilation shows
        // +0x38a0 is consumed by UpgradeMeshes, +0x38a8 by UpgradeTextures,
        // and +0x38b0 is the combined ceiling consumed by both.
        std::uintptr_t stateDemoteActiveOffset          = 0x389c;
        std::uintptr_t stateMeshUpgradeBudgetOffset     = 0x38a0;
        std::uintptr_t stateTextureUpgradeBudgetOffset  = 0x38a8;
        std::uintptr_t stateCombinedUpgradeBudgetOffset = 0x38b0;
        std::uintptr_t stateTotalDegradeOffset          = 0x38b8;
        std::uintptr_t stateRefreshAllOffset            = 0x38c0;
        std::uintptr_t stateEmergencyDegradeOffset      = 0x38c1;

        // ── Primary intervention: exact PauseMenu state freeze ───────────
        // CheckBudget and all five native states that can change mesh/texture
        // LOD are hooked. While the engine reports PauseMenu open, each is
        // redirected through Reset::Func3. Reset is the engine's own safe transition: it clears
        // the current budgets/iterators, destroys the current state object and
        // advances to GatherMaterials. The state machine remains live, but no
        // promotion or degradation batch executes during the pause.
        bool freezeLODWhilePaused = true;

        // ── Fast quit to desktop ─────────────────────────────────────────
        // The engine's exit teardown re-serializes the entire D3D12 pipeline
        // cache (Pipeline.cache) and walks its whole heap — up to a minute on
        // a large session. TerminateProcess skips all of it and closes the
        // game instantly. Decompiled on 1.16.236: the ONLY file the teardown
        // writes is Pipeline.cache (verified across 3,804 functions), and the
        // engine already gates that write on its own "needs saving" byte. We
        // read the same byte and only fast-exit when the engine itself would
        // write nothing — so no shader cache can ever be lost. When new shaders
        // WERE compiled this session, the byte is set, we skip the fast-exit,
        // and the engine persists the cache normally that one time.
        bool fastExit = true;
        // If false, fast-exit unconditionally, even when the pipeline cache is
        // dirty (this session's new shaders recompile next launch). Off by
        // default so the smart, lossless behaviour above is what ships.
        bool fastExitEvenIfCacheDirty = false;

        // ── Teardown safety ──────────────────────────────────────────────
        // Quitting to desktop is chosen FROM the pause menu, so the menu never
        // closes, the freeze never releases, and our detours would still be
        // redirecting into Reset::Func3 while Starfield destroys the very LOD
        // subsystem those functions belong to.
        //
        // The signal is the engine's own quit-requested flag, Main::Singleton
        // + 0x28 — see MenuWatcher::EngineShutdownRequested. It is checked
        // before every redirect, freeze and release alike. There is
        // deliberately no time limit on the freeze: a pause may last as long as
        // the player wants, which is the whole point of the fix.
        //
        // Secondary, free: never redirect unless the budget singleton and
        // tracker still read cleanly. Measured on 1.16.236 this never fires
        // (the singleton stays valid to the very end), but teardown order is
        // not guaranteed to be the same on every build, so it stays as a
        // backstop that costs a couple of guarded reads.
        bool requireValidBudgetForFreeze = true;

        // ── VRAM pressure gate ───────────────────────────────────────────
        // Never hold the freeze while the engine is trying to recover VRAM.
        // Every hooked state is redirected through Reset, which also zeroes
        // totalDegrade, so a frozen engine cannot demote at all — and demotion
        // is its only route back from pressure. Blocking it costs nothing while
        // there is headroom, but under pressure each pause becomes a pause the
        // engine could not use to recover, and usage ratchets up over a session.
        //
        // Freeze only at or below this budget mode. 0 = plenty of VRAM,
        // 1 = light budgeting, 2 = real pressure, 3 = emergency.
        int maxFreezeMode = 1;
        // ...and only with at least this much room left under the engine's own
        // texture target. Mode alone is not enough: mode 1 computes
        // totalDegrade = max(usage - target, 34MB), so it can carry real
        // overage while still reporting light budgeting.
        std::uint64_t minFreezeHeadroomBytes = 128ull << 20;

        // ── Legacy intervention A: refreshAll clear (upgrade side) ───────
        // `UpgradeTextures::Func4` reads state+0x38c0 and, when it is set,
        // forces mip 0 — FULL RESOLUTION — on every material it walks.
        // CheckBudget's mode-0 branch is the only thing that sets it. That is
        // the mass-upload event behind the reported "VRAM spikes to 100% when
        // you pause" bug. Clearing it makes UpgradeTextures fall back to its
        // normal per-material screen-coverage path, so textures return
        // gradually instead of all at once.
        //
        // This is the safe direction: over-suppressing an UPGRADE means
        // textures stay blurry a moment longer. Over-suppressing a DEMOTE (what
        // v0.4 did) blocks the engine's only way back from a VRAM emergency.
        bool          clearRefreshAll = false;
        // Cap normal texture/mesh upgrade work while the menu is open. Setting
        // the texture and combined byte budgets to zero does not skip the
        // state machine: it permits at most the current 100-material batch,
        // then makes the native state advance normally.
        bool holdUpgradeBudgets = false;
        // Keep both upgrade-side protections armed across the unpause
        // transient, when the tester reports the FPS collapse begins.
        std::uint32_t upgradeHoldAfterCloseMs = 0;

        // The tester trace also crossed into the same mode-3 stall during
        // world streaming without a pause. Native mode switching reacts after
        // a large asynchronous upload burst is already queued, so cap upgrade
        // work early when target headroom is low. Hysteresis avoids chatter.
        bool          pressureGuardEnabled = false;
        std::uint64_t pressureGuardEnterHeadroomBytes = 768ull << 20;
        std::uint64_t pressureGuardExitHeadroomBytes  = 1024ull << 20;

        // ── Intervention B: degrade hold (v0.5, demote side) ─────────────
        int  maxSuppressMode      = 1;      // never hold above this mode
        bool holdDegradeBudget    = false;  // legacy experiment; unsafe near pressure
        // Do not trade away the last reserve of VRAM merely to preserve mip
        // quality. The repeated-pause trace had only 767 MB left when the old
        // hold stayed armed and mode 3 followed three seconds later.
        std::uint64_t minHeadroomForDegradeHoldBytes = 1024ull << 20;
        bool abortOnEscalation    = true;
        std::uint32_t maxSuppressMsPerMenu = 20000;

        // ── Watchdog ─────────────────────────────────────────────────────
        bool          watchdogEnabled  = false;
        float         watchdogRatio    = 0.5f;
        std::uint32_t watchdogTripMs   = 1500;
        std::uint32_t healthSampleMs   = 250;

        // ── Repro harness ────────────────────────────────────────────────
        // Sets refreshAll = 1 on every tick, reproducing the mass full-res
        // upload on demand. This DELIBERATELY CAUSES the bug — it exists so a
        // candidate fix can be A/B tested without waiting for a natural
        // occurrence. Never ship enabled.
        bool forceRefreshAll = false;

        // ── Diagnostics ──────────────────────────────────────────────────
        // Timeline samples are recorded on the render thread into a ring and
        // drained by the diag thread, so the values are coherent and the hot
        // path never touches the log.
        //
        // The timeline is OFF by default. At 250 ms it writes about 10 MB and
        // 52,000 lines per four hours of play — right for a bug report, wrong
        // for everyone else. With it off the log still records startup, every
        // pause, the per-pause recovery report, and the periodic verdict, which
        // is what a report actually needs.
        // A diagnostic build compiles the verbose defaults in, so a tester needs
        // no INI at all — the DLL alone produces a usable log. Release builds
        // stay quiet. See SLH_DIAGNOSTIC_BUILD in CMakeLists.txt.
#ifdef SLH_DIAGNOSTIC_BUILD
        bool          logTimeline     = true;
        std::uint32_t summaryIntervalMs = 5000;
#else
        bool          logTimeline     = false;
        std::uint32_t summaryIntervalMs = 15000;
#endif
        std::uint32_t timelinePeriodMs = 250;   // heartbeat; changes log immediately
        bool   dryRun           = false;
        bool   logEveryCall     = false;
        std::uint32_t verdictIntervalSec = 60;
    };

    bool Install(const Config& cfg);
    void Shutdown();

    // Writes the "did the mass-upload path fire?" summary block. The live
    // emission path is the diag thread (periodic, and once when the engine's
    // quit flag is seen); DllMain is deliberately empty, so nothing runs at
    // process detach. Shutdown() and this function currently have no callers.
    void LogFinalVerdict();

    struct Counters
    {
        std::uint64_t totalCalls;
        std::uint64_t suppressedCalls;   // degrade holds
        std::uint64_t refreshClears;
        std::uint64_t upgradeBudgetCaps;
        std::uint64_t pausedStateSkips;
        std::uint64_t postPauseResets;
        std::uint64_t passedCalls;
        std::array<std::uint64_t, 4> modeHistogram;
    };
    Counters SnapshotCounters() noexcept;
}
