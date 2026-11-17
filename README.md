# FileCount

Show a sortable file count in Windows 11 File Explorer. In icon views, folders show a small round number at the top right while sorted by file count.

## Install

Run this command in Command Prompt or PowerShell on Windows x64.

```text
msiexec.exe /i "https://raw.githubusercontent.com/pritamsay/FileCount/main/dist/FileCount-1.1.1-x64.msi" /passive
```

Windows Installer installs the app for the current user, starts it, and creates a Startup shortcut. If you used the older portable Explorer Counts helper, exit it from the notification area and remove its old Startup shortcut first.

## Uninstall

Choose **Settings → Apps → Installed apps → FileCount → Uninstall**, or run this command for version 1.1.1.

```text
msiexec.exe /x "{B3155947-DD08-4664-A8ED-822CCC5055E6}" /passive
```

Uninstall closes the installed helper and removes the File count column and sorting from open Explorer views and recognized saved folder layouts for the current user. When File count was the only sort, Explorer switches to Name. Windows supplies the built-in **File count** entry in **More…** and continues to list it there as an available property.

## Use

1. In Explorer's Details view, right-click a column heading, choose **More…**, check **File count**, and click **OK**.
2. Click that heading to sort numerically. Click again to reverse the order. You can also choose **File count** from **Sort by**.
3. Switch to an icon view to see folder badges. Choose a different sort to hide them.

The count covers files directly inside each folder. It respects Explorer's hidden-file settings and ignores the Windows metadata files `desktop.ini` and `Thumbs.db`. Unreadable folders have a blank count. Search results and library views are unsupported.

The helper runs in the notification area and reads Explorer's shell view through COM. It uses UI Automation to position its click-through badge window. Folder names on disk stay unchanged. The MSI and executable are unsigned.

## Build

Install the Visual Studio x64 C++ Build Tools, Windows SDK, and 64-bit Python 3.10. From the repository directory run these commands.

```bat
Source\build.cmd
py -3.10 -m venv .venv
.venv\Scripts\python.exe build_installer.py
```

The MSI is written to `dist`.
