#include "PCH.h"
#include "Hook.h"
#include "MenuWatcher.h"
#include "Scanner.h"
#include "AddressLib.h"

namespace slh::Hook
{
    namespace
    {
        Config              g_cfg;
        std::uintptr_t      g_moduleBase = 0;
        std::uintptr_t      g_budgetSingletonPtr = 0;
        std::vector<std::uintptr_t> g_hookTargets;
        std::atomic<bool>   g_writesEnabled{ false };   // false unless prologue + singleton verified

        using StateFn = void(__fastcall*)(void*, void*, std::uintptr_t, void**);
        StateFn             g_origCheckBudget = nullptr;
        StateFn             g_origUpgradeMeshes = nullptr;
        StateFn             g_origUpgradeTextures = nullptr;
        StateFn             g_origDegradeTextures = nullptr;
        StateFn             g_origBalanceTextureLODs = nullptr;
        StateFn             g_origDegradeMeshes = nullptr;
        StateFn             g_resetState = nullptr;
        std::atomic<bool>   g_installed{ false };
        std::atomic<bool>   g_pauseFreezeReady{ false };

        // Counters.
        std::atomic<std::uint64_t> g_totalCalls{ 0 };
        std::atomic<std::uint64_t> g_degradeHolds{ 0 };
        std::atomic<std::uint64_t> g_refreshClears{ 0 };
        std::atomic<std::uint64_t> g_upgradeBudgetCaps{ 0 };
        std::atomic<std::uint64_t> g_refreshObserved{ 0 };   // times the engine had refreshAll set
        std::atomic<std::uint64_t> g_forcedRefresh{ 0 };
        std::atomic<std::uint64_t> g_pausedStateSkips{ 0 };
        std::atomic<std::uint64_t> g_postPauseResets{ 0 };
        std::atomic<std::uint64_t> g_passedCalls{ 0 };
        std::atomic<std::uint64_t> g_actionCalls{ 0 };       // any intervention, for the watchdog
        std::array<std::atomic<std::uint64_t>, 4> g_modeHistogram{};

        // ── Per-menu-session arm / latch state (degrade hold only) ────────
        enum class Latch : int { None = 0, ModeEscalation, LowHeadroom, TimeCap, Watchdog };

        const char* LatchName(int l) noexcept
        {
            switch (static_cast<Latch>(l)) {
                case Latch::ModeEscalation: return "mode-escalation";
                case Latch::LowHeadroom:    return "low-headroom";
                case Latch::TimeCap:        return "time-cap";
                case Latch::Watchdog:       return "watchdog";
                default:                    return "none";
            }
        }

        std::atomic<std::uint64_t>  g_armedSession{ 0 };
        std::atomic<int>            g_latch{ 0 };
        std::atomic<std::int64_t>   g_sessionStartMs{ 0 };
        std::atomic<bool>           g_loggedLatchThisSession{ false };

        std::atomic<std::uint64_t> g_teardownSkips{ 0 };      // redirects declined
        std::atomic<std::uint64_t> g_pressureStandDowns{ 0 }; // declined for VRAM pressure
        std::atomic<std::uint64_t> g_freezeStoodDownSession{ 0 };

        std::atomic<bool>   g_watchdogTripped{ false };
        std::atomic<double> g_baselineRate{ 0.0 };
        std::atomic<bool>   g_pressureGuardActive{ false };
        std::atomic<std::uint64_t> g_loggedFreezeSession{ 0 };
        std::atomic<std::uint64_t> g_pendingPostPauseReset{ 0 };
        std::atomic<std::uint64_t> g_completedPostPauseReset{ 0 };

        // The diag thread is started DETACHED and no std::thread object is
        // kept. A static std::thread that is still joinable when the CRT runs
        // static destructors at DLL_PROCESS_DETACH calls std::terminate — and
        // since the dormancy change the thread FINISHES on quit rather than
        // running forever, which is exactly the joinable-but-done state that
        // trips it. Starfield's usual TerminateProcess exit skips detach, but
        // any ExitProcess-based exit (patch, another plugin, CRT exit()) would
        // have aborted the game during shutdown and looked like our crash.
        std::atomic<bool>  g_shutdown{ false };

