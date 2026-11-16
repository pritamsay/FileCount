"""Build the per-user Windows Installer package with 64-bit Python 3.10."""

from pathlib import Path
import msilib
from msilib import schema, sequence


root = Path(__file__).resolve().parent
release = root / "dist"
release.mkdir(exist_ok=True)
package = release / "FileCount-1.1.0-x64.msi"
if not msilib.AMD64:
    raise RuntimeError("Use 64-bit Python.")
if not (root / "ExplorerCounts.exe").is_file():
    raise RuntimeError("Run Source/build.cmd first.")

db = msilib.init_database(str(package), schema, "FileCount",
                          "{00F17C7A-452D-4CDD-B47D-3416E2046183}", "1.1.0", "pritamsay")
summary = db.GetSummaryInformation(1)
# Compressed media (2) and a package that needs only current-user access (8).
summary.SetProperty(msilib.PID_WORDCOUNT, 10)
summary.Persist()
cab = msilib.CAB("FileCount.cab")
feature = msilib.Feature(db, "FileCount", "FileCount", "Folder file counts in Explorer", 1)
def add(table, rows):
    msilib.add_data(db, table, rows)


add("Property", [("UpgradeCode", "{AE89250B-8094-46BD-B4BB-148D78C80DF1}"),
                 ("INSTALLLEVEL", "1"), ("ARPNOMODIFY", "1")])
add("Directory", [("TARGETDIR", None, "SourceDir"),
                  ("LocalAppDataFolder", "TARGETDIR", "."),
                  ("Programs", "LocalAppDataFolder", "Programs"),
                  ("APPDIR", "Programs", "FileCount"),
                  ("ProgramMenuFolder", "TARGETDIR", "."),
                  ("StartupFolder", "TARGETDIR", ".")])

# HKCU is the key path so Windows Installer tracks this installation per user.
add("Component", [("Application", "{E3BF51B9-D217-4E46-B1C3-F781D8C65111}",
                   "APPDIR", 260, None, "InstallLocation")])
add("Registry", [("InstallLocation", 1, r"Software\FileCount", "InstallPath",
                  "[APPDIR]", "Application")])
add("FeatureComponents", [(feature.id, "Application")])
exe = root / "ExplorerCounts.exe"
file_sequence, file_id = cab.append(str(exe), exe.name, "ExplorerCounts")
add("File", [(file_id, "Application", "COUNT.EXE|ExplorerCounts.exe", exe.stat().st_size,
              "1.1.0.0", "1033", 512, file_sequence)])

for folder in ["ProgramMenuFolder", "StartupFolder"]:
    add("Shortcut", [(folder + "Link", folder, "FileCount", "Application",
                      "[#ExplorerCounts]", None, None, None, None, None, 1,
                      "APPDIR")])
add("RemoveFile", [("RemoveApplicationFolder", "Application", None, "APPDIR", 2)])
add("CustomAction", [("StartFileCount", 210, file_id, None)])
add("InstallExecuteSequence", sequence.InstallExecuteSequence +
    [("StartFileCount", "NOT Installed AND NOT REMOVE", 6800)])
add("AdminExecuteSequence", sequence.AdminExecuteSequence)
# A basic progress UI is sufficient. There are no configuration dialogs.
add("InstallUISequence", [("ExecuteAction", None, 1300)])
add("AdminUISequence", [("ExecuteAction", None, 1300)])
cab.commit(db)
db.Commit()
print(package)
