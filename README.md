# StarfieldPauseLODHold

SFSE plugin that freezes the **currently displayed mesh and texture LOD levels**
while Starfield's exact `PauseMenu` is open. The player/camera is stationary,
so promotion and degradation work is unnecessary until gameplay resumes.

**Version coverage:** the freeze requires the matching Starfield Address
Library `versionlib-<v>-<v>-<v>-<v>.bin` beside the plugin. It resolves the
exact `PauseMenu` detector and all six LOD state boundaries by ID. If any one
does not resolve, the freeze fails open and the log says it is disabled.

The older CheckBudget diagnostic hook additionally has a byte-signature scan
and a hardcoded 1.16.236/1.7 RVA fallback. Those fallbacks keep telemetry
available without a versionlib; they cannot provide the complete freeze.

Cross-version verification done at build time:

| Version    | CheckBudget::Func4 RVA | Budget singleton RVA |
|------------|------------------------|----------------------|
| 1.7        | 0x02a4bfa0             | 0x06202a98           |
| 1.16.236   | 0x02a4bfa0 (unchanged) | 0x06202a98           |
| 1.16.242   | 0x02a4c250 (+0x2b0)    | 0x06202c58 (+0x1c0)  |
| 1.16.244   | 0x02a47cd0              | 0x061fac30           |

The signature scanner has wildcards on the disp32 fields, so it should
match 1.16.242's prologue too if the user lacks the versionlib bin —
but with the bin present, the Address Library path resolves directly.

## v0.9 — same fix, made provable (current tester build)

v0.9 changes no part of the freeze mechanism. It exists because the v0.8 tester
session (two runs, 16 pauses) could not answer the question it was sent to
answer. What the v0.8 data does show, quantitatively:

- Every pause behaved exactly as designed: 16/16 freezes engaged within 13–64 ms
  of the menu opening, all 15 completed closes were covered by the correct
  number of release Resets (back-to-back reopens 10–20 ms apart coalesced into
  one, as intended), and no legacy write ever fired.
- **No residual cost after unpause was measurable.** LOD-cycle rate returned to
  its pre-pause value (63.4/s before → 63–68/s after in run 1; 14.2–14.6/s on
  both sides of 14 pauses in run 2), tracked VRAM was bit-identical across the
  pause (1819 MB → 1819 MB; 6408 MB → 6408 MB), and mode never escalated
  because of a pause.
- The severe slowdown visible in run 2 (rate 250/s decaying to 14/s, VRAM 6408 MB
  against a 2658 MB target) was established entirely by **city streaming before
  the first pause**, with the engine's own emergency demotion reclaiming nothing.
  Pausing did not worsen it. The v0.6 logs corroborate this: the same explosion
  fired three times with no menu open at all.

So the honest position is that the pause-side regression is gone, and what
remains is not something the LOD state machine causes. Three gaps blocked any
stronger claim, and v0.9 closes all three:

1. **The pause interval was dark.** The frozen hook returned before the
   telemetry block, so a pause produced one line at open and one at close. Since
   pause *length* is the reported bug's strongest predictor, that was the one
   interval worth logging. There is now a heartbeat while frozen.
2. **The OS's view of VRAM was never sampled.** The engine tracker cannot see
   WDDM evicting an idle process's allocations — which is what "worse the longer
   you pause, and only on cards below ~12 GB" actually looks like. Every timeline
   row and pause report now carries `os vram used/budget` from
   `IDXGIAdapter3::QueryVideoMemoryInfo`. `dxgi.dll` is loaded by name at
   runtime, so the plugin gains no load-time dependency.
3. **Nothing in the log stated an outcome.** Each pause now writes one
   `[pause-report]` line comparing the 10 s before the pause against 0–1 s,
   1–5 s and 5–15 s after it, ending in `recovery=EXACT|OK|DEGRADED`. The
   VERDICT block is emitted every 60 s instead of only at process detach — which
   never fired: Starfield fast-exits, so no tester log in eight runs ever
   contained it.

One real defect was fixed. `MenuWatcher` published the session id before the
open timestamp, so a renderer visit at the open edge could pair the new session
with the previous session's close timestamp, satisfy the short-pause release
catch, and consume the one-shot post-pause discard *while the menu was opening* —
leaving the real close with no discard at all. Publication is now
openMs → sessionId (release) → open flag, with acquire loads in the hook.
Also: the native Reset call is SEH-guarded and fails to native for the rest of
the session rather than crashing if an address ever resolves wrongly; the six
resolved state addresses are checked for aliasing and for being within 1 MB of
`CheckBudget` (they are members of one class, emitted together); and the log now
starts fresh each launch, keeping the previous run as `*.prev.log`.

