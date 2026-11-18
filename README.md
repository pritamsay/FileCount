# FileCount

Show a sortable file count in Windows 11 File Explorer. In icon views, folders show a small round number at the top right while sorted by file count.

## Install and uninstall

Run these commands in Command Prompt or PowerShell on Windows x64.

**Check whether FileCount is installed**

```text
powershell.exe -NoProfile -Command "try { if ((New-Object -ComObject WindowsInstaller.Installer).ProductState('{84EA5B79-6BCE-4209-9097-C84CDB8DD07A}') -eq 5 -or (New-Object -ComObject WindowsInstaller.Installer).ProductState('{1180C4D7-58A0-48B4-BDB0-94C42594E7F2}') -eq 5 -or (New-Object -ComObject WindowsInstaller.Installer).ProductState('{B3155947-DD08-4664-A8ED-822CCC5055E6}') -eq 5 -or (New-Object -ComObject WindowsInstaller.Installer).ProductState('{00F17C7A-452D-4CDD-B47D-3416E2046183}') -eq 5) { 'FileCount is installed' } else { 'FileCount is not installed' } } catch { Write-Error 'Could not check Windows Installer'; exit 1 }"
```

This asks Windows Installer whether FileCount 1.1.0, 1.1.1, 1.1.2 or 1.1.3 is installed and prints `FileCount is installed` or `FileCount is not installed`. It works in both 32-bit and 64-bit PowerShell and recognizes installations whose uninstall entry is under HKLM. A Windows Installer lookup failure produces an error. Portable test copies are outside this check. Add the product code here when releasing another MSI version.

**Install or update FileCount 1.1.3**

```text
msiexec.exe /i "https://raw.githubusercontent.com/pritamsay/FileCount/main/dist/FileCount-1.1.3-x64.msi" /passive
```

**Uninstall FileCount 1.1.3**

```text
msiexec.exe /x "{84EA5B79-6BCE-4209-9097-C84CDB8DD07A}" /passive
```

Installation starts FileCount for the current user and adds a Startup shortcut. If you used the older portable Explorer Counts helper, exit it and remove its old Startup shortcut first.

You can also uninstall through **Settings → Apps → Installed apps → FileCount**. Uninstall closes the helper and removes the File count column and sorting from open Explorer views and recognized saved folder layouts for the current user. When File count was the only sort, Explorer switches to Name. Windows supplies the built-in **File count** entry in **More…** and continues to list it there as an available property.

## Use

1. In Explorer's Details view, right-click a column heading, choose **More…**, check **File count**, and click **OK**.
2. Click that heading to sort numerically. Click again to reverse the order. You can also choose **File count** from **Sort by**.
3. Switch to an icon view to see folder badges. Choose a different sort to hide them.

The count covers files directly inside each folder. It respects Explorer's hidden-file settings and ignores the Windows metadata files `desktop.ini` and `Thumbs.db`. Unreadable folders have a blank count. Search results and library views are unsupported.

Version 1.1.3 moves file counting to two background workers, cancels obsolete work after navigation, and publishes count properties in small batches. File count remains the primary sort throughout a refresh. Counts can take a few seconds to arrive; large or network folders may take longer. Pending, inaccessible, and timed-out counts stay blank. Minimized windows pause updates. Directory changes are checked periodically (up to 30 seconds), and cached file totals become due for a recount after 60 seconds while the view is in use.

The helper runs in the notification area and reads Explorer's shell view through COM. It reads badge positions on a separate MTA worker with short UI Automation timeouts, refreshes them after layout events, and only caches visible item geometry. It does not load a DLL into Explorer. Folder names on disk stay unchanged. The MSI and executable are unsigned.

## Build

Install the Visual Studio x64 C++ Build Tools, Windows SDK, and 64-bit Python 3.10. From the repository directory run these commands.

```bat
Source\build.cmd
py -3.10 -m venv .venv
.venv\Scripts\python.exe build_installer.py
```

The MSI is written to `dist`.

## Limits and performance

The native column uses Windows' view-property cache. Microsoft marks [SetViewProperty as deprecated](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifolderview2-setviewproperty); this is not an officially extensible built-in Name/Date column. Compatibility depends on Explorer. A slow shell provider can still delay an individual COM call: the batch time budget is cooperative, not a hard timeout. The helper backs off after a slow update. Network I/O can outlast its cooperative deadline inside a Windows call, but it stays on a bounded background worker and helper shutdown does not wait for it.

UI Automation follows Microsoft's [separate-thread guidance](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading). These changes reduce repeated work and avoid synchronous UI Automation on the window thread; they cannot guarantee that Explorer never hangs or that every antivirus accepts an unsigned executable. No antivirus exclusions or security changes are required by FileCount.

The 1.1.3 release EXE/MSI passed the recorded Kaspersky scans. A separate, non-distributed development diagnostic was flagged and removed; that alert has not been verified as a false positive. See the [validation record](Tests/VALIDATION.md) for the distinction and remaining test limitations.

## Tests

Run `Tests\run.cmd` with the same Visual Studio build tools. It verifies direct counts, hidden/protected files, metadata exclusions, unknown/error results, cancellation, and the bounded worker queue using a deliberately slow counter. Temporary test data is isolated from user folders.
