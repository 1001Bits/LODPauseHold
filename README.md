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

close=5090 now=5140 (budget pre=7900 now=7900) | recovery=EXACT
[summary] ... pausedStateSkips=412 (+73) postPauseResets=3 upgradeCaps=0 degradeHolds=0 ...
```