Rejected after review, with the evidence against each:

- **Re-adding v0.7's post-close upgrade-budget cooldown.** v0.7's own log shows
  a +421 MB upload in 0.6 s *while* those budgets were forced to zero. The
  upload path does not consume them.
- **A post-close mode-1 degrade hold.** There is no measured post-close deficit
  to remove, and the window it would act in is exactly when a real upload burst
  would begin — the shape of the v0.4 regression.
- **Skipping the release Reset on very short pauses.** Zero measured benefit,
  and cycle throughput collapses during the stuck state, so "no hooked visit
  happened" is *more* likely mid-incident — it would remove Resets precisely
  when Esc-spam (repeated Resets) is the only thing users report working.
- **Hooking `GatherMaterials`/`SortLODs` as well.** Defensible, but deferred: if
  `Reset::Func3` enters the next state synchronously the way `CheckBudget` does,
  redirecting `GatherMaterials` back into `Reset` recurses. That needs a Ghidra
  pass on `Reset::Func3` first, and it would buy nothing measurable today.

## v0.8 — exact PauseMenu LOD-level freeze

The appended v0.7 tester session showed two different events:

- The first real pause left tracked VRAM fixed at 530 MB and mode 1, yet the
  tester reported 29 FPS becoming 22/23 FPS.
- A later pause was a separate pressure event: tracked usage rose from 1.4 GB
  to 6.3 GB, entered native emergency mode 3, and did not recover.

It also proved the tested DLL was stale: startup said detection was heuristic,
not `UI::IsMenuOpen("PauseMenu")`, and every intervention began about 300 ms
late. More importantly, v0.7 was not a true freeze. Zero byte budgets are
checked only after a state has visited up to 100 materials, `CheckBudget`
refills them every cycle, and `BalanceTextureLODs` can submit detail changes
directly. Repeated cycles can therefore keep changing LODs.

v0.8 freezes the level itself. There is no standalone FPS value used by this
controller; the equivalent of “ignore pause FPS” is to execute no LOD-changing
state while paused:

1. Resolve engine `UI::IsMenuOpen("PauseMenu")`; cursor fallback is never
   trusted for the freeze.
2. While it is open, redirect `CheckBudget`, `UpgradeMeshes`,
   `UpgradeTextures`, `DegradeTextures`, `BalanceTextureLODs`, and
   `DegradeMeshes` through native `Reset::Func3`.
3. `Reset::Func3` clears pending budgets/iterators, destroys the current state,
   and advances safely to `GatherMaterials`. It does **not** change the current
   resident mesh or texture LOD.
4. On close, do one Reset to discard coverage data sampled during the
   stationary pause. The next fresh gameplay cycle runs normally. There is no
   timed post-close cap.
5. All v0.5–v0.7 budget writes are forced off while this mode is enabled.

Address Library IDs used by the freeze:

| State | ID |
|---|---:|
| `UI` singleton | 937580 |
| `UI::IsMenuOpen` | 130475 |
| engine-owned `BSFixedString("PauseMenu")` getter | 130409 |
| `CheckBudget::Func4` | 143698 |
| `UpgradeMeshes::Func4` | 143703 |
| `UpgradeTextures::Func4` | 143706 |
| `DegradeTextures::Func4` | 143709 |
| `BalanceTextureLODs::Func4` | 143711 |
| `DegradeMeshes::Func4` | 143714 |
| native `Reset::Func3` transition | 143715 |

### Tester protocol

Fully exit Starfield and replace both the DLL and INI. No log housekeeping is
needed any more — the log starts fresh each launch and the previous run is kept
as `StarfieldPauseLODHold.prev.log`.

The longest pause in the v0.8 data was 6.6 s, but the reported bug is described
as getting worse the longer you pause. **Long pauses are the missing
measurement.** In one consistent dense-city exterior save:

1. Play for a minute so the city is fully streamed in.
2. Pause and wait **at least 60 s, ideally 3 minutes**, then unpause and note
   whether FPS returns.
3. Repeat with roughly 10 s and roughly 5 minutes.
4. A few quick Esc taps for comparison.
5. Quit and send the log, saying which pause misbehaved if any did.

