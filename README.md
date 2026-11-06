# TaskManagerPlus

TaskManagerPlus is a fork of [System Informer](LICENSE.txt) that runs fully
locally: it makes no network connections to any other machine and carries no
update checks, online lookups, telemetry or links to upstream services.

Based on System Informer by Winsider Seminars & Solutions, Inc. and its
contributors. The original copyright and licence notices are kept in
`LICENSE.txt` and `COPYRIGHT.txt`.

## System requirements

Windows 10 or higher, 32-bit or 64-bit.

## Features

* A detailed overview of system activity with highlighting.
* Graphs and statistics to track down resource hogs and runaway processes.
* Find which processes are using a file.
* See which programs have active network connections (local views only, no
  address resolution) and close them if necessary.
* Real-time information on disk access.
* Detailed stack traces with kernel-mode, WOW64 and .NET support.
* Create, edit and control services.
* Small, portable and no installation required.

## What was removed

The Updater, OnlineChecks (VirusTotal and similar), NetworkTools (whois, ping,
traceroute, GeoIP download) and AgentTools plugins, "Search online", reverse
DNS of connections, signature revocation checks over the network, the default
Microsoft symbol server, Source Link URLs and all upstream hyperlinks. See
`docs/offline-audit.md` in the parent workspace for the audit.

Symbols resolve from local paths only (default `C:\Symbols`). A symbol server
can still be added by hand in the DbgHelp search path option, but
nothing is configured by default.

## Building the project

Requires Visual Studio (2022 or later). Run `build\build_init.cmd` once, then
build `SystemInformer.sln` followed by `plugins\Plugins.sln` (x64, Release) with
MSBuild or Visual Studio. See `build/README.md` for more information.

## Settings

To keep settings next to the executable (for example on a USB drive), create a
blank file named `SystemInformer.exe.settings.xml` in the same directory as
`SystemInformer.exe`.
