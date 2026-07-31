#include "PCH.h"
#include "MenuWatcher.h"
#include "AddressLib.h"

namespace slh::MenuWatcher
{
    namespace
    {
        Settings                g_cfg;

        std::atomic<bool>       g_cursorVisible{ false };
        std::atomic<bool>       g_cursorClipped{ false };
        std::atomic<bool>       g_enginePauseMenu{ false };
        std::atomic<bool>       g_foreground{ false };
        std::atomic<bool>       g_menuOpen{ false };       // DEBOUNCED state — what the hook reads
        std::atomic<std::uint64_t> g_sessionId{ 0 };
        std::atomic<std::int64_t>  g_lastOpenMs{ 0 };
        std::atomic<std::int64_t>  g_lastCloseMs{ 0 };

        // Started detached; no std::thread object is kept. A static
        // std::thread left joinable at DLL_PROCESS_DETACH static destruction
        // calls std::terminate — see the matching comment in Hook.cpp.
        std::atomic<bool>       g_started{ false };
        std::atomic<bool>       g_shutdown{ false };
        std::vector<std::string> g_configuredMenus;

        // Raw, minimal equivalents of UI::GetSingleton,
        // UI::IsMenuOpen(const BSFixedString&) and the engine-owned static
        // BSFixedString("PauseMenu") getter.
        // Main::Singleton. Byte +0x28 is the engine's "quit requested" flag:
        // every quit route sets it and then the main loop unwinds. Found by
        // decompiling the QuitGame/qqq console command on 1.16.236, whose whole
        // body is:
        //     Main::Singleton->quitRequested = 1;
        // Seven distinct call sites write it — the console command, the pause
        // menu's Quit to Desktop, and the other menu paths — so reading this one
        // byte covers all of them. It is set the moment quit is REQUESTED, well
        // before the engine starts destroying anything, which is exactly the
        // signal the LOD freeze needs and could not get any other way.
        // ID 883591 -> RVA 0x5e794d0 (1.16.236), 0x5e71710 (1.16.244).
        constexpr std::uint64_t kIdMainSingleton = 883591;
        constexpr std::uintptr_t kMainQuitFlagOffset = 0x28;
        std::uintptr_t g_mainSingletonPtr = 0;
        std::atomic<bool> g_quitRequested{ false };

        constexpr std::uint64_t kIdUISingleton = 937580;
        constexpr std::uint64_t kIdUIIsMenuOpen = 130475;
        // Compiler-generated getter for the engine-owned static
        // BSFixedString("PauseMenu"). Unlike BSStringPool::GetEntry ID 139352,
        // this remains present in the 1.16.244 Address Library.
        constexpr std::uint64_t kIdPauseMenuName = 130409;
        std::uintptr_t g_uiSingletonPtr = 0;
        using IsMenuOpenFn = bool(__fastcall*)(void*, const void*);
        using GetPauseMenuNameFn = const void*(__fastcall*)();
        IsMenuOpenFn g_isMenuOpen = nullptr;
        const void*  g_pauseMenuName = nullptr;
        bool         g_engineDetectionReady = false;