The startup log must contain `engine detector resolved` and
`exact PauseMenu state freeze=true`. A pause that reaches an LOD-changing state
contains one `[freeze] ACTIVE` row; every detected pause should eventually
produce one `[freeze] RELEASE` row. A pause can legitimately have only
`RELEASE` if the paused state machine never visits a hooked action boundary.
Legacy counters `upgradeCaps`, `degradeHolds`, and `refreshCleared` must remain
zero, and no `[verdict]` line may report a faulted Reset.

The line that answers the question is `[pause-report]`. `recovery=EXACT` means
the LOD-cycle rate came back to at least 95% of its pre-pause median with no
mode escalation; `DEGRADED` means it stayed below 75%. If a report says
`DEGRADED`, compare `vram MB:` against `os vram MB:` on the same line — engine
usage flat while the OS numbers moved is eviction, not an engine upload, and no
hook in this plugin can prevent it.

## v0.6 — refreshAll experiment (superseded)

Everything before v0.6 intervened on the **demote** path. The reported bug is a
*VRAM spike to 100%* — an **upload** event. Wrong half of the system.

`UpgradeTextures::Func4` (RVA 0x2a4cb90):

```c
if (state[0x38c0] /* refreshAll */ == 0) {
    mip = <from per-material screen coverage>;   // incremental
} else {
    uVar15 = 0;                                  // force mip 0 — FULL RES, every material
}
```

`refreshAll` is set in exactly one place: `CheckBudget`'s **mode 0** branch.
When it fires, every resident material is forced to full resolution in one
sweep. On a card with no spare VRAM that is the 100% spike, and everything that
follows — driver paging, single-digit FPS, no recovery.

This hypothesis mapped onto the reported symptoms, but the v0.6 tester trace
falsified it for the affected machine: `refreshAll` remained zero throughout
all three sessions.

It also explains v0.3 retroactively. Forcing mode 0 pinned `refreshAll = 1`
permanently — v0.3 did not fail to fix the bug, **it caused it**. That gives us
a reproduction on demand (see `[Repro]` in the ini).

v0.6 cleared `refreshAll` while a menu was open and for 3 s after close; v0.7
retained it as a fallback. v0.8 disables that write for a clean level-freeze
test.

This is the safe direction. Over-suppressing an *upgrade* means textures stay
blurry a moment longer. Over-suppressing a *demote* — what v0.4 did — blocks the
engine's only way back from a VRAM emergency.

## What changed in v0.5

v0.4 made frame rates **worse** on VRAM-limited cards: testers saw ~30 FPS drop
to ~2 FPS after a few seconds in a menu, and it did not recover on unpause.
`bDryRun = 1` measurably outperformed `bDryRun = 0`, which is the same as saying
the intervention itself was the problem.

v0.4 zeroed three fields after `CheckBudget` returned: `totalDegrade` (0x38b8),
`emergencyDegrade` (0x38c1) and `demoteActive` (0x389c). All three are inputs to
`DegradeTextures::Func4` (1.16.236 RVA 0x2a4ced0), and zeroing them **cripples
the engine's VRAM shed at exactly the moment it is needed**:

- `totalDegrade = 0` — this is the decrementing byte budget *and* the state's
  exit condition. At 0, DegradeTextures demotes one visit's worth of materials
  (≤100) and immediately returns to `BalanceTextureLODs`, instead of staying in
  the degrade state until the overage is cleared.
- `emergencyDegrade = 0` — this is what forces every material to its lowest mip
  during an emergency (`uVar11 = 0xffff`). Without it the shed falls back to
  screen-size thresholds and reclaims a fraction as much per material.
- `demoteActive = 0` — gates the second-pass block inside DegradeTextures.

So on a card that was genuinely over budget, the engine kept trying to shed and
kept being throttled. It never got back under budget, the driver started paging
texture residency, and the mode byte stuck at 3 permanently. That matches the
tester logs exactly: with writes enabled the mode-3 sample count climbed and the
tick rate sat at ~9 calls per 5 s with no recovery after the menu closed; in dry
run the engine dipped to 23, shed, and was back to ~384 within seconds.

Note also that the state transition — including the next state's entry call —
runs *synchronously inside* the original function, so nothing the hook writes
afterwards affects that tick's dispatch; it only changes what the next tick's
state reads.

