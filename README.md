# TaskManagerPlus

TaskManagerPlus is a free, open-source Windows process viewer and task manager
replacement with a calmer interface, per-program rules, a startup manager and
a rolling process history. It runs fully offline: it makes no outbound network
connections of any kind, with no update checks, telemetry, online lookups or
account features. The repository ships a guard script that enforces this
(see "Offline guarantee").

TaskManagerPlus by sethdtwigg. A fork of System Informer, MIT licensed, copyright Winsider Seminars & Solutions, Inc.; modifications copyright (c) 2026 sethdtwigg. Source: github.com/sethdtwigg/TaskManagerPlus (plain text; the app never contacts it). See "License" below.

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
x64/x86 Spectre-mitigated libraries), the .NET 10 SDK and Git. From a
PowerShell prompt in the repository root:

```
cmd /c build\build_init.cmd INIT
$msb = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $msb SystemInformer.sln -p:Configuration=Release -p:Platform=x64 -m -t:Build -nologo -v:m
& $msb plugins\Plugins.sln -p:Configuration=Release -p:Platform=x64 -m -nologo -v:m
```

Do not use `build\build_release.cmd`: its `/graph` build crashes MSBuild on
current Visual Studio. Output is `bin\Release64\SystemInformer.exe` and
`bin\Release64\plugins\*.dll`. If `build_init` fails with "PrivateSdkAssemblies
ItemGroup is required", run
`dotnet restore tools\CustomBuildTool\CustomBuildTool.csproj -r win-x64 -p:Platform=x64 -p:PublishAot=true`
first.

## Distribution

Releases are a portable zip: `SystemInformer.exe`, the `plugins` folder and
`Resources`. Extract anywhere and run; there is no installer. Binaries are
self-signed, so Windows SmartScreen may warn on first run.

## License

MIT. TaskManagerPlus is by sethdtwigg (modifications copyright (c) 2026 sethdtwigg) and is based on
System Informer, copyright Winsider Seminars & Solutions, Inc., and modified. The original notices are in `LICENSE.txt`
and `COPYRIGHT.txt`; credits are in `NOTICE.txt`.
