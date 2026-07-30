#pragma once
#include "PCH.h"

namespace slh::MenuWatcher
{
    struct Settings
    {
        // Preferred path: call UI::IsMenuOpen("PauseMenu") through Address
        // Library IDs. This scopes intervention to the actual pause menu and
        // prevents MainMenu/inventory cursor visibility from arming the hook.
        bool useEnginePauseMenu = true;

        // Cursor visibility is the one Win32 signal that actually tracks
        // Starfield's menus: fullscreen play hides the cursor, menus show it.
        // It is now a fallback only when engine-side resolution fails.
        bool useCursorVisible = true;

        // Cursor CLIP rect is NOT a menu signal. On a multi-monitor desktop
        // the game confines the cursor to its own monitor during normal
        // gameplay, so the clip rect is permanently smaller than the virtual
        // screen and this "signal" reads true forever. It produced the
        // 25-46%-of-all-frames false-positive rate in the v0.4 tester logs.
        // Off by default; kept behind a flag for diagnosis only.
        bool useCursorClip = false;

        // Ignore cursor state unless the foreground window belongs to us —
        // stops alt-tab, overlays and recording software from faking a menu.
        bool requireForeground = true;

        // Exact engine detection is used without debounce. These values apply
        // only to the cursor fallback, where filtering false positives matters.
        std::uint32_t openDebounceMs  = 300;
        std::uint32_t closeDebounceMs = 150;
        std::uint32_t pollMs          = 10;
    };

    // Initialise. The menu-name list is informational; the primary predicate
    // is the engine's exact PauseMenu name, with heuristics fallback-only.
    bool Install(const Settings& settings, const std::vector<std::string>& suspendingMenus);

    // Atomic predicate read by the CheckBudget hook on the engine's renderer
    // thread. The hook is on the hot path; this must be branch-free fast.
    [[nodiscard]] bool IsSuspendingMenuOpen() noexcept;

    // True only when the engine-side detector resolved and the debounced
    // active signal is specifically PauseMenu. The LOD freeze never trusts
    // cursor fallback.
    [[nodiscard]] bool IsExactPauseMenuOpen() noexcept;
    [[nodiscard]] bool ExactPauseMenuDetectionReady() noexcept;

    // Monotonic id, incremented on every OPEN transition. The hook uses it to
    // scope its arm/latch state to a single menu session.
    [[nodiscard]] std::uint64_t SessionId() noexcept;

    // True once the engine's own quit-requested flag (Main::Singleton + 0x28)
    // is set. Quit-to-desktop is chosen from INSIDE the pause menu, so the menu
    // never closes and the LOD freeze would otherwise keep redirecting into the
    // engine while it tears itself down. Every quit route — the qqq console
    // command, the pause menu, the main menu — sets this one byte, and sets it
    // when quit is requested rather than when teardown begins, so it is both
    // universal and early. Read-only; polled on the watcher thread, the render
    // thread only reads an atomic. Latches: a requested quit is never undone.
    [[nodiscard]] bool EngineShutdownRequested() noexcept;

    // False when Main::Singleton could not be resolved for this runtime, which
    // means there is no shutdown signal and the freeze must not run.
    [[nodiscard]] bool QuitDetectionReady() noexcept;

    // For diagnostics — which signal is currently holding us open, or "".
    [[nodiscard]] std::string CurrentMenuName();

    // Open / close timestamps in steady-clock ms — used in log lines.
    [[nodiscard]] std::int64_t LastOpenTimeMs() noexcept;
    [[nodiscard]] std::int64_t LastCloseTimeMs() noexcept;

    // Install path: the watcher cannot directly hook the UI event source
    // without CommonLibSF, so it uses an active-poll path on its own thread.
    // This start function spins that thread.
    void Start();
    void Stop();
}