> **A caveat on one branch.** `CheckBudget`'s tail contains
> `if (high == 0 && DAT_145948618 == 0) next = demoteActive ? UpgradeTextures :
> DegradeTextures; else next = UpgradeMeshes;`. If that flag is ever 0 at
> runtime, v0.4's `emergencyDegrade = 0` would additionally flip `demoteActive`
> to 1 on a mode-3 fall-through tick and send the machine to **UpgradeTextures
> during a VRAM emergency** — a second, worse failure path. `DAT_145948618` is
> statically initialised to 1 and Ghidra finds no writers, so it is probably
> always 1 and that branch is likely dead. Either way v0.5 removes both writes,
> so the fix does not depend on which is true.

**v0.5 writes one field, in one mode.** `emergencyDegrade` and `demoteActive`
are never written again. See "The fix" below.

## What the bug is, in one paragraph

Starfield's `ScreenSizeBasedLOD::LODStateMachine` ticks a state machine through
`CheckBudget → BalanceTextureLODs → DegradeTextures → UpgradeTextures`. The
trigger is a "budget mode" byte that climbs from 0 to 1/2/3 under VRAM pressure.
On 16 GB cards there's enough headroom that the mode never crosses 0; on
8-10 GB cards in dense scenes the menu's working-set push tips it over, and
the state machine starts demoting world texture mips at ~100 materials/tick.
On unpause the inverse `UpgradeTextures` path has to re-stream those mips
from BA2 — that's the 2-FPS stall. Spamming Esc works because the state
machine doesn't accumulate across ticks; rapid open/close never lets the
degrade-budget reach nonzero on any single tick.

Full analysis with addresses and decompiles is in
`C:\Development\Cell Offset Generator Starfield\` agent history; this plugin
is the productionised fix.

## The v0.5/v0.7 budget policy (historical; disabled in v0.8)

One hook on `ScreenSizeBasedLOD::State::CheckBudget::Func4` (1.16.236 RVA
`0x02a4bfa0`; resolved by Address Library on other versions). The hook
**always calls the original** — CheckBudget computes the LOD budgets, picks the
next state and enters it; skipping it deadlocks the dispatcher.

Then, and only when **all** of the following hold, it writes a single field:

| Gate | Why |
|---|---|
| A menu is open | that's the point |
| Budget mode is exactly 1 | mode 0 already schedules no demotion; modes 2/3 are real VRAM pressure the engine must be allowed to resolve |
| At least 1 GB below the VRAM target | repeated-pause trace crossed mode 3 three seconds after falling below this reserve |
| No escalation this menu | if the mode ever climbs above `iMaxHoldMode` during a menu, the hold stands down for the rest of it |
| Under the per-menu time cap | default 20 s |
| Watchdog has not tripped | see below |

The write is `totalDegrade` (state+0x38b8) `= 0`.

`DegradeTextures::Func4` consumes that field as a decrementing byte budget
*and* uses it as its exit condition — it stays in the DegradeTextures state,
demoting up to 100 materials per visit, until the budget reaches 0 or the
material list is exhausted, then returns to `BalanceTextureLODs`. Zeroing it
caps demotion at a single visit's worth instead of a sustained multi-tick run.

Mode 1 is the right and only place for this. `CheckBudget`'s mode-1/2 branch
computes `totalDegrade = max(overage, DAT_1459485b8 << 20)` — note the **floor**:
even with no real overage the engine schedules a minimum degrade budget every
tick. That floor is what demotes your textures while you sit in a menu with
headroom to spare, and it is what this plugin removes.

`emergencyDegrade` (0x38c1), `demoteActive` (0x389c), and the mode byte are
**never written**. v0.7 separately capped the texture/combined upgrade budgets
(`0x38a8/0x38b0`) and retained the v0.6 `refreshAll` clear as a fallback.
All of these legacy writes are off when `bFreezeLODWhilePaused=1`.

### The legacy watchdog

The plugin learns the hook's tick rate while no menu is open. If, while holding,
the rate stays below `iWatchdogPercent` of that baseline for `iWatchdogTripMs`,
it latches off for the remainder of the menu and logs why. So even if the mode
gate is wrong on some future game build, a harmful intervention self-limits to
about a second and a half instead of persisting until you quit.

### Menu detection

1. **Engine-side `UI::IsMenuOpen("PauseMenu")`** — primary. Address Library
   resolves `UI::Singleton`, `UI::IsMenuOpen`, and the engine-owned static
   `BSFixedString("PauseMenu")` getter for the active runtime. Main menu,
   inventory, and other cursor-visible screens no longer arm the hook.
2. **Cursor visibility** (`GetCursorInfo`) — fallback if any of those IDs fail
   to resolve. Fullscreen play hides the cursor; menus show it.
3. **Foreground-window ownership** — applied to cursor fallback only. The
   exact engine `PauseMenu` state remains authoritative even if focus changes.
4. **Cursor clip rect** (`GetClipCursor`) — **off by default.** On a
   multi-monitor desktop the game confines the cursor to its own monitor during
   normal gameplay, so this reads true forever; it is what made v0.4 treat
   25-46% of all frames as "in a menu". Kept behind `bUseCursorClip` for
   diagnosis only.

The plugin is safe to leave installed at all times. v0.8 redirects LOD states
only while exact `PauseMenu` is open, regardless of budget mode; outside it,
normal LOD operation resumes immediately after the one release Reset.

## Install — tester quick path

```
1. Copy StarfieldPauseLODHold.dll       → Data\SFSE\Plugins\
2. Copy StarfieldPauseLODHold.ini       → Data\SFSE\Plugins\
3. Launch via SFSE (sfse_loader.exe)
4. After playing/pausing, check log at:
       Data\SFSE\Plugins\StarfieldPauseLODHold.log