        std::int64_t NowMs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Locate once, then probe liveness.
        //
        // IsWindow() is useless for this: measured on 1.16.236, Starfield never
        // calls DestroyWindow when quitting — it tears down and exits, and the
        // OS reaps the window at process death, so the handle stays valid all
        // the way through DLL_PROCESS_DETACH. It never once returned false.
        //
        // What does change, ~20 seconds before exit, is that the main thread
        // stops pumping messages. A zero-cost WM_NULL with SMTO_ABORTIFHUNG
        // detects exactly that. Debounced, because a heavy load screen can also
        // stall the pump briefly, and allowed to recover, so a transient stall
        // does not disable the fix for the rest of the session.
        // Read the engine's own quit-requested flag.
        //
        // Three passive signals were tested against a real quit on 1.16.236 and
        // all three failed, which is why this reads engine state instead:
        //   - budget singleton readability : never went invalid (0 hits)
        //   - IsWindow(main window)        : never went false. Starfield does not
        //                                    DestroyWindow on quit; the OS reaps
        //                                    the window at process death.
        //   - message pump (WM_NULL probe) : not separable — ordinary streaming
        //                                    stalls the pump for up to 33s,
        //                                    longer than the ~21s stall before
        //                                    exit. Tripped 11x in one session.
        // Do not reintroduce any of them.
        void UpdateQuitRequested() noexcept
        {
            if (g_mainSingletonPtr == 0 || g_quitRequested.load(std::memory_order_relaxed)) {
                return;   // latched: a quit is never taken back
            }
            __try {
                const auto main = *reinterpret_cast<std::uintptr_t*>(g_mainSingletonPtr);
                if (main == 0) return;
                if (*reinterpret_cast<const std::uint8_t*>(main + kMainQuitFlagOffset) != 0) {
                    g_quitRequested.store(true, std::memory_order_release);
                    spdlog::warn("[menu] engine quit requested (Main+0x{:x}) — LOD freeze stands "
                                 "down so shutdown runs entirely on engine code",
                                 kMainQuitFlagOffset);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Singleton went away underneath us: also a shutdown.
                g_quitRequested.store(true, std::memory_order_release);
            }
        }

        // Is the foreground window ours? Without this, any overlay or another
        // app showing a cursor while Starfield runs windowed reads as a menu.
        bool ForegroundIsOurProcess() noexcept
        {
            HWND fg = ::GetForegroundWindow();
            if (!fg) return false;
            DWORD pid = 0;
            ::GetWindowThreadProcessId(fg, &pid);
            return pid == ::GetCurrentProcessId();
        }

        bool QueryEnginePauseMenu() noexcept
        {
            if (!g_engineDetectionReady || g_uiSingletonPtr == 0
                || g_isMenuOpen == nullptr || g_pauseMenuName == nullptr) {
                return false;
            }

            __try {
                void* ui = *reinterpret_cast<void**>(g_uiSingletonPtr);
                return ui != nullptr && g_isMenuOpen(ui, g_pauseMenuName);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool TryResolvePauseMenuName(GetPauseMenuNameFn getName) noexcept
        {
            if (getName == nullptr) return false;
            __try {
                g_pauseMenuName = getName();
                return g_pauseMenuName != nullptr
                    && *reinterpret_cast<void* const*>(g_pauseMenuName) != nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool ComputeRawSignal() noexcept
        {
            // When the engine path resolved, it is authoritative. Cursor
            // heuristics are used only as a cross-version fallback.
            if (g_cfg.useEnginePauseMenu && g_engineDetectionReady) {
                return g_enginePauseMenu.load(std::memory_order_relaxed);
            }
            if (g_cfg.requireForeground && !g_foreground.load(std::memory_order_relaxed)) {
                return false;
            }
            bool raw = false;
            if (g_cfg.useCursorVisible) raw = raw || g_cursorVisible.load(std::memory_order_relaxed);
            if (g_cfg.useCursorClip)    raw = raw || g_cursorClipped.load(std::memory_order_relaxed);
            return raw;
        }

        void PollLoop()
        {
            ::SetThreadDescription(::GetCurrentThread(), L"PauseLODHold.MenuWatcher");

            bool rawLast = false;
            auto rawSince = std::chrono::steady_clock::now();

            // UI::IsMenuOpen("PauseMenu") is authoritative, so delaying its
            // OPEN transition merely leaves an avoidable window in which LOD
            // work can run. Debounce is retained only for cursor fallback.
            const bool exactEngineSignal =
                g_cfg.useEnginePauseMenu && g_engineDetectionReady;
            const auto openDebounce = std::chrono::milliseconds(
                exactEngineSignal ? 0 : g_cfg.openDebounceMs);
            const auto closeDebounce = std::chrono::milliseconds(
                exactEngineSignal ? 0 : g_cfg.closeDebounceMs);

            while (!g_shutdown.load(std::memory_order_relaxed)) {
                const auto now = std::chrono::steady_clock::now();

                UpdateQuitRequested();

                // Once the engine wants to quit, this thread STOPS ENTIRELY.
                //
                // Latching the flag was not enough. QueryEnginePauseMenu below
                // calls the engine's UI::IsMenuOpen every 10ms on a background
                // thread; continuing to do that while the game dismantles its
                // UI means calling into subsystems that are being destroyed,
                // and contending for their locks exactly when shutdown wants
                // them. A tester A/B'd a slow exit that stopped when the plugin
                // was removed, with the LOD redirects already guarded — this
                // polling was the remaining engine contact.
                //
                // Returning also ends the thread, so nothing of ours is left
                // running for the rest of the shutdown.
                if (g_quitRequested.load(std::memory_order_relaxed)) {
                    g_enginePauseMenu.store(false, std::memory_order_relaxed);
                    g_menuOpen.store(false, std::memory_order_release);
                    spdlog::info("[menu] watcher stopped — no further engine calls will be made "
                                 "from this plugin during shutdown");
                    return;
                }

                g_foreground.store(ForegroundIsOurProcess(), std::memory_order_relaxed);
                const bool enginePauseMenu = QueryEnginePauseMenu();
                g_enginePauseMenu.store(enginePauseMenu, std::memory_order_relaxed);

                // 1) Cursor visibility — fullscreen play hides the cursor; menus
                // show it. CURSOR_SUPPRESSED means "shown, but hidden because the
                // user is on touch/gamepad input" — that is not a menu signal.
                CURSORINFO ci{ sizeof(CURSORINFO), 0, nullptr, {} };
                bool cursorVisible = false;
                if (::GetCursorInfo(&ci)) {
                    cursorVisible = (ci.flags & CURSOR_SHOWING) != 0;
                }
                g_cursorVisible.store(cursorVisible, std::memory_order_relaxed);

                // 2) Cursor clip rect — recorded for diagnosis, but OFF by default
                // as a decision input. See Settings::useCursorClip for why.
                RECT clip{};
                bool cursorClipped = false;
                if (::GetClipCursor(&clip)) {
                    int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
                    long w = clip.right - clip.left;
                    long h = clip.bottom - clip.top;
                    cursorClipped = (w < vw && h < vh) && (w < vw - 32 || h < vh - 32);
                }
                g_cursorClipped.store(cursorClipped, std::memory_order_relaxed);

                // 3) Debounce the raw signal, then emit transitions on the
                // debounced state. rawSince tracks how long raw has held its
                // current value; the menu only flips once that exceeds the
                // relevant threshold — so short flickers never reach the hook.
                const bool raw = ComputeRawSignal();
                if (raw != rawLast) {
                    rawLast  = raw;
                    rawSince = now;
                }
                const auto stable = now - rawSince;
                const bool open = g_menuOpen.load(std::memory_order_relaxed);

                if (!open && raw && stable >= openDebounce) {
                    // Publish order is load-bearing: openMs first, then
                    // sessionId (release), then menuOpen (release). An acquire
                    // load of sessionId that returns N is thereby guaranteed
                    // to see openMs(N), and an acquire load of menuOpen==true
                    // sees both. Without this, a renderer-thread visit could
                    // pair the NEW session id with the PREVIOUS session's
                    // open/close timestamps — which satisfies the hook's
                    // short-pause release catch and consumes the one-shot
                    // post-pause Reset at the OPEN edge, silently skipping the
                    // discard at the real close.
                    g_lastOpenMs.store(NowMs(), std::memory_order_relaxed);
                    g_sessionId.fetch_add(1, std::memory_order_release);
                    g_menuOpen.store(true, std::memory_order_release);
                    spdlog::info("[menu] OPEN  session={}  pauseMenu={} cursorVisible={} "
                                 "cursorClipped={} foreground={}  (stable {}ms)",
                                 g_sessionId.load(std::memory_order_relaxed),
                                 enginePauseMenu, cursorVisible, cursorClipped,
                                 g_foreground.load(std::memory_order_relaxed),
                                 std::chrono::duration_cast<std::chrono::milliseconds>(stable).count());
                } else if (open && !raw && stable >= closeDebounce) {
                    auto ms = NowMs();
                    auto openMs = g_lastOpenMs.load(std::memory_order_relaxed);
                    g_lastCloseMs.store(ms, std::memory_order_relaxed);
                    // Publish CLOSE after its timestamp for the same reason.
                    g_menuOpen.store(false, std::memory_order_release);
                    auto durationMs = (openMs > 0) ? (ms - openMs) : 0;
                    spdlog::info("[menu] CLOSE session={} after {} ms",
                                 g_sessionId.load(std::memory_order_relaxed), durationMs);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.pollMs));
            }
        }
    }

    bool Install(const Settings& settings, const std::vector<std::string>& suspendingMenus)
    {
        g_cfg = settings;
        g_configuredMenus = suspendingMenus;
        g_engineDetectionReady = false;
        g_uiSingletonPtr = 0;
        g_isMenuOpen = nullptr;
        g_pauseMenuName = nullptr;

        if (g_cfg.useEnginePauseMenu) {
            const auto moduleBase =
                reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            const auto uiRva = slh::AddressLib::LookupRVA(kIdUISingleton);
            const auto isOpenRva = slh::AddressLib::LookupRVA(kIdUIIsMenuOpen);
            const auto getNameRva = slh::AddressLib::LookupRVA(kIdPauseMenuName);
            if (moduleBase != 0 && uiRva != 0 && isOpenRva != 0 && getNameRva != 0) {
                g_uiSingletonPtr = moduleBase + uiRva;
                g_isMenuOpen = reinterpret_cast<IsMenuOpenFn>(moduleBase + isOpenRva);
                auto getName =
                    reinterpret_cast<GetPauseMenuNameFn>(moduleBase + getNameRva);
                g_engineDetectionReady = TryResolvePauseMenuName(getName);
            }
        }

        {
            const auto moduleBase =
                reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
            const auto rva = slh::AddressLib::LookupRVA(kIdMainSingleton);
            if (moduleBase != 0 && rva != 0) {
                g_mainSingletonPtr = moduleBase + rva;
                spdlog::info("[menu] resolved Main::Singleton: ID {} -> RVA 0x{:x}; byte +0x{:x} "
                             "is the engine's quit-requested flag and is the shutdown signal "
                             "for the LOD freeze",
                             kIdMainSingleton, rva, kMainQuitFlagOffset);
            } else {
                spdlog::error("[menu] Main::Singleton (ID {}) did not resolve — the LOD freeze "
                              "has NO shutdown signal on this runtime and will be disabled",
                              kIdMainSingleton);
            }
        }

        if (g_cfg.useEnginePauseMenu && g_engineDetectionReady) {
            spdlog::info("[menu] engine detector resolved: UI::IsMenuOpen(\"PauseMenu\")");
        } else if (g_cfg.useEnginePauseMenu) {
            spdlog::warn("[menu] engine PauseMenu detector unavailable — falling back to cursor signals");
        }
        spdlog::info("[menu] watcher installed.  signals: enginePause={} (ready={}) "
                     "cursorVisible={} cursorClip={} requireForeground={}  "
                     "effective debounce open/close={}/{}ms  poll={}ms",
                     g_cfg.useEnginePauseMenu, g_engineDetectionReady,
                     g_cfg.useCursorVisible, g_cfg.useCursorClip, g_cfg.requireForeground,
                     g_engineDetectionReady ? 0 : g_cfg.openDebounceMs,
                     g_engineDetectionReady ? 0 : g_cfg.closeDebounceMs,
                     g_cfg.pollMs);
        spdlog::info("[menu] configured menu names ({}) are informational; active target is PauseMenu",
                     suspendingMenus.size());
        return true;
    }

    // The hook reads the DEBOUNCED state, not the raw cursor signal — so a
    // momentary cursor flicker during gameplay never triggers suppression.
    bool IsSuspendingMenuOpen() noexcept { return g_menuOpen.load(std::memory_order_acquire); }

    bool IsExactPauseMenuOpen() noexcept
    {
        return g_engineDetectionReady
            && g_menuOpen.load(std::memory_order_acquire);
    }

    bool ExactPauseMenuDetectionReady() noexcept { return g_engineDetectionReady; }

    bool EngineShutdownRequested() noexcept
    {
        return g_quitRequested.load(std::memory_order_acquire);
    }

    bool QuitDetectionReady() noexcept { return g_mainSingletonPtr != 0; }

    // Acquire pairs with the release fetch_add in the OPEN transition: a
    // reader that observes session N also observes openMs(N).
    std::uint64_t SessionId() noexcept { return g_sessionId.load(std::memory_order_acquire); }

    std::string CurrentMenuName()
    {
        if (g_enginePauseMenu.load(std::memory_order_relaxed)) return "PauseMenu";
        if (g_cfg.useCursorVisible && g_cursorVisible.load(std::memory_order_relaxed)) return "<cursor:visible>";
        if (g_cfg.useCursorClip    && g_cursorClipped.load(std::memory_order_relaxed)) return "<cursor:clipped>";
        return "";
    }

    std::int64_t LastOpenTimeMs() noexcept  { return g_lastOpenMs.load(std::memory_order_relaxed); }
    std::int64_t LastCloseTimeMs() noexcept { return g_lastCloseMs.load(std::memory_order_relaxed); }

    void Start()
    {
        if (g_started.exchange(true)) return;
        g_shutdown.store(false);
        std::thread(PollLoop).detach();
    }

    void Stop()
    {
        // Detached thread: signal only; it exits within one poll interval.
        g_shutdown.store(true);
    }
}
