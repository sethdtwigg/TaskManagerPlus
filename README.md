# TaskManagerPlus

TaskManagerPlus is a free, open-source Windows process viewer and task manager
replacement with a calmer interface, per-program rules, a startup manager and
a rolling process history. It runs fully offline: it makes no outbound network
connections of any kind, with no update checks, telemetry, online lookups or
account features. The repository ships a guard script that enforces this
(see "Offline guarantee").

TaskManagerPlus by Seth Twigg. A fork of System Informer, MIT licensed, copyright Winsider Seminars & Solutions, Inc.; modifications copyright (c) 2026 Seth Twigg. Source: github.com/sethdtwigg/TaskManagerPlus (plain text; the app never contacts it). See "License" below.

## System requirements

Windows 10 or later, 64-bit. Runs without installation and without the kernel
driver (some driver-only features, such as kernel handle and memory access and
some protected-process details, are therefore unavailable). Administrator
rights are needed for some information (for example disk and firewall
monitoring) but not to start.

## Features

* Process, service, network, disk and performance views with a simplified,
  calmer default layout, light, dark and OLED themes.
* **Rules** tab: per-program rules (priority, CPU affinity including P/E-core
  presets, efficiency mode, end on start, run a command) and watchdogs
  (CPU, memory, I/O, handles or threads above a threshold for N seconds, then
  notify, lower priority, suspend, end, write a dump), with a runaway guard.
* **Startup** tab: Run/RunOnce keys, Startup folders, logon and boot scheduled
  tasks and Store startup entries in one list, with enable, disable and delete.
* **History** tab: rolling per-process snapshots with a time slider, kept on
  disk, to answer "what was using the CPU or memory a while ago".
* Replace Windows Task Manager (Options, "Make default...") with the
  `taskmgr.exe` Image File Execution Options debugger value; restorable.
* Detailed process properties, stack traces, handle and module views, service
  control, and file-in-use search, all local.

## Offline guarantee

Updater, online file checks, whois/ping/traceroute tools, GeoIP download,
"search online", reverse-DNS of connections, certificate revocation lookups
and the default symbol server were removed. Symbols resolve from local paths
only (default `C:\Symbols`). `tools\check-offline.ps1` scans the tracked
source for network APIs and remote hosts, and with `-Binaries` it checks the
built exe and DLLs for network imports and remote strings; run it before any
release:

```
powershell -File tools\check-offline.ps1
powershell -File tools\check-offline.ps1 -Binaries
```

## Data and settings

Settings and plugin data live in `%APPDATA%\TaskManagerPlus\`. On first start,
if that folder does not exist but `%APPDATA%\SystemInformer\settings.json`
does, settings, plugin data (`tmp-*.json`, `tmp-*.bin`) and user notes are
copied over once; the old folder is never changed. The program uses its own
single-instance and object names, so it can run beside a stock System
Informer. To keep everything next to the executable (portable mode), create
`settings.json` beside `SystemInformer.exe`.

## Building

Requires Visual Studio 2026 (Desktop development with C++, plus the MSVC
x64/x86 Spectre-mitigated libraries), the .NET 10 SDK, `nuget.exe` on PATH and
Git.

**The build is offline and needs a local NuGet feed that is not in this
repository.** `nuget.config` clears every remote source and points at
`..\offline-packages`, a folder of 28 `.nupkg` files (about 860 MB: the Windows
SDK/WDK for C++, the CustomBuildTool dependencies and the .NET runtime /
NativeAOT packs) next to the repository folder, so the layout is
`<parent>\offline-packages` and `<parent>\systeminformer`. The packages are
Microsoft-licensed and are not redistributed here. Create the feed once:

* on a machine that already built (or restored online), run
  `dotnet restore tools\CustomBuildTool\CustomBuildTool.csproj`, then
  `powershell -File tools\make-offline-packages.ps1`, and copy the resulting
  `offline-packages` folder next to this repository, or
* on a machine that never built, restore online once: temporarily add
  nuget.org as a package source in `nuget.config`, run the three commands
  below including `build_init` (it fetches every package, including the
  x86 NativeAOT pack that a plain restore skips), then run
  `make-offline-packages.ps1` and revert `nuget.config`.

Then, from a PowerShell prompt in the repository root:

```
dotnet restore tools\CustomBuildTool\CustomBuildTool.csproj
nuget restore packages.config -ConfigFile nuget.config -PackagesDirectory packages
cmd /c build\build_init.cmd INIT > build_init.log 2>&1
$msb = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $msb SystemInformer.sln -p:Configuration=Release -p:Platform=x64 -m -t:Build -nologo -v:m
& $msb plugins\Plugins.sln -p:Configuration=Release -p:Platform=x64 -m -nologo -v:m
```

(Use the `Professional` or `Enterprise` folder instead of `Community` if that
is what you installed.) `build_init` takes about a minute; redirect its output
to a file as shown, because once in a pipe it appeared to hang. Do not use
`build\build_release.cmd`: its `/graph` build crashes MSBuild on current Visual
Studio. Output is `bin\Release64\SystemInformer.exe` and
`bin\Release64\plugins\*.dll`. If `build_init` fails with
"PrivateSdkAssemblies ItemGroup is required", run
`dotnet restore tools\CustomBuildTool\CustomBuildTool.csproj -r win-x64 -p:Platform=x64 -p:PublishAot=true`
first. If you see `'vswhere.exe' is not recognized`, add
`C:\Program Files (x86)\Microsoft Visual Studio\Installer` to PATH and make
sure the environment variable `NoDefaultCurrentDirectoryInExePath` is not set.
Verify with `powershell -File tools\check-offline.ps1 -Binaries`.

## Distribution

Releases are a portable zip: `SystemInformer.exe`, the `plugins` folder and
`Resources`. Extract anywhere and run; there is no installer. Binaries are
self-signed, so Windows SmartScreen may warn on first run.

## License

MIT. TaskManagerPlus is by Seth Twigg (modifications copyright (c) 2026 Seth Twigg) and is based on
System Informer, copyright Winsider Seminars & Solutions, Inc., and modified. The original notices are in `LICENSE.txt`
and `COPYRIGHT.txt`; credits are in `NOTICE.txt`.