```

Every setting is documented inline in the `.ini`. The defaults are what we test
with; the only switch most testers need is `[General] bDryRun`.

## Reading the log

One pause looks like this:

```
[menu] OPEN  session=3  pauseMenu=true ... (stable 0ms)
[freeze] ACTIVE session=3 — skipped CheckBudget and used native Reset | mode=1 ... | os vram used/budget=5100/7900MB
[tl] mode=1 menu=true ... act=FROZEN ... | os vram used/budget=5100/7900MB
[menu] CLOSE session=3 after 30000 ms
[freeze] RELEASE session=3 — discarded pause-sampled work at CheckBudget with one native Reset ...
[pause-report] session=3 pause=30000ms | LOD-cycle rate/s: pre(10s median)=63.4 post 0-1s=61.0 1-5s=64.2 5-15s=63.8 | vram MB: pre=1819 at-close=1819 now=1863 (target 2728) | mode: pre=1 now=1 | os vram MB: pre=5100 at-close=5090 now=5140 (budget pre=7900 now=7900) | recovery=EXACT
[summary] ... pausedStateSkips=412 (+73) postPauseResets=3 upgradeCaps=0 degradeHolds=0 ...
```

What each tells you:

- **`ACTIVE`** — an LOD-changing native state was discarded; the visible
  resident levels were left untouched.
- **`[tl] act=FROZEN`** — heartbeat during the pause. New in v0.9; this interval
  used to be invisible.
- **`RELEASE`** — one boundary Reset discarded pause-sampled controller work.
  This happens once, not for a timed cooldown.
- **`[pause-report]`** — the verdict for that pause. `recovery=EXACT` means the
  LOD-cycle rate returned to ≥95% of its pre-pause median with no mode
  escalation, `OK` means ≥75%, `DEGRADED` means it did not come back.
- **`os vram`** — the OS's own accounting, which the engine tracker cannot see.
  Engine usage flat while these numbers move means the OS evicted and re-paged
  our allocations; that is outside anything this plugin hooks.
- **`pausedStateSkips`** — total promote/demote/balance opportunities blocked
  during exact PauseMenu sessions. It counts all six hooked states, so it
  advances several times per LOD cycle and is not comparable 1:1 with `calls`.
- Legacy `upgradeCaps`, `degradeHolds`, and `refreshCleared` should stay zero.

Note that `rate`/`calls` is LOD state-machine **cycle throughput**, not FPS. It
tracks render-thread health well (it craters during stalls) but its absolute
value is not a frame rate.

### What "working" looks like

Every detected pause has one `RELEASE`; pauses with an LOD-changing state visit
also have one `ACTIVE`. `postPauseResets` increments once per pause, all
legacy-write counters stay zero, every `[pause-report]` says `recovery=EXACT`,
and FPS returns to the same range after repeated and long pauses.

## Address resolution

The complete freeze resolves all detector and state functions through
Address Library IDs. The CheckBudget diagnostic hook separately has two
fallback paths:

1. **Address Library IDs** (required for the freeze): `130409`, `130475`, and
   `937580` for exact PauseMenu detection; `143698`–`143715` for the freeze
   boundaries; and `944397` for budget telemetry.
2. **CheckBudget::Func4 by signature scan**. The compiled-in
   pattern matches the function's prologue, with disp32 fields wildcarded.
   Look for log line:
   ```
   [scan] CheckBudget::Func4: matched at 0x... (RVA 0x...)
   ```
3. **CheckBudget RVA fallback** if the scan misses (signature changed by
   patch). Default is the verified `0x02a4bfa0`. Look for:
   ```
   [hook] signature scan missed — trying RVA fallback 0x...
   ```
4. **Budget singleton fallback**: decoded from the prologue's
   `48 8B 05 <disp32>` instruction if its Address Library lookup missed.
   Look for:
   ```
   [hook] budget singleton via RIP-relative decode at 0x... (RVA 0x...)
   ```

If both signature and RVA fail, the hook aborts and logs:
```
[hook] RVA fallback at 0x... also invalid (first byte = 0xcc); aborting
```

**To update for a future patch that breaks both**: open the new
Starfield.exe in Ghidra, find RTTI symbol
`CreationRendererPrivate__ScreenSizeBasedLOD____State__CheckBudget`, walk
its vtable to slot [4]. Subtract `0x140000000` for the RVA and set
`Config::checkBudgetFunc4RvaFallback` in `src/Hook.h`, or update
`kDefaultSigCheckBudget` in `src/main.cpp`.

Four safety interlocks depend on the resolution succeeding:

- The **prologue check** validates the bytes that encode the tracker (+0xf8)
  and mode (+0x28) offsets. On mismatch the hook installs but never writes.
- If the **budget singleton** can't be resolved, the mode gate is unavailable,
  so the hold is disabled outright rather than run blind. (v0.4 downgraded this
  to "mode-byte logging disabled" and kept writing — it no longer does.)
- The six freeze addresses are checked for **aliasing** and for lying within
  1 MB of `CheckBudget`. They are members of one class emitted together, so a
  stale versionlib that resolves an unrelated function is caught here instead of
  crashing on the first pause. The freeze is disabled, with a named error line.
- The native Reset call is **SEH-guarded**. A fault disables the freeze for the
  rest of the session, lets the original state run, and reports it in the
  VERDICT block — a mis-resolved address degrades to observation-only rather
  than a crash that would be blamed on the bug.

## Build

Requires VS 2022 + vcpkg (`C:\vcpkg` by default; set `VCPKG_ROOT` or pass
`-DCMAKE_TOOLCHAIN_FILE` to redirect).

```powershell
cmake -S . -B build/vs2022-x64 -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build/vs2022-x64 --config Release
```

Output: `build/vs2022-x64/Release/StarfieldPauseLODHold.dll` is auto-copied to
`C:\Games\Starfield 1.7\Data\SFSE\Plugins\` after build (set `SF_DEPLOY_DIR`
to your install path to redirect).

## Caveats

- The primary detector calls engine-side `UI::IsMenuOpen("PauseMenu")`.
  Cursor visibility remains available for diagnostics, but the state freeze
  fails open if exact detection is unavailable.
- The freeze uses native state functions resolved through Address Library rather
  than writing the controller's raw budget fields.
- Work already submitted before PauseMenu is observed cannot be cancelled.
  Exact detection has zero debounce and a 10 ms poll interval to keep that
  window small. A pause shorter than one poll produces no session at all, so it
  gets neither a freeze nor a release.
- While paused, native emergency shedding is also frozen because it changes the
  visible LOD level. It resumes immediately on close after the one Reset. On a
  card genuinely over budget, a long pause therefore exits with whatever overage
  accumulated during it — the engine gets its relief valve back at close, not
  before.
- `GatherMaterials` and `SortLODs` are **not** hooked, so they keep running
  during a pause and sample the menu view. The one release Reset at close is
  what discards that; there is no second line of defence if it is ever missed.
- Sample publication assumes all six hooked states run on the renderer thread.
  The ring claims indices atomically so a second producer would lose its own
  sample rather than corrupt another's, but a torn sample is still possible if
  that assumption is wrong.
- `Reset::Func3` is called with whichever state object the dispatcher was
  executing as `this`. That it is class-agnostic, shares the 4-argument shape,
  and does not re-enter a hooked function is inferred from ~2,150 successful
  redirects on 1.16.236/1.16.244, not from decompilation. Verifying it in Ghidra
  is the outstanding prerequisite for hooking `GatherMaterials`/`SortLODs`.
