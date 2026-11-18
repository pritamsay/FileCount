# FileCount 1.1.3 validation

Tested on the development Windows 11 PC on 2026-11-18.

- `Tests\run.cmd`: all 16 checks passed, including deliberately slow background work, the two-worker limit, cancellation, and queued completion.
- An early candidate passed 16 Explorer refresh checks across Details and Icons views, in ascending and descending order. Computer Use showed folder badges 10, 1, and 0, and no badge on the ordinary file.
- The final installed build passed another eight small-folder refresh checks. File count stayed primary throughout; the folder counts were populated and numerically sorted. Completion was approximately 1.0–2.3 seconds in this fixture.
- The final installed build passed refresh in both directions with 403 folders, retaining File count as primary. Completion took approximately 6.8 and 14.5 seconds under the probe's extra COM traffic. This checks correctness under load; it is not a latency guarantee. The first stress attempt was discarded because the Explorer window was minimized and updates were paused.
- MSI upgrade returned 0. Windows Installer reported 1.1.3 installed and 1.1.2 absent. The installed EXE matched the built EXE (SHA-256 `C3273F7E16BEB9BC2DF4E1FEFC4AB35D3C62B2248694483C887A561141AF5C9A`).
- Kaspersky scanned the final EXE/MSI: five objects OK, zero detections, suspicions, skipped objects, or errors. This is a result for this build and scan, not an antivirus guarantee.

Final visual rechecking of scroll/overlap behavior and the user's network folder remains incomplete: the Computer Use helper reported an active-request error after retries and reset. The earlier visual check was on a candidate before the final layout/cache adjustments. An additional heartbeat diagnostic could not run because Windows reported its executable was locked. No successful final heartbeat measurement is claimed.

## Antivirus finding on an optional diagnostic

Kaspersky subsequently flagged the local `read_probe.exe` diagnostic as `VHO:Trojan-PSW.Win32.Crypt.gen`. That executable is not included in this repository, the MSI, or the installed app. It was removed and was not rebuilt, renamed, excluded, or run after the alert. This finding has not been established as a false positive.

The unchanged installed FileCount EXE, built EXE, and release MSI were rescanned after that finding: all three objects were OK, with zero detections, skipped objects, or errors. The installed helper was stopped while the user considers whether to resume it. Antivirus settings and exclusions were not changed.

The generated 400-folder stress data was removed from the Explorer test fixture. No user folders or file contents were changed.