        std::int64_t NowMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        template <typename T>
        bool TryRead(std::uintptr_t addr, T& out) noexcept
        {
            __try { out = *reinterpret_cast<const T*>(addr); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool TryWriteU8(std::uintptr_t addr, std::uint8_t v) noexcept
        {
            __try { *reinterpret_cast<std::uint8_t*>(addr) = v; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool TryWriteU64(std::uintptr_t addr, std::uint64_t v) noexcept
        {
            __try { *reinterpret_cast<std::uint64_t*>(addr) = v; return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // One walk of singleton → tracker, returning the mode byte and the
        // three VRAM accounting fields CheckBudget derives everything from.
        struct BudgetRead
        {
            bool          valid = false;
            int           mode = -1;
            std::uint64_t budget = 0, usage = 0, target = 0;
        };

        BudgetRead ReadBudget() noexcept
        {
            BudgetRead b{};
            if (g_budgetSingletonPtr == 0) return b;
            std::uintptr_t singleton = 0;
            if (!TryRead(g_budgetSingletonPtr, singleton) || singleton == 0) return b;
            std::uintptr_t tracker = 0;
            if (!TryRead(singleton + g_cfg.budgetTrackerOffset, tracker) || tracker == 0) return b;

            std::uint8_t mode = 0;
            if (!TryRead(tracker + g_cfg.budgetModeOffset, mode)) return b;
            b.mode = mode;
            bool ok = true;
            ok &= TryRead(tracker + g_cfg.trackerBudgetOffset, b.budget);
            ok &= TryRead(tracker + g_cfg.trackerUsageOffset,  b.usage);
            ok &= TryRead(tracker + g_cfg.trackerTargetOffset, b.target);
            b.valid = ok;
            return b;
        }

        // Pipeline-cache "needs saving" flag, off the same budget singleton.
        // Chain verified on 1.16.236 (FUN_142a1c690, the exit teardown):
        //   singleton = *g_budgetSingletonPtr           (AddrLib 944397)
        //   renderer  = *(singleton + 0x30)
        //   pipelineMgr = *(renderer + 0x560)
        //   dirty     = *(pipelineMgr + 0x10)
        // The engine serializes Pipeline.cache on exit iff this byte != 0.
        // Returns 0 = clean (engine would write nothing), 1 = dirty, -1 =
        // could not read. Fast-exit acts only on a clean 0 read, so a wrong
        // offset on some other build fails safe: a fault or null returns -1
        // and the game shuts down normally.
        constexpr std::uintptr_t kRendererOffset    = 0x30;
        constexpr std::uintptr_t kPipelineMgrOffset = 0x560;
        constexpr std::uintptr_t kPipelineDirtyByte = 0x10;

        int ReadPipelineCacheDirty() noexcept
        {
            if (g_budgetSingletonPtr == 0) return -1;
            std::uintptr_t singleton = 0, renderer = 0, pipelineMgr = 0;
            std::uint8_t dirty = 0;
            if (!TryRead(g_budgetSingletonPtr, singleton) || singleton == 0) return -1;
            if (!TryRead(singleton + kRendererOffset, renderer) || renderer == 0) return -1;
            if (!TryRead(renderer + kPipelineMgrOffset, pipelineMgr) || pipelineMgr == 0) return -1;
            if (!TryRead(pipelineMgr + kPipelineDirtyByte, dirty)) return -1;
            return dirty != 0 ? 1 : 0;
        }

        std::atomic<bool> g_fastExitDone{ false };

        // Called once, when the engine's quit flag is seen. Either terminates
        // the process (instant close) or returns to let the normal shutdown run.
        void MaybeFastExit() noexcept
        {
            if (!g_cfg.fastExit) return;
            if (g_fastExitDone.exchange(true)) return;   // once only

            if (!g_cfg.fastExitEvenIfCacheDirty) {
                const int dirty = ReadPipelineCacheDirty();
                if (dirty != 0) {
                    spdlog::info("[quit] fast-exit skipped this once — {}; letting the engine "
                                 "shut down normally so the shader cache persists",
                                 dirty < 0 ? "could not read the pipeline-cache state"
                                           : "new shaders were compiled this session");
                    spdlog::default_logger()->flush();
                    return;
                }
                spdlog::info("[quit] pipeline cache is clean — closing immediately "
                             "(skipping the engine's slow teardown)");
            } else {
                spdlog::info("[quit] fast exit (unconditional)");
            }
            spdlog::default_logger()->flush();
            ::TerminateProcess(::GetCurrentProcess(), 0);
        }

        // ── Timeline ring ─────────────────────────────────────────────────
        // Written on the render thread, drained and formatted by the diag
        // thread. Nothing on the hot path ever touches the logger.
        struct Sample
        {
            std::int64_t  tMs;
            std::uint64_t session;
            int           mode;
            std::uint8_t  refreshAll, emergency, demoteActive, action;
            bool          inMenu, cooldown, pressureGuard;
            std::uint64_t totalDegrade, meshUpgrade, textureUpgrade, combinedUpgrade;
            std::uint64_t budget, usage, target;
            std::uint64_t osBudget, osUsage;   // WDDM view; 0 if unavailable
        };
        enum : std::uint8_t {
            kActNone = 0,
            kActHoldDegrade = 1,
            kActClearRefresh = 2,
            kActForceRefresh = 4,
            kActCapUpgrade = 8,
            kActFrozen = 16          // heartbeat taken while the freeze holds
        };
        enum class FrozenState : std::uint8_t {
            CheckBudget = 1,
            UpgradeMeshes,
            UpgradeTextures,
            DegradeTextures,
            BalanceTextureLODs,
            DegradeMeshes
        };

        constexpr std::size_t kRingSize = 1024;
        Sample                      g_ring[kRingSize]{};
        std::atomic<std::uint64_t>  g_ringWrite{ 0 };      // next slot to claim
        std::atomic<std::uint64_t>  g_ringPublished{ 0 };  // slots finished writing
        std::uint64_t               g_ringRead = 0;        // diag thread only

        // Hook-thread-only recording state.
        std::atomic<std::uint64_t> g_lastSig{ ~0ull };
        std::atomic<std::int64_t>  g_lastSampleMs{ 0 };

        // SINGLE-PRODUCER by assumption: all six hooked states run on the
        // renderer thread (supported by ~218k observed samples with no
        // corruption, but not proven). The claim/publish split does NOT make
        // this multi-producer safe — g_ringPublished is a completion COUNT,
        // not a contiguous watermark, so with two producers the drain could
        // consume a slot the slower producer is still writing. If the
        // single-thread assumption is ever disproven, this needs per-slot
        // sequence numbers, not a bigger counter.
        void PushSample(const Sample& s) noexcept
        {
            const auto i = g_ringWrite.fetch_add(1, std::memory_order_relaxed);
            g_ring[i % kRingSize] = s;
            g_ringPublished.fetch_add(1, std::memory_order_release);
        }

        const char* ActionName(std::uint8_t a) noexcept
        {
            switch (a) {
                case kActFrozen:                          return "FROZEN";
                case kActHoldDegrade:                     return "HOLD-DEGRADE";
                case kActClearRefresh:                    return "CLEAR-REFRESH";
                case kActHoldDegrade | kActClearRefresh:  return "HOLD+CLEAR";
                case kActForceRefresh:                    return "FORCE-REFRESH(repro)";
                case kActCapUpgrade:                      return "CAP-UPGRADE";
                case kActHoldDegrade | kActCapUpgrade:    return "HOLD+CAP";
                case kActClearRefresh | kActCapUpgrade:   return "CLEAR+CAP";
                case kActHoldDegrade | kActClearRefresh |
                     kActCapUpgrade:                      return "HOLD+CLEAR+CAP";
                default:                                  return "-";
            }
        }

        const char* FrozenStateName(FrozenState state) noexcept
        {
            switch (state) {
                case FrozenState::CheckBudget:      return "CheckBudget";
                case FrozenState::UpgradeMeshes:   return "UpgradeMeshes";
                case FrozenState::UpgradeTextures: return "UpgradeTextures";
                case FrozenState::DegradeTextures: return "DegradeTextures";
                case FrozenState::BalanceTextureLODs: return "BalanceTextureLODs";
                case FrozenState::DegradeMeshes:   return "DegradeMeshes";
                default:                           return "unknown";
            }
        }

        inline double MB(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

        // ── WDDM residency telemetry ─────────────────────────────────────
        // The engine's tracker is not the OS's view. The reported symptom —
        // worse the longer you pause, only on cards below ~12 GB — is what
        // OS-side eviction of an idle process's VRAM looks like: the LOD
        // machine never sees it, but every evicted resource has to be paged
        // back in on unpause. These two numbers are the only way to tell that
        // apart from an engine-side upload. Sampled on the diag thread; the
        // render thread only copies the published values.
        std::atomic<std::uint64_t> g_osBudget{ 0 }, g_osUsage{ 0 };
        std::atomic<bool>          g_osValid{ false };

        // Diag-thread-only. dxgi.dll is loaded lazily and by name so a failure
        // anywhere in here costs nothing but the telemetry itself — no import
        // is added to the plugin and no write path depends on it.
        struct OsVramSampler
        {
            using CreateFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);

            HMODULE                     dxgi = nullptr;
            IDXGIFactory1*              factory = nullptr;
            std::vector<IDXGIAdapter3*> adapters;

            bool Init() noexcept
            {
                dxgi = ::LoadLibraryW(L"dxgi.dll");
                if (dxgi == nullptr) return false;
                auto create = reinterpret_cast<CreateFactory1Fn>(
                    ::GetProcAddress(dxgi, "CreateDXGIFactory1"));
                if (create == nullptr) return false;
                if (FAILED(create(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(&factory)))
                    || factory == nullptr) {
                    return false;
                }
                for (UINT i = 0; i < 8; ++i) {
                    IDXGIAdapter1* a1 = nullptr;
                    if (factory->EnumAdapters1(i, &a1) != S_OK || a1 == nullptr) break;
                    DXGI_ADAPTER_DESC1 desc{};
                    IDXGIAdapter3* a3 = nullptr;
                    if (SUCCEEDED(a1->QueryInterface(__uuidof(IDXGIAdapter3),
                                                     reinterpret_cast<void**>(&a3)))
                        && a3 != nullptr) {
                        adapters.push_back(a3);
                        if (SUCCEEDED(a1->GetDesc1(&desc))) {
                            spdlog::info("[osvram] adapter {}: dedicated={:.0f}MB shared={:.0f}MB "
                                         "luid={:08x}:{:08x}",
                                         i, MB(desc.DedicatedVideoMemory),
                                         MB(desc.SharedSystemMemory),
                                         static_cast<std::uint32_t>(desc.AdapterLuid.HighPart),
                                         desc.AdapterLuid.LowPart);
                        }
                    }
                    a1->Release();
                }
                return !adapters.empty();
            }

            // Which adapter Starfield renders on is not knowable from here, so
            // take the one currently holding the most local video memory —
            // on any rig running the game that is the game's GPU.
            void Sample() noexcept
            {
                std::uint64_t bestUsage = 0, bestBudget = 0;
                bool any = false;
                for (auto* a : adapters) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
                    if (FAILED(a->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                        continue;
                    }
                    any = true;
                    if (info.CurrentUsage >= bestUsage) {
                        bestUsage  = info.CurrentUsage;
                        bestBudget = info.Budget;
                    }
                }
                g_osUsage.store(bestUsage, std::memory_order_relaxed);
                g_osBudget.store(bestBudget, std::memory_order_relaxed);
                g_osValid.store(any, std::memory_order_relaxed);
            }

            void Release() noexcept
            {
                for (auto* a : adapters) a->Release();
                adapters.clear();
                if (factory) { factory->Release(); factory = nullptr; }
                if (dxgi)    { ::FreeLibrary(dxgi); dxgi = nullptr; }
                g_osValid.store(false, std::memory_order_relaxed);
            }
        };

        // ── SEH-guarded native transition ────────────────────────────────
        // Every freeze target is resolved by Address Library id and validated
        // only shallowly, so a future patch could point g_resetState at the
        // wrong code. A fault here would otherwise be a crash in exactly the
        // scenario under test, and would be blamed on the bug. Factored out
        // because MSVC forbids __try in a frame that needs unwinding.
        std::atomic<bool> g_resetFaulted{ false };

        bool CallResetState(void* self, void* a2, std::uintptr_t state,
                            void** outNextState) noexcept
        {
            __try {
                g_resetState(self, a2, state, outNextState);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Fail to native for the rest of the session and never retry:
                // the likeliest cause is a mis-resolved address, in which case
                // nothing was modified and the original state function is the
                // correct thing to run instead.
                //
                // Known tradeoff: if Reset faulted PARTWAY (some fields already
                // zeroed, state possibly destroyed), the caller falls back to
                // the original Func4 in the same visit, which then runs against
                // half-reset state. The alternative — skipping the visit — is
                // worse: nothing would write *outNextState and the dispatcher
                // would consume garbage. The original at least leaves the
                // machine coherent, and the freeze is permanently off either
                // way.
                g_pauseFreezeReady.store(false, std::memory_order_release);
                g_resetFaulted.store(true, std::memory_order_relaxed);
                return false;
            }
        }

        std::uint64_t Headroom(const BudgetRead& bud) noexcept
        {
            if (!bud.valid || bud.target == 0 || bud.usage >= bud.target) return 0;
            return bud.target - bud.usage;
        }

        bool UpdatePressureGuard(const BudgetRead& bud) noexcept
        {
            if (!g_cfg.pressureGuardEnabled || !bud.valid || bud.target == 0) {
                g_pressureGuardActive.store(false, std::memory_order_relaxed);
                return false;
            }

            const std::uint64_t headroom = Headroom(bud);
            const bool wasActive = g_pressureGuardActive.load(std::memory_order_relaxed);
            const bool active = wasActive
                ? headroom < g_cfg.pressureGuardExitHeadroomBytes
                : headroom <= g_cfg.pressureGuardEnterHeadroomBytes;
            g_pressureGuardActive.store(active, std::memory_order_relaxed);
            return active;
        }

        // ── Exact PauseMenu LOD freeze ───────────────────────────────────
        // Reset::Func3 is a native state transition, not an arbitrary vtable
        // overwrite. It clears the LOD budgets and iterators, destroys the
        // currently active state through its virtual destructor, and advances
        // to GatherMaterials. Redirecting CheckBudget plus every state that
        // can change resident mesh/texture detail keeps the dispatcher alive
        // while executing zero promote/demote batches during PauseMenu.
        bool RedirectLODStateAtPauseBoundary(void* self, void* a2, std::uintptr_t state,
                                             void** outNextState,
                                             FrozenState frozen) noexcept
        {
            if (!g_cfg.freezeLODWhilePaused || g_cfg.dryRun
                || !g_pauseFreezeReady.load(std::memory_order_acquire)
                || g_resetState == nullptr) {
                return false;
            }

            // The engine's own quit-requested flag. Quit-to-desktop is chosen
            // from inside the pause menu, so the menu never closes and every
            // heuristic still says "keep freezing" while the engine dismantles
            // the LOD subsystem underneath us. This is the one signal that is
            // both universal (all quit routes set it) and early (set on
            // request, not on teardown). Guards the freeze and the release
            // alike, since both make the same native Reset call.
            if (MenuWatcher::EngineShutdownRequested()) {
                g_teardownSkips.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            // Both loads are acquire, and MenuWatcher publishes openMs, then
            // the session id, then the OPEN flag. Observing session N therefore
            // implies observing openMs(N): a visit at the OPEN edge can never
            // pair the new session with the previous session's close timestamp,
            // which would satisfy the short-pause catch below and consume the
            // one-shot release before the pause it belongs to has even ended.
            const bool pauseOpen = MenuWatcher::IsExactPauseMenuOpen();
            const auto session = MenuWatcher::SessionId();
            if (pauseOpen) {
                const std::int64_t now = NowMs();

                const BudgetRead bud = ReadBudget();

                // ── Teardown guard: is the LOD subsystem still there? ──
                // Quit-to-desktop is chosen from inside the pause menu, so the
                // menu never closes and this branch keeps running while the
                // engine dismantles itself. Redirecting into a native state
                // transition at that point is a use-after-free waiting to
                // happen. The budget singleton dies with the renderer, so if it
                // no longer reads, hand control straight back to the engine.
                if (g_cfg.requireValidBudgetForFreeze && (!bud.valid || state == 0)) {
                    g_teardownSkips.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }

                // ── VRAM pressure gate ────────────────────────────────────
                // Demotion is the engine's ONLY route back from VRAM pressure,
                // and the freeze blocks it along with everything else — every
                // hooked state is redirected to Reset, which also zeroes
                // totalDegrade, so a paused engine cannot shed a single byte.
                //
                // That is free while there is headroom and harmful once there
                // is not: each pause taken under pressure is a pause the engine
                // could not use to recover, so usage ratchets up across a
                // session instead of oscillating. Once this engine reaches
                // budget mode 3 it has never been observed to return to 1.
                // A tester reported exactly that shape — good at first, steadily
                // worse over two hours of play.
                //
                // So the freeze now only runs while the engine is comfortable,
                // and stands down for the REST OF THIS PAUSE the moment it is
                // not. Latched rather than re-evaluated so we cannot oscillate:
                // freeze, engine sheds, headroom returns, freeze again.
                if (g_freezeStoodDownSession.load(std::memory_order_relaxed) == session) {
                    // Count once per LOD cycle (the CheckBudget visit), not
                    // once per hooked visit — the verdict line is read by
                    // humans and a per-visit count reads as tens of thousands
                    // for one long pause.
                    if (frozen == FrozenState::CheckBudget) {
                        g_pressureStandDowns.fetch_add(1, std::memory_order_relaxed);
                    }
                    return false;
                }
                // minFreezeHeadroomBytes == 0 disables the headroom half
                // outright (documented in the INI); the mode check remains.
                const bool underPressure =
                    bud.mode > g_cfg.maxFreezeMode
                    || (g_cfg.minFreezeHeadroomBytes != 0 && bud.target != 0
                        && bud.usage + g_cfg.minFreezeHeadroomBytes > bud.target);
                if (underPressure) {
                    if (frozen == FrozenState::CheckBudget) {
                        g_pressureStandDowns.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (session != 0
                        && g_freezeStoodDownSession.exchange(session, std::memory_order_relaxed)
                           != session) {
                        // A stood-down pause must NOT get the one-shot RELEASE
                        // Reset at close: the engine is running natively and
                        // possibly mid-demote, and that Reset would zero
                        // totalDegrade — aborting the recovery this stand-down
                        // exists to allow. Clear any pending release from
                        // frozen visits earlier in this same pause, and the
                        // post-close catch below also skips latched sessions.
                        g_pendingPostPauseReset.store(0, std::memory_order_relaxed);
                        Sample s{};
                        s.tMs = now;
                        s.session = session;
                        s.mode = bud.mode;
                        s.refreshAll = 0xFB;   // pressure stand-down marker
                        s.inMenu = true;
                        s.budget = bud.budget;
                        s.usage = bud.usage;
                        s.target = bud.target;
                        PushSample(s);
                    }
                    return false;
                }

                g_pendingPostPauseReset.store(session, std::memory_order_relaxed);
                g_pausedStateSkips.fetch_add(1, std::memory_order_relaxed);
                if (frozen == FrozenState::CheckBudget) {
                    g_actionCalls.fetch_add(1, std::memory_order_relaxed);
                }

                if (session != 0
                    && g_loggedFreezeSession.exchange(session, std::memory_order_relaxed) != session) {
                    Sample marker{};
                    marker.tMs = now;
                    marker.session = session;
                    marker.mode = bud.mode;
                    marker.refreshAll = 0xFD;  // exact-freeze marker
                    marker.action = static_cast<std::uint8_t>(frozen);
                    marker.inMenu = true;
                    marker.budget = bud.budget;
                    marker.usage = bud.usage;
                    marker.target = bud.target;
                    marker.osBudget = g_osBudget.load(std::memory_order_relaxed);
                    marker.osUsage  = g_osUsage.load(std::memory_order_relaxed);
                    PushSample(marker);
                    g_lastSampleMs.store(now, std::memory_order_relaxed);
                }
                // Heartbeat while frozen. Until now the pause interval was the
                // one thing the log could not see — and pause LENGTH is the
                // reported bug's strongest predictor. Gated on the same
                // timeline period as gameplay sampling so the ~150 frozen
                // visits/s cannot lap the ring between drains.
                else if (g_cfg.logTimeline
                         && (now - g_lastSampleMs.load(std::memory_order_relaxed))
                            >= static_cast<std::int64_t>(g_cfg.timelinePeriodMs)) {
                    g_lastSampleMs.store(now, std::memory_order_relaxed);
                    Sample s{};
                    s.tMs = now;
                    s.session = session;
                    s.mode = bud.mode;
                    s.action = kActFrozen;
                    s.inMenu = true;
                    s.budget = bud.budget; s.usage = bud.usage; s.target = bud.target;
                    s.osBudget = g_osBudget.load(std::memory_order_relaxed);
                    s.osUsage  = g_osUsage.load(std::memory_order_relaxed);
                    TryRead(state + g_cfg.stateTotalDegradeOffset, s.totalDegrade);
                    TryRead(state + g_cfg.stateMeshUpgradeBudgetOffset, s.meshUpgrade);
                    TryRead(state + g_cfg.stateTextureUpgradeBudgetOffset, s.textureUpgrade);
                    TryRead(state + g_cfg.stateCombinedUpgradeBudgetOffset, s.combinedUpgrade);
                    PushSample(s);
                }

                return CallResetState(self, a2, state, outNextState);
            }

            // A pause may end while GatherMaterials/SortLODs holds data sampled
            // during the stationary pause. Discard that work exactly once on
            // the first hooked state after close, then let a fresh gameplay
            // cycle run normally. This is a boundary reset, not a cooldown.
            auto pending = g_pendingPostPauseReset.load(std::memory_order_relaxed);
            const auto openedAt = MenuWatcher::LastOpenTimeMs();
            const auto closedAt = MenuWatcher::LastCloseTimeMs();
            if (session != 0 && closedAt >= openedAt && closedAt != 0 && pending != session
                && g_freezeStoodDownSession.load(std::memory_order_relaxed) != session) {
                // Covers a very short pause that opens and closes entirely
                // between two state-action visits. Stood-down sessions are
                // excluded: the engine ran natively through them (possibly
                // mid-demote), so there is no pause-sampled work to discard and
                // a Reset here would zero totalDegrade during recovery.
                pending = session;
                g_pendingPostPauseReset.store(pending, std::memory_order_relaxed);
            }
            // The release Reset is the same native call as the freeze, so it
            // needs the same teardown guard. A pause that ends because the
            // process is exiting must not get one.
            if (g_cfg.requireValidBudgetForFreeze && pending != 0) {
                const BudgetRead probe = ReadBudget();
                if (!probe.valid || state == 0) {
                    g_teardownSkips.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
            }

            auto completed = g_completedPostPauseReset.load(std::memory_order_relaxed);
            if (pending != 0 && completed != pending
                && g_completedPostPauseReset.compare_exchange_strong(
                    completed, pending, std::memory_order_relaxed)) {
                g_postPauseResets.fetch_add(1, std::memory_order_relaxed);
                if (frozen == FrozenState::CheckBudget) {
                    g_actionCalls.fetch_add(1, std::memory_order_relaxed);
                }

                const BudgetRead bud = ReadBudget();
                Sample marker{};
                marker.tMs = NowMs();
                marker.session = pending;
                marker.mode = bud.mode;
                marker.refreshAll = 0xFC;  // one-shot post-pause reset marker
                marker.action = static_cast<std::uint8_t>(frozen);
                marker.budget = bud.budget;
                marker.usage = bud.usage;
                marker.target = bud.target;
                marker.osBudget = g_osBudget.load(std::memory_order_relaxed);
                marker.osUsage  = g_osUsage.load(std::memory_order_relaxed);
                PushSample(marker);

                return CallResetState(self, a2, state, outNextState);
            }

            return false;
        }

        void __fastcall Hooked_UpgradeMeshes(void* self, void* a2,
                                             std::uintptr_t state, void** outNextState)
        {
            if (!RedirectLODStateAtPauseBoundary(self, a2, state, outNextState,
                                                 FrozenState::UpgradeMeshes)) {
                g_origUpgradeMeshes(self, a2, state, outNextState);
            }
        }

        void __fastcall Hooked_UpgradeTextures(void* self, void* a2,
                                               std::uintptr_t state, void** outNextState)
        {
            if (!RedirectLODStateAtPauseBoundary(self, a2, state, outNextState,
                                                 FrozenState::UpgradeTextures)) {
                g_origUpgradeTextures(self, a2, state, outNextState);
            }
        }

        void __fastcall Hooked_DegradeTextures(void* self, void* a2,
                                               std::uintptr_t state, void** outNextState)
        {
            if (!RedirectLODStateAtPauseBoundary(self, a2, state, outNextState,
                                                 FrozenState::DegradeTextures)) {
                g_origDegradeTextures(self, a2, state, outNextState);
            }
        }

        void __fastcall Hooked_BalanceTextureLODs(void* self, void* a2,
                                                  std::uintptr_t state, void** outNextState)
        {
            if (!RedirectLODStateAtPauseBoundary(self, a2, state, outNextState,
                                                 FrozenState::BalanceTextureLODs)) {
                g_origBalanceTextureLODs(self, a2, state, outNextState);
            }
        }

        void __fastcall Hooked_DegradeMeshes(void* self, void* a2,
                                             std::uintptr_t state, void** outNextState)
        {
            if (!RedirectLODStateAtPauseBoundary(self, a2, state, outNextState,
                                                 FrozenState::DegradeMeshes)) {
                g_origDegradeMeshes(self, a2, state, outNextState);
            }
        }

        // ── The hook ──────────────────────────────────────────────────────
        //
        // CheckBudget::Func4 (1.16.236) computes this tick's LOD budgets from
        // the VRAM tracker and picks the next state:
        //
        //   mode 0     : budgets = full, totalDegrade = 0, refreshAll = 1
        //   mode 1 / 2 : budgets clamped from overage; totalDegrade =
        //                max(usage - target, 34 MB floor); refreshAll cleared
        //   mode 3 (+overage guard) : emergencyDegrade = 1, totalDegrade = overage
        //
        // The v0.6 trace proved refreshAll never becomes nonzero on the
        // affected machine. Ordinary UpgradeMeshes/UpgradeTextures work
        // remains live through +0x38a0/+0x38a8/+0x38b0 while usage climbs.
        // Zeroing the texture and combined budgets lets the native state
        // finish its current <=100-material batch, then advance normally.
        //
        // totalDegrade (0x38b8) is the DegradeTextures byte budget and its exit
        // condition, and the gate BalanceTextureLODs uses to choose Reset vs
        // DegradeMeshes. We zero it only at mode 1, where the engine's own
        // floor makes it demote without real pressure.
        void __fastcall Hooked_CheckBudget(void* self, void* a2,
                                          std::uintptr_t state, void** outNextState)
        {
            g_totalCalls.fetch_add(1, std::memory_order_relaxed);

            const BudgetRead bud = ReadBudget();
            if (bud.mode >= 0 && bud.mode <= 3) {
                g_modeHistogram[bud.mode].fetch_add(1, std::memory_order_relaxed);
            }

            const bool          inMenu  = MenuWatcher::IsSuspendingMenuOpen();
            const std::uint64_t session = MenuWatcher::SessionId();
            const std::int64_t  now     = NowMs();

            if (RedirectLODStateAtPauseBoundary(
                    self, a2, state, outNextState, FrozenState::CheckBudget)) {
                return;
            }

            // Run the engine's CheckBudget: it computes the budgets, picks the
            // next state and enters it. Skipping it deadlocks the dispatcher.
            g_origCheckBudget(self, a2, state, outNextState);

            // The reported spike straddles pause AND unpause, so upgrade-side
            // protection stays armed briefly after the menu closes.
            const std::int64_t closedAt = MenuWatcher::LastCloseTimeMs();
            const bool cooldown = !inMenu && closedAt > 0 && g_cfg.upgradeHoldAfterCloseMs > 0
                && (now - closedAt) < static_cast<std::int64_t>(g_cfg.upgradeHoldAfterCloseMs);
            const bool pressureGuard = UpdatePressureGuard(bud);

            std::uint8_t refreshAll = 0, emergency = 0, demoteActive = 0;
            std::uint64_t meshUpgrade = 0, textureUpgrade = 0, combinedUpgrade = 0;
            TryRead(state + g_cfg.stateRefreshAllOffset, refreshAll);
            TryRead(state + g_cfg.stateEmergencyDegradeOffset, emergency);
            TryRead(state + g_cfg.stateDemoteActiveOffset, demoteActive);
            TryRead(state + g_cfg.stateMeshUpgradeBudgetOffset, meshUpgrade);
            TryRead(state + g_cfg.stateTextureUpgradeBudgetOffset, textureUpgrade);
            TryRead(state + g_cfg.stateCombinedUpgradeBudgetOffset, combinedUpgrade);

            if (refreshAll != 0) {
                g_refreshObserved.fetch_add(1, std::memory_order_relaxed);
            }

            std::uint8_t action = kActNone;
            const bool writable = state != 0 && g_writesEnabled.load(std::memory_order_acquire)
                && !g_cfg.dryRun;

            // ── Repro harness ────────────────────────────────────────────
            // Deliberately causes the bug so a fix can be A/B tested.
            if (g_cfg.forceRefreshAll) {
                if (writable && TryWriteU8(state + g_cfg.stateRefreshAllOffset, 1)) {
                    action |= kActForceRefresh;
                    g_forcedRefresh.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // ── Intervention A1: kill the mass full-res upload ───────────
            else if (g_cfg.clearRefreshAll && refreshAll != 0
                     && (inMenu || cooldown || pressureGuard)) {
                if (writable && TryWriteU8(state + g_cfg.stateRefreshAllOffset, 0)) {
                    action |= kActClearRefresh;
                    g_refreshClears.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // ── Intervention A2: cap ordinary upgrade work ───────────────
            // UpgradeMeshes and UpgradeTextures check these byte budgets
            // after each <=100-material visit. CheckBudget refreshes them on
            // every cycle. Zeroing both after CheckBudget therefore caps the
            // next state visit without skipping or deadlocking the dispatcher.
            const bool upgradeWindow = inMenu || cooldown || pressureGuard;
            const bool watchdogAllowsUpgrade =
                !inMenu || !g_watchdogTripped.load(std::memory_order_relaxed) || pressureGuard;
            if (g_cfg.holdUpgradeBudgets && upgradeWindow && watchdogAllowsUpgrade
                && (textureUpgrade != 0 || combinedUpgrade != 0)) {
                const bool textureOk = writable
                    && TryWriteU64(state + g_cfg.stateTextureUpgradeBudgetOffset, 0);
                const bool combinedOk = writable
                    && TryWriteU64(state + g_cfg.stateCombinedUpgradeBudgetOffset, 0);
                if (textureOk && combinedOk) {
                    action |= kActCapUpgrade;
                    g_upgradeBudgetCaps.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // ── Intervention B: degrade hold, menu-scoped and mode-gated ──
            if (g_cfg.holdDegradeBudget && inMenu) {
                if (g_armedSession.load(std::memory_order_relaxed) != session) {
                    g_armedSession.store(session, std::memory_order_relaxed);
                    g_latch.store(static_cast<int>(Latch::None), std::memory_order_relaxed);
                    g_sessionStartMs.store(now, std::memory_order_relaxed);
                    g_watchdogTripped.store(false, std::memory_order_relaxed);
                    g_loggedLatchThisSession.store(false, std::memory_order_relaxed);
                }

                int latch = g_latch.load(std::memory_order_relaxed);
                if (latch == static_cast<int>(Latch::None)) {
                    if (g_cfg.abortOnEscalation && bud.mode > g_cfg.maxSuppressMode) {
                        latch = static_cast<int>(Latch::ModeEscalation);
                    } else if (bud.valid && g_cfg.minHeadroomForDegradeHoldBytes > 0
                               && Headroom(bud) < g_cfg.minHeadroomForDegradeHoldBytes) {
                        latch = static_cast<int>(Latch::LowHeadroom);
                    } else if (g_cfg.watchdogEnabled && g_watchdogTripped.load(std::memory_order_relaxed)) {
                        latch = static_cast<int>(Latch::Watchdog);
                    } else if (g_cfg.maxSuppressMsPerMenu != 0
                               && (now - g_sessionStartMs.load(std::memory_order_relaxed))
                                  >= static_cast<std::int64_t>(g_cfg.maxSuppressMsPerMenu)) {
                        latch = static_cast<int>(Latch::TimeCap);
                    }
                    if (latch != static_cast<int>(Latch::None)) {
                        g_latch.store(latch, std::memory_order_relaxed);
                    }
                }

                if (latch == static_cast<int>(Latch::None)) {
                    if (bud.mode >= 1 && bud.mode <= g_cfg.maxSuppressMode) {
                        if (writable && TryWriteU64(state + g_cfg.stateTotalDegradeOffset, 0)) {
                            action |= kActHoldDegrade;
                            g_degradeHolds.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                } else if (!g_loggedLatchThisSession.exchange(true, std::memory_order_relaxed)) {
                    // Cheap enough: once per menu session, off the hot path in
                    // practice because the exchange gates it.
                    Sample s{};
                    s.tMs = now; s.session = session;
                    s.mode = bud.mode; s.refreshAll = 0xFE;  // marker row
                    s.action = static_cast<std::uint8_t>(latch);
                    s.inMenu = true;
                    PushSample(s);
                }
            }

            if (action != kActNone) {
                g_actionCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_passedCalls.fetch_add(1, std::memory_order_relaxed);
            }

            // ── Timeline sampling: on change, or on a heartbeat ───────────
            if (!g_cfg.logTimeline) return;

            const std::uint64_t sig =
                  (static_cast<std::uint64_t>(bud.mode + 1) & 0xff)
                | (static_cast<std::uint64_t>(refreshAll ? 1 : 0) << 8)
                | (static_cast<std::uint64_t>(emergency ? 1 : 0) << 9)
                | (static_cast<std::uint64_t>(demoteActive ? 1 : 0) << 10)
                | (static_cast<std::uint64_t>(inMenu ? 1 : 0) << 11)
                | (static_cast<std::uint64_t>(cooldown ? 1 : 0) << 12)
                | (static_cast<std::uint64_t>(pressureGuard ? 1 : 0) << 13)
                | (static_cast<std::uint64_t>(action) << 16);

            const bool changed = sig != g_lastSig.load(std::memory_order_relaxed);
            const bool heartbeat =
                (now - g_lastSampleMs.load(std::memory_order_relaxed))
                >= static_cast<std::int64_t>(g_cfg.timelinePeriodMs);
            if (!changed && !heartbeat) return;

            g_lastSig.store(sig, std::memory_order_relaxed);
            g_lastSampleMs.store(now, std::memory_order_relaxed);

            Sample s{};
            s.tMs = now;
            s.session = session;
            s.mode = bud.mode;
            s.refreshAll = refreshAll;
            s.emergency = emergency;
            s.demoteActive = demoteActive;
            s.action = action;
            s.inMenu = inMenu;
            s.cooldown = cooldown;
            s.pressureGuard = pressureGuard;
            s.budget = bud.budget; s.usage = bud.usage; s.target = bud.target;
            s.osBudget = g_osBudget.load(std::memory_order_relaxed);
            s.osUsage  = g_osUsage.load(std::memory_order_relaxed);
            TryRead(state + g_cfg.stateTotalDegradeOffset, s.totalDegrade);
            TryRead(state + g_cfg.stateMeshUpgradeBudgetOffset, s.meshUpgrade);
            TryRead(state + g_cfg.stateTextureUpgradeBudgetOffset, s.textureUpgrade);
            TryRead(state + g_cfg.stateCombinedUpgradeBudgetOffset, s.combinedUpgrade);
            PushSample(s);
        }

        // "os=used/budget" is the WDDM view, blank when DXGI is unavailable.
        std::string OsVram(const Sample& s)
        {
            if (s.osBudget == 0 && s.osUsage == 0) return {};
            return fmt::format(" | os vram used/budget={:.0f}/{:.0f}MB",
                               MB(s.osUsage), MB(s.osBudget));
        }

        void DrainTimeline()
        {
            auto w = g_ringPublished.load(std::memory_order_acquire);
            if (w - g_ringRead > kRingSize) {
                spdlog::warn("[timeline] dropped {} samples (ring lapped)", w - g_ringRead - kRingSize);
                g_ringRead = w - kRingSize;
            }
            while (g_ringRead < w) {
                // Re-check for lapping every iteration, not just at entry:
                // each drained line is a synchronous flush (flush_on(info)),
                // so this loop's duration scales with disk latency, and a
                // fast producer can wrap the ring while we are mid-drain.
                // Copying a slot the producer has re-claimed would emit a
                // torn, plausible-looking row.
                const auto claimed = g_ringWrite.load(std::memory_order_relaxed);
                if (claimed - g_ringRead >= kRingSize) {
                    spdlog::warn("[timeline] producer lapped the drain mid-loop — skipping {} "
                                 "samples to resynchronise", w - g_ringRead);
                    g_ringRead = w;
                    break;
                }
                const Sample s = g_ring[g_ringRead % kRingSize];
                ++g_ringRead;

                if (s.refreshAll == 0xFC) {   // one-shot release reset marker
                    spdlog::info("[freeze] RELEASE session={} — discarded pause-sampled work at {} "
                                 "with one native Reset | mode={} vram usage/target={:.0f}/{:.0f}MB{}",
                                 s.session,
                                 FrozenStateName(static_cast<FrozenState>(s.action)),
                                 s.mode, MB(s.usage), MB(s.target), OsVram(s));
                    continue;
                }

                if (s.refreshAll == 0xFD) {   // exact PauseMenu freeze marker
                    spdlog::info("[freeze] ACTIVE session={} — skipped {} and used native Reset "
                                 "| mode={} vram usage/target={:.0f}/{:.0f}MB{}",
                                 s.session,
                                 FrozenStateName(static_cast<FrozenState>(s.action)),
                                 s.mode, MB(s.usage), MB(s.target), OsVram(s));
                    continue;
                }

                if (s.refreshAll == 0xFB) {   // VRAM pressure stand-down
                    spdlog::warn("[freeze] STAND DOWN session={} — VRAM pressure (mode={} "
                                 "usage={:.0f}MB target={:.0f}MB). The engine needs to shed "
                                 "textures and demotion is its only way to do that, so the "
                                 "freeze is off for the rest of this pause.",
                                 s.session, s.mode, MB(s.usage), MB(s.target));
                    continue;
                }

                if (s.refreshAll == 0xFE) {   // latch marker row
                    spdlog::info("[hold] STAND DOWN ({}) mode={} — degrade hold off for the rest of this menu",
                                 LatchName(s.action), s.mode);
                    continue;
                }

                // The line that answers the question. Make it greppable.
                if (s.refreshAll != 0
                    || (s.action & (kActClearRefresh | kActForceRefresh | kActCapUpgrade))) {
                    spdlog::warn("[EVENT] refreshAll={} mode={} menu={} cooldown={} pressure={} action={} "
                                 "| vram usage={:.0f}MB budget={:.0f}MB target={:.0f}MB "
                                 "| upgrade mesh/texture/combined={:.0f}/{:.0f}/{:.0f}MB "
                                 "totalDegrade={:.0f}MB{}",
                                 s.refreshAll, s.mode, s.inMenu, s.cooldown, s.pressureGuard,
                                 ActionName(s.action),
                                 MB(s.usage), MB(s.budget), MB(s.target),
                                 MB(s.meshUpgrade), MB(s.textureUpgrade), MB(s.combinedUpgrade),
                                 MB(s.totalDegrade), OsVram(s));
                } else {
                    spdlog::info("[tl] mode={} menu={} cd={} pressure={} act={} "
                                 "refreshAll={} emerg={} demote={} "
                                 "| vram use/budget/target={:.0f}/{:.0f}/{:.0f}MB "
                                 "| upgrade mesh/texture/combined={:.0f}/{:.0f}/{:.0f}MB "
                                 "degrade={:.0f}MB{}",
                                 s.mode, s.inMenu, s.cooldown, s.pressureGuard,
                                 ActionName(s.action),
                                 s.refreshAll, s.emergency, s.demoteActive,
                                 MB(s.usage), MB(s.budget), MB(s.target),
                                 MB(s.meshUpgrade), MB(s.textureUpgrade), MB(s.combinedUpgrade),
                                 MB(s.totalDegrade), OsVram(s));
                }
            }
        }

        void LogVerdict(const char* trigger)
        {
            spdlog::info("──────────── VERDICT ({}) ────────────", trigger);
            const auto obs   = g_refreshObserved.load();
            const auto clr   = g_refreshClears.load();
            const auto caps  = g_upgradeBudgetCaps.load();
            const auto holds = g_degradeHolds.load();
            const auto forced= g_forcedRefresh.load();
            const auto frozen= g_pausedStateSkips.load();
            const auto releases = g_postPauseResets.load();
            const auto m0    = g_modeHistogram[0].load();

            spdlog::info("[verdict] budget-mode 0 ticks observed : {}", m0);
            spdlog::info("[verdict] paused LOD state skips       : {}", frozen);
            spdlog::info("[verdict] one-shot post-pause resets  : {}", releases);
            spdlog::info("[verdict] refreshAll seen SET by engine: {}  (cleared by us: {})", obs, clr);
            spdlog::info("[verdict] upgrade-budget caps          : {}", caps);
            spdlog::info("[verdict] degrade holds (mode 1, menu) : {}", holds);
            const auto pressure = g_pressureStandDowns.load();
            spdlog::info("[verdict] LOD cycles under pressure stand-down: {}{}", pressure,
                         pressure ? "  — the engine was near or over its texture target while "
                                    "paused, so the freeze got out of the way and let it demote" : "");
            const auto teardown = g_teardownSkips.load();
            if (teardown != 0) {
                spdlog::info("[verdict] redirects declined (teardown): {}  — the LOD subsystem "
                             "stopped reading cleanly, so control was handed back to the engine "
                             "instead of calling into it. This is the guard against hanging or "
                             "crashing on Quit to Desktop.", teardown);
            }
            if (g_osValid.load(std::memory_order_relaxed)) {
                spdlog::info("[verdict] OS (WDDM) vram used/budget    : {:.0f}/{:.0f}MB",
                             MB(g_osUsage.load(std::memory_order_relaxed)),
                             MB(g_osBudget.load(std::memory_order_relaxed)));
            }
            if (g_resetFaulted.load(std::memory_order_relaxed)) {
                spdlog::error("[verdict] the native Reset transition FAULTED — the freeze was "
                              "disabled for the rest of this session and every state ran "
                              "natively. The address library almost certainly does not match "
                              "this runtime; report this line.");
            }
            if (forced) {
                spdlog::warn("[verdict] REPRO MODE WAS ON — refreshAll forced {} times. "
                             "This build deliberately caused the bug.", forced);
            }
            if (g_cfg.freezeLODWhilePaused && frozen > 0) {
                // Careful wording: while frozen, CheckBudget never dispatches,
                // so the counted visits are almost entirely CheckBudget polls —
                // they measure how often the LOD machine WOULD have run, not
                // how many promote/demote batches were prevented.
                spdlog::info("[verdict] => Exact PauseMenu freeze DID run: {} LOD-machine visits "
                             "during pauses were redirected through the native Reset transition "
                             "instead of executing.", frozen);
            } else if (g_cfg.freezeLODWhilePaused && releases > 0) {
                spdlog::info("[verdict] => Exact PauseMenu was observed and the release Reset ran. "
                             "No hooked LOD-changing state visited while the menu was open, so "
                             "there was nothing to skip during that pause.");
            } else if (g_cfg.freezeLODWhilePaused) {
                spdlog::warn("[verdict] => Exact PauseMenu freeze has not run in this session so "
                             "far. If a pause DID occur before this line, verify the startup log "
                             "says the engine detector and all five action hooks resolved.");
            } else if (obs > 0) {
                spdlog::info("[verdict] => The mass full-resolution upload path DID fire in this "
                             "session. This is the suspected cause of the pause VRAM spike. Search "
                             "this log for \"[EVENT]\" to see exactly when, and what VRAM was doing.");
            } else {
                spdlog::info("[verdict] => refreshAll never fired; no exact state freeze was enabled.");
            }
            spdlog::info("─────────────────────────────────");
        }

        void DiagLoop()
        {
            ::SetThreadDescription(::GetCurrentThread(), L"PauseLODHold.Diag");

            OsVramSampler os{};
            if (os.Init()) {
                os.Sample();
                spdlog::info("[osvram] WDDM telemetry active ({} adapter(s)) — "
                             "'os vram used/budget' on timeline rows is the OS's view, "
                             "which the engine's tracker cannot see", os.adapters.size());
            } else {
                os.Release();
                spdlog::warn("[osvram] DXGI unavailable — timeline rows will omit the OS VRAM view");
            }

            auto next_summary = std::chrono::steady_clock::now();
            auto lastSummary  = next_summary;
            auto next_health  = next_summary;
            auto lastHealth   = next_summary;
            auto next_verdict = next_summary + std::chrono::seconds(g_cfg.verdictIntervalSec);

            std::uint64_t prevTotal = 0, prevAct = 0, prevPass = 0, prevFrozen = 0;
            std::uint64_t healthPrevTotal = 0, healthPrevAct = 0;
            std::uint64_t lastVerdictTotal = ~0ull;
            std::chrono::steady_clock::time_point lowSince{};
            bool lowActive = false;

            // ── Pause recovery report (diag thread only) ──────────────────
            // One [pause-report] line per exact-PauseMenu session, comparing
            // the LOD-cycle rate and VRAM before OPEN against 0-1s / 1-5s /
            // 5-15s after CLOSE — so "did we return to pre-pause performance"
            // is a grep, not a spreadsheet.
            struct RateSample { std::int64_t tMs; double rate; };
            std::array<RateSample, 96> rateRing{};
            std::size_t rateCount = 0;

            struct {
                bool          open = false, pending = false;
                std::uint64_t session = 0;
                std::int64_t  openMs = 0, closeMs = 0;
                double        preRate = -1.0;
                int           preMode = -1;
                std::uint64_t preUsage = 0, closeUsage = 0;
                std::uint64_t preOsUsage = 0, preOsBudget = 0;
                std::uint64_t closeOsUsage = 0;
            } pr;

            auto rateStat = [&](std::int64_t fromMs, std::int64_t toMs,
                                bool median) -> double {
                std::array<double, 96> v{};
                std::size_t n = 0;
                const std::size_t have = std::min(rateCount, rateRing.size());
                for (std::size_t i = 0; i < have; ++i) {
                    if (rateRing[i].tMs > fromMs && rateRing[i].tMs <= toMs) {
                        v[n++] = rateRing[i].rate;
                    }
                }
                if (n == 0) return -1.0;
                if (median) {
                    std::nth_element(v.begin(), v.begin() + n / 2, v.begin() + n);
                    return v[n / 2];
                }
                double sum = 0.0;
                for (std::size_t i = 0; i < n; ++i) sum += v[i];
                return sum / static_cast<double>(n);
            };

            auto fmtRate = [](double r) {
                return r < 0.0 ? std::string("n/a") : fmt::format("{:.1f}", r);
            };

            auto emitPauseReport = [&](const char* truncatedBy) {
                const BudgetRead bud = ReadBudget();
                const double p01  = rateStat(pr.closeMs,        pr.closeMs + 1000,  false);
                const double p15  = rateStat(pr.closeMs + 1000, pr.closeMs + 5000,  false);
                const double p515 = rateStat(pr.closeMs + 5000, pr.closeMs + 15000, false);
                const double late = p515 >= 0.0 ? p515 : (p15 >= 0.0 ? p15 : p01);
                // Below this, the ratios are noise over noise: at a few cycles
                // per second a single missing sample flips DEGRADED, and a
                // verdict that cries wolf in every quiet scene is one testers
                // learn to ignore. Say so instead of guessing.
                constexpr double kMinRateToJudge = 20.0;
                const char* verdict =
                    truncatedBy                            ? truncatedBy
                    : (pr.preRate <= 0.0 || late < 0.0)    ? "NO-DATA"
                    : (pr.preRate < kMinRateToJudge)       ? "IDLE (too little LOD activity to judge)"
                    : (late >= pr.preRate * 0.95
                       && bud.mode <= pr.preMode)          ? "EXACT"
                    : (late >= pr.preRate * 0.75)          ? "OK"
                                                           : "DEGRADED";
                std::string osPart;
                if (g_osValid.load(std::memory_order_relaxed) || pr.preOsBudget != 0) {
                    // If the OS shrank our budget or evicted our allocations
                    // while the game sat idle, this is where it shows up — and
                    // nothing in the LOD machine could have prevented it.
                    const auto nowOsUsage  = g_osUsage.load(std::memory_order_relaxed);
                    const auto nowOsBudget = g_osBudget.load(std::memory_order_relaxed);
                    osPart = fmt::format(
                        " | os vram MB: pre={} at-close={} now={} (budget pre={} now={})",
                        pr.preOsUsage >> 20, pr.closeOsUsage >> 20, nowOsUsage >> 20,
                        pr.preOsBudget >> 20, nowOsBudget >> 20);
                }

                spdlog::info("[pause-report] session={} pause={}ms | LOD-cycle rate/s: "
                             "pre(10s median)={} post 0-1s={} 1-5s={} 5-15s={} | "
                             "vram MB: pre={} at-close={} now={} (target {}) | "
                             "mode: pre={} now={}{} | recovery={}",
                             pr.session, pr.closeMs - pr.openMs,
                             fmtRate(pr.preRate), fmtRate(p01), fmtRate(p15), fmtRate(p515),
                             pr.preUsage >> 20, pr.closeUsage >> 20,
                             bud.valid ? (bud.usage >> 20) : 0,
                             bud.valid ? (bud.target >> 20) : 0,
                             pr.preMode, bud.mode, osPart, verdict);
                pr.pending = false;
            };

            while (!g_shutdown.load(std::memory_order_relaxed)) {
                auto now = std::chrono::steady_clock::now();
                const std::int64_t nowMs = NowMs();
                const bool inMenu = MenuWatcher::IsSuspendingMenuOpen();

                // Once the engine wants to quit, this thread STOPS ENTIRELY.
                //
                // Everything below touches something the shutdown is busy
                // destroying: ReadBudget walks the engine's VRAM singleton,
                // os.Sample() calls QueryVideoMemoryInfo on the graphics
                // adapter while D3D is tearing the device down, and every log
                // line is disk I/O competing with the engine's own writes.
                // Gating only the LOD redirects left all of this running,
                // which is why a tester still saw a slow exit that stopped
                // when the plugin was removed.
                //
                // Write the final verdict, drop DXGI, and end the thread so
                // nothing of ours remains for the rest of the shutdown.
                if (MenuWatcher::EngineShutdownRequested()) {
                    if (pr.pending && !pr.open) {
                        emitPauseReport("TRUNCATED (quit requested)");
                    }
                    DrainTimeline();
                    LogVerdict("quit requested");
                    spdlog::info("[hook] diagnostics stopped — no further engine, driver or log "
                                 "activity from this plugin during shutdown");
                    spdlog::default_logger()->flush();
                    os.Release();
                    // Read the singleton BEFORE it is torn down. If the pipeline
                    // cache is clean this terminates the process here; otherwise
                    // it returns and the game shuts down normally to persist it.
                    MaybeFastExit();
                    return;
                }

                DrainTimeline();

                if (now >= next_health) {
                    os.Sample();
                    const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHealth).count();
                    const auto total = g_totalCalls.load(std::memory_order_relaxed);
                    const auto act   = g_actionCalls.load(std::memory_order_relaxed);
                    if (dtMs > 0) {
                        const double rate = static_cast<double>(total - healthPrevTotal) * 1000.0 / static_cast<double>(dtMs);
                        // Gameplay samples only. In-menu rates are frozen-visit
                        // throughput, and storing them would pollute the next
                        // pause-report's pre-pause median when pauses come
                        // back-to-back.
                        if (!inMenu) {
                            rateRing[rateCount % rateRing.size()] = { nowMs, rate };
                            ++rateCount;
                        }
                        const bool intervening = (act - healthPrevAct) > 0;

                        if (!inMenu) {
                            if (rate > 1.0) {
                                const double b = g_baselineRate.load(std::memory_order_relaxed);
                                g_baselineRate.store(b <= 0.0 ? rate : (b * 0.8 + rate * 0.2),
                                                     std::memory_order_relaxed);
                            }
                            lowActive = false;
                            g_watchdogTripped.store(false, std::memory_order_relaxed);
                        } else if (g_cfg.watchdogEnabled && intervening) {
                            const double baseline = g_baselineRate.load(std::memory_order_relaxed);
                            if (baseline > 5.0 && rate < baseline * static_cast<double>(g_cfg.watchdogRatio)) {
                                if (!lowActive) { lowActive = true; lowSince = now; }
                                else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lowSince).count()
                                         >= static_cast<std::int64_t>(g_cfg.watchdogTripMs)) {
                                    if (!g_watchdogTripped.exchange(true, std::memory_order_relaxed)) {
                                        spdlog::warn("[watchdog] tick rate {:.1f}/s fell below {:.0f}% of the "
                                                     "{:.1f}/s pre-menu baseline for {}ms — standing down",
                                                     rate, g_cfg.watchdogRatio * 100.0, baseline, g_cfg.watchdogTripMs);
                                    }
                                }
                            } else {
                                lowActive = false;
                            }
                        }
                    }
                    healthPrevTotal = total; healthPrevAct = act;
                    lastHealth = now;
                    next_health = now + std::chrono::milliseconds(g_cfg.healthSampleMs);
                }

                // ── Pause-report OPEN/CLOSE edges ─────────────────────────
                const bool pauseOpen = MenuWatcher::IsExactPauseMenuOpen();
                if (pauseOpen && !pr.open) {
                    if (pr.pending) {
                        emitPauseReport("TRUNCATED (next pause arrived)");
                    }
                    const BudgetRead bud = ReadBudget();
                    pr.open        = true;
                    pr.session     = MenuWatcher::SessionId();
                    pr.openMs      = nowMs;
                    pr.preRate     = rateStat(nowMs - 10000, nowMs, true);
                    pr.preMode     = bud.mode;
                    pr.preUsage    = bud.valid ? bud.usage : 0;
                    pr.preOsUsage  = g_osUsage.load(std::memory_order_relaxed);
                    pr.preOsBudget = g_osBudget.load(std::memory_order_relaxed);
                } else if (!pauseOpen && pr.open) {
                    const BudgetRead bud = ReadBudget();
                    pr.open         = false;
                    pr.closeMs      = nowMs;
                    pr.closeUsage   = bud.valid ? bud.usage : 0;
                    pr.closeOsUsage = g_osUsage.load(std::memory_order_relaxed);
                    pr.pending      = true;
                }
                if (pr.pending && !pr.open && nowMs - pr.closeMs >= 15000) {
                    emitPauseReport(nullptr);
                }

                if (now >= next_summary) {
                    auto total = g_totalCalls.load(std::memory_order_relaxed);
                    auto act   = g_actionCalls.load(std::memory_order_relaxed);
                    auto pass  = g_passedCalls.load(std::memory_order_relaxed);
                    auto frozen = g_pausedStateSkips.load(std::memory_order_relaxed);
                    auto dTotal = total - prevTotal;

                    // Emitted even when nothing happened. This line is the
                    // plugin's proof of life: with it, a log that simply stops
                    // means the PROCESS died. Without it, an alt-tabbed game
                    // whose renderer went idle looked exactly like a crash —
                    // which is why one v0.8 tester session could never be
                    // explained. calls=0 means the renderer is not ticking.
                    {
                        // Measured elapsed, not the configured interval — the
                        // wall time between summaries stretches whenever the
                        // diag thread stalls in synchronous log flushes, and a
                        // rate computed against the nominal interval would
                        // silently overstate throughput in exactly those
                        // moments.
                        const auto sumDt = std::max<std::int64_t>(1,
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSummary).count());
                        spdlog::info("[summary] last {}ms — calls={} (acted={}, pass={})  rate={:.1f}/s  "
                                     "baseline={:.1f}/s  refreshSeen={} refreshCleared={} "
                                     "pausedStateSkips={} (+{}) postPauseResets={} "
                                     "upgradeCaps={} degradeHolds={} pressureGuard={}  "
                                     "mode histogram[0/1/2/3]={}/{}/{}/{}",
                                     sumDt, dTotal, act - prevAct, pass - prevPass,
                                     static_cast<double>(dTotal) * 1000.0 / static_cast<double>(sumDt),
                                     g_baselineRate.load(std::memory_order_relaxed),
                                     g_refreshObserved.load(std::memory_order_relaxed),
                                     g_refreshClears.load(std::memory_order_relaxed),
                                     frozen, frozen - prevFrozen,
                                     g_postPauseResets.load(std::memory_order_relaxed),
                                     g_upgradeBudgetCaps.load(std::memory_order_relaxed),
                                     g_degradeHolds.load(std::memory_order_relaxed),
                                     g_pressureGuardActive.load(std::memory_order_relaxed),
                                     g_modeHistogram[0].load(std::memory_order_relaxed),
                                     g_modeHistogram[1].load(std::memory_order_relaxed),
                                     g_modeHistogram[2].load(std::memory_order_relaxed),
                                     g_modeHistogram[3].load(std::memory_order_relaxed));
                    }
                    prevTotal = total; prevAct = act; prevPass = pass;
                    prevFrozen = frozen;
                    lastSummary  = now;
                    next_summary = now + std::chrono::milliseconds(g_cfg.summaryIntervalMs);
                }

                // Starfield quits via TerminateProcess, which skips
                // DLL_PROCESS_DETACH — no tester log has ever reached the
                // shutdown verdict. Emit it periodically instead, so the tail
                // of any log always carries a recent one.
                if (now >= next_verdict) {
                    const auto total = g_totalCalls.load(std::memory_order_relaxed);
                    if (total != lastVerdictTotal) {
                        LogVerdict("periodic — latest state");
                        lastVerdictTotal = total;
                    }
                    next_verdict = now + std::chrono::seconds(g_cfg.verdictIntervalSec);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (pr.pending && !pr.open) {
                emitPauseReport("TRUNCATED (shutdown)");
            }
            DrainTimeline();
            os.Release();
        }
    }

    bool Install(const Config& cfg)
    {
        g_cfg = cfg;
        g_hookTargets.clear();
        g_pauseFreezeReady.store(false, std::memory_order_release);
        g_loggedFreezeSession.store(0, std::memory_order_relaxed);
        g_pendingPostPauseReset.store(0, std::memory_order_relaxed);
        g_completedPostPauseReset.store(0, std::memory_order_relaxed);
        g_resetState = nullptr;
        g_moduleBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
        if (g_moduleBase == 0) {
            spdlog::error("[hook] GetModuleHandle(nullptr) failed");
            return false;
        }

        std::uintptr_t targetAddr = 0;
        const char* resolvedVia = nullptr;

        if (cfg.idCheckBudgetFunc4 != 0) {
            std::uintptr_t rva = slh::AddressLib::LookupRVA(cfg.idCheckBudgetFunc4);
            if (rva != 0) {
                targetAddr = g_moduleBase + rva;
                resolvedVia = "Address Library";
                spdlog::info("[hook] resolved CheckBudget::Func4 via Address Library: ID {} -> RVA 0x{:x}",
                             cfg.idCheckBudgetFunc4, rva);
            }
        }
        if (targetAddr == 0 && !cfg.sigCheckBudgetFunc4.empty()) {
            bool ambiguous = false;
            targetAddr = slh::Scanner::FindUnique(cfg.sigCheckBudgetFunc4, "CheckBudget::Func4", ambiguous);
            if (targetAddr != 0) {
                resolvedVia = "signature scan";
            } else if (!ambiguous) {
                spdlog::warn("[hook] signature scan missed — trying RVA fallback 0x{:x}",
                             cfg.checkBudgetFunc4RvaFallback);
            }
        }
        if (targetAddr == 0) {
            targetAddr = g_moduleBase + cfg.checkBudgetFunc4RvaFallback;
            std::uint8_t firstByte = 0;
            if (!TryRead(targetAddr, firstByte) || firstByte == 0 || firstByte == 0xCC) {
                spdlog::error("[hook] RVA fallback at 0x{:x} also invalid (first byte = 0x{:02x}); aborting",
                              targetAddr, firstByte);
                return false;
            }
            resolvedVia = "RVA fallback";
        }
        spdlog::info("[hook] CheckBudget::Func4 target = 0x{:x} (RVA 0x{:x}) — resolved via {}",
                     targetAddr, targetAddr - g_moduleBase, resolvedVia);

        struct PrologueCheck { std::size_t off; std::uint8_t want; const char* label; };
        constexpr PrologueCheck kProl[] = {
            { 0x00, 0x48, "prologue.0" }, { 0x01, 0x89, "prologue.1" }, { 0x05, 0x57, "push rdi" },
            { 0x0a, 0x48, "mov rax,[rip+disp]" }, { 0x0b, 0x8b, "mov rax,[rip+disp]" },
            { 0x0c, 0x05, "mov rax,[rip+disp]" },
            { 0x1e, 0x4c, "mov r10,[rax+0xf8]" }, { 0x1f, 0x8b, "mov r10,[rax+0xf8]" },
            { 0x20, 0x90, "mov r10,[rax+0xf8]" }, { 0x21, 0xf8, "tracker offset 0xf8" },
            { 0x25, 0x41, "movzx ecx,[r10+0x28]" }, { 0x26, 0x0f, "movzx ecx,[r10+0x28]" },
            { 0x27, 0xb6, "movzx ecx,[r10+0x28]" }, { 0x28, 0x4a, "movzx ecx,[r10+0x28]" },
            { 0x29, 0x28, "mode byte offset 0x28" },
        };
        int prologueMatches = 0, prologueChecks = 0;
        for (const auto& c : kProl) {
            std::uint8_t got = 0;
            if (TryRead(targetAddr + c.off, got)) {
                ++prologueChecks;
                if (got == c.want) ++prologueMatches;
                else spdlog::warn("[hook] prologue mismatch at +0x{:02x}: got 0x{:02x} expected 0x{:02x} ({})",
                                  c.off, got, c.want, c.label);
            }
        }
        const bool prologueOk = (prologueMatches == prologueChecks) && (prologueChecks > 0);

        // Resolution trust matters more than the prologue itself. An Address
        // Library hit is a positive identification, so a byte mismatch there
        // just means a recompiled prologue on a newer patch: keep the hook,
        // refuse to write. A signature scan or the hardcoded RVA is a guess,
        // and a guess that fails verification is probably a DIFFERENT function
        // — detouring it would call an unknown callee with our four arguments.
        // Refuse, rather than risk a crash a user would blame on the mod.
        const bool trustedResolution = resolvedVia != nullptr
            && std::string_view(resolvedVia) == "Address Library";
        if (!prologueOk && !trustedResolution) {
            spdlog::error("[hook] {} gave RVA 0x{:x}, but its prologue does not match "
                          "CheckBudget::Func4 ({}/{} bytes). That address is probably a "
                          "different function on this build, so no hook will be installed. "
                          "Install the Address Library for your Starfield version "
                          "(versionlib-*.bin in Data\\SFSE\\Plugins) and this resolves properly.",
                          resolvedVia, targetAddr - g_moduleBase,
                          prologueMatches, prologueChecks);
            return false;
        }
        if (prologueOk) {
            spdlog::info("[hook] prologue verification PASSED ({}/{} bytes)", prologueMatches, prologueChecks);
        } else {
            spdlog::warn("[hook] prologue verification FAILED ({}/{}) — writes DISABLED, "
                         "observation only", prologueMatches, prologueChecks);
        }

        std::uintptr_t singletonPtr = 0;
        if (cfg.idBudgetSingletonPtr != 0) {
            std::uintptr_t rva = slh::AddressLib::LookupRVA(cfg.idBudgetSingletonPtr);
            if (rva != 0) {
                singletonPtr = g_moduleBase + rva;
                spdlog::info("[hook] budget singleton via Address Library: ID {} -> RVA 0x{:x}",
                             cfg.idBudgetSingletonPtr, rva);
            }
        }
        if (singletonPtr == 0) {
            singletonPtr = slh::Scanner::DecodeRipRelativeTarget(
                targetAddr + cfg.singletonLoadOffset, 3, cfg.singletonLoadInstrLen);
            if (singletonPtr != 0) {
                spdlog::info("[hook] budget singleton via RIP-relative decode at 0x{:x} (RVA 0x{:x})",
                             singletonPtr, singletonPtr - g_moduleBase);
            }
        }
        g_budgetSingletonPtr = singletonPtr;
        if (singletonPtr == 0) {
            spdlog::error("[hook] could not resolve the budget singleton — no mode gate and no VRAM "
                          "telemetry, so writes are DISABLED (observation only)");
        }

        g_writesEnabled.store(prologueOk && singletonPtr != 0, std::memory_order_release);

        struct FreezeHookSpec
        {
            std::uint64_t id;
            const char* name;
            LPVOID detour;
            StateFn* original;
            std::uintptr_t target = 0;
        };
        std::array<FreezeHookSpec, 5> freezeHooks{ {
            { cfg.idUpgradeMeshesFunc4, "UpgradeMeshes::Func4",
              reinterpret_cast<LPVOID>(&Hooked_UpgradeMeshes), &g_origUpgradeMeshes },
            { cfg.idUpgradeTexturesFunc4, "UpgradeTextures::Func4",
              reinterpret_cast<LPVOID>(&Hooked_UpgradeTextures), &g_origUpgradeTextures },
            { cfg.idDegradeTexturesFunc4, "DegradeTextures::Func4",
              reinterpret_cast<LPVOID>(&Hooked_DegradeTextures), &g_origDegradeTextures },
            { cfg.idBalanceTextureLODsFunc4, "BalanceTextureLODs::Func4",
              reinterpret_cast<LPVOID>(&Hooked_BalanceTextureLODs), &g_origBalanceTextureLODs },
            { cfg.idDegradeMeshesFunc4, "DegradeMeshes::Func4",
              reinterpret_cast<LPVOID>(&Hooked_DegradeMeshes), &g_origDegradeMeshes }
        } };

        bool freezeResolved = false;
        if (cfg.freezeLODWhilePaused) {
            if (!prologueOk) {
                // The prologue bytes encode the exact layout this plugin's
                // model of the engine depends on. A mismatch means this binary
                // differs from the one we verified — and the freeze redirects
                // native code using that model. Disabling only WRITES (as the
                // legacy holds do) is not enough for the freeze; refuse it.
                spdlog::error("[freeze] CheckBudget prologue did not verify on this runtime — "
                              "state freeze DISABLED along with writes; this build of the game "
                              "does not match the layout this plugin was verified against");
            } else if (!MenuWatcher::ExactPauseMenuDetectionReady()) {
                spdlog::error("[freeze] exact UI::IsMenuOpen(\"PauseMenu\") detector unavailable — "
                              "state freeze DISABLED; cursor fallback is intentionally not trusted");
            } else if (!MenuWatcher::QuitDetectionReady()) {
                // Without it we cannot tell that the game is quitting, and the
                // freeze would still be redirecting into the engine during
                // teardown — the thing that hangs people's games.
                spdlog::error("[freeze] Main::Singleton did not resolve, so there is no way to "
                              "detect shutdown on this runtime — state freeze DISABLED rather "
                              "than risk redirecting while the engine tears down");
            } else {
                const auto resetRva = slh::AddressLib::LookupRVA(cfg.idResetFunc3);
                std::uint8_t resetFirstByte = 0;
                bool allResolved = resetRva != 0
                    && TryRead(g_moduleBase + resetRva, resetFirstByte)
                    && resetFirstByte != 0 && resetFirstByte != 0xCC;
                if (allResolved) {
                    g_resetState = reinterpret_cast<StateFn>(g_moduleBase + resetRva);
                    spdlog::info("[freeze] resolved Reset::Func3: ID {} -> RVA 0x{:x}",
                                 cfg.idResetFunc3, resetRva);
                } else {
                    spdlog::error("[freeze] Reset::Func3 ID {} did not resolve",
                                  cfg.idResetFunc3);
                }

                for (auto& hook : freezeHooks) {
                    const auto rva = slh::AddressLib::LookupRVA(hook.id);
                    std::uint8_t firstByte = 0;
                    if (rva == 0
                        || !TryRead(g_moduleBase + rva, firstByte)
                        || firstByte == 0 || firstByte == 0xCC) {
                        spdlog::error("[freeze] {} ID {} did not resolve to valid code",
                                      hook.name, hook.id);
                        allResolved = false;
                        continue;
                    }
                    hook.target = g_moduleBase + rva;
                    spdlog::info("[freeze] resolved {}: ID {} -> RVA 0x{:x}",
                                 hook.name, hook.id, rva);
                }

                // Six distinct states plus Reset must be six distinct
                // functions, all near CheckBudget — they are members of the
                // same class, emitted together. A stale or mismatched address
                // library can satisfy the first-byte check while pointing
                // somewhere unrelated; detouring that would crash on pause and
                // look exactly like the bug we are chasing.
                if (allResolved) {
                    const std::uintptr_t checkRva = targetAddr - g_moduleBase;
                    constexpr std::uintptr_t kMaxSpread = 0x100000;  // 1 MB
                    std::array<std::uintptr_t, 6> seen{};
                    std::size_t n = 0;
                    seen[n++] = g_resetState != nullptr
                        ? reinterpret_cast<std::uintptr_t>(g_resetState) : 0;
                    for (const auto& hook : freezeHooks) seen[n++] = hook.target;

                    for (std::size_t i = 0; i < n && allResolved; ++i) {
                        const std::uintptr_t rva = seen[i] - g_moduleBase;
                        const auto spread = rva > checkRva ? rva - checkRva : checkRva - rva;
                        if (seen[i] == targetAddr || spread > kMaxSpread) {
                            spdlog::error("[freeze] target at RVA 0x{:x} is 0x{:x} bytes from "
                                          "CheckBudget (or aliases it) — address library looks "
                                          "wrong for this runtime; freeze DISABLED", rva, spread);
                            allResolved = false;
                        }
                        for (std::size_t j = i + 1; j < n; ++j) {
                            if (seen[i] == seen[j]) {
                                spdlog::error("[freeze] two freeze targets resolved to the same "
                                              "address 0x{:x} — freeze DISABLED", seen[i]);
                                allResolved = false;
                            }
                        }
                    }
                }
                freezeResolved = allResolved;
            }
        }

        MH_STATUS st = MH_Initialize();
        if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
            spdlog::error("[hook] MH_Initialize failed: {}", static_cast<int>(st));
            return false;
        }
        st = MH_CreateHook(reinterpret_cast<LPVOID>(targetAddr),
                           reinterpret_cast<LPVOID>(&Hooked_CheckBudget),
                           reinterpret_cast<LPVOID*>(&g_origCheckBudget));
        if (st != MH_OK) {
            spdlog::error("[hook] CheckBudget MH_CreateHook failed: {}", static_cast<int>(st));
            return false;
        }
        st = MH_EnableHook(reinterpret_cast<LPVOID>(targetAddr));
        if (st != MH_OK) {
            spdlog::error("[hook] CheckBudget MH_EnableHook failed: {}", static_cast<int>(st));
            MH_RemoveHook(reinterpret_cast<LPVOID>(targetAddr));
            return false;
        }
        g_hookTargets.push_back(targetAddr);

        bool freezeHooksEnabled = false;
        if (freezeResolved) {
            std::vector<std::uintptr_t> created;
            bool createOk = true;
            for (auto& hook : freezeHooks) {
                st = MH_CreateHook(reinterpret_cast<LPVOID>(hook.target),
                                   hook.detour,
                                   reinterpret_cast<LPVOID*>(hook.original));
                if (st != MH_OK) {
                    spdlog::error("[freeze] {} MH_CreateHook failed: {}",
                                  hook.name, static_cast<int>(st));
                    createOk = false;
                    break;
                }
                created.push_back(hook.target);
            }

            if (!createOk) {
                for (auto target : created) {
                    MH_RemoveHook(reinterpret_cast<LPVOID>(target));
                }
            } else {
                // Patch the five action functions in one suspended-thread
                // transaction. If MinHook reports an apply failure, retain
                // every created trampoline and keep freezeReady false: any
                // partially enabled detour safely passes through its original,
                // and no live callback can race a rollback/free.
                st = MH_EnableHook(MH_ALL_HOOKS);
                g_hookTargets.insert(g_hookTargets.end(), created.begin(), created.end());
                if (st != MH_OK) {
                    spdlog::error("[freeze] batched action-hook enable failed: {}; "
                                  "state freeze remains disabled",
                                  static_cast<int>(st));
                } else {
                    g_pauseFreezeReady.store(true, std::memory_order_release);
                    freezeHooksEnabled = true;
                }
            }
        }

        g_installed.store(true);
        spdlog::info("[hook] CheckBudget installed at 0x{:x}  writes={}  dry_run={}",
                     targetAddr, g_writesEnabled.load(), cfg.dryRun);
        spdlog::info("[freeze] exact PauseMenu state freeze={} "
                     "(CheckBudget + {} LOD action hooks; one Reset on release)",
                     freezeHooksEnabled, freezeHooks.size());
        spdlog::info("[hook] legacy policy: clearRefreshAll={} holdUpgradeBudgets={} "
                     "(+{}ms after menu close)  pressureGuard={} (enter/exit={:.0f}/{:.0f}MB)",
                     cfg.clearRefreshAll, cfg.holdUpgradeBudgets, cfg.upgradeHoldAfterCloseMs,
                     cfg.pressureGuardEnabled,
                     MB(cfg.pressureGuardEnterHeadroomBytes),
                     MB(cfg.pressureGuardExitHeadroomBytes));
        spdlog::info("[hook] legacy policy: holdDegradeBudget={} (mode 1..{}, minHeadroom={:.0f}MB)  "
                     "watchdog={}",
                     cfg.holdDegradeBudget, cfg.maxSuppressMode,
                     MB(cfg.minHeadroomForDegradeHoldBytes), cfg.watchdogEnabled);
        if (cfg.forceRefreshAll) {
            spdlog::warn("**********************************************************");
            spdlog::warn("[hook] REPRO MODE ENABLED (bForceRefreshAll=1).");
            spdlog::warn("[hook] This build will DELIBERATELY CAUSE the pause VRAM spike.");
            spdlog::warn("[hook] Set it back to 0 unless you are running the A/B test.");
            spdlog::warn("**********************************************************");
        }

        g_shutdown.store(false);
        std::thread(DiagLoop).detach();   // see the comment on g_shutdown
        return true;
    }

    void Shutdown()
    {
        if (!g_installed.exchange(false)) return;
        // The diag thread is detached; signalling is all that can be done. It
        // exits on its own within one 25ms iteration. (This function has no
        // callers — teardown is the quit-flag dormancy path — but keep it
        // coherent for anyone who wires it up later.)
        g_shutdown.store(true);
        g_pauseFreezeReady.store(false, std::memory_order_release);
        for (auto it = g_hookTargets.rbegin(); it != g_hookTargets.rend(); ++it) {
            MH_DisableHook(reinterpret_cast<LPVOID>(*it));
            MH_RemoveHook(reinterpret_cast<LPVOID>(*it));
        }
        g_hookTargets.clear();
        spdlog::info("[hook] shutdown — totalCalls={} pausedStateSkips={} postPauseResets={}",
                     g_totalCalls.load(), g_pausedStateSkips.load(), g_postPauseResets.load());
        DrainTimeline();
        LogVerdict("shutdown");
    }

    Counters SnapshotCounters() noexcept
    {
        Counters c{};
        c.totalCalls = g_totalCalls.load(std::memory_order_relaxed);
        c.suppressedCalls = g_degradeHolds.load(std::memory_order_relaxed);
        c.refreshClears = g_refreshClears.load(std::memory_order_relaxed);
        c.upgradeBudgetCaps = g_upgradeBudgetCaps.load(std::memory_order_relaxed);
        c.pausedStateSkips = g_pausedStateSkips.load(std::memory_order_relaxed);
        c.postPauseResets = g_postPauseResets.load(std::memory_order_relaxed);
        c.passedCalls = g_passedCalls.load(std::memory_order_relaxed);
        for (size_t i = 0; i < 4; i++) {
            c.modeHistogram[i] = g_modeHistogram[i].load(std::memory_order_relaxed);
        }
        return c;
    }

    void LogFinalVerdict()
    {
        // Safe here: either the diag thread is joined (normal shutdown) or the
        // process is exiting and every other thread is already stopped.
        DrainTimeline();
        LogVerdict("process exit");
    }
}
