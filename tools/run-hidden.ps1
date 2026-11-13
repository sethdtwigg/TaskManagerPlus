<#
.SYNOPSIS
  Runs SystemInformer.exe on a private (invisible) Windows desktop for UI tests and screenshots.

.DESCRIPTION
  Nothing appears on the user's screen and nothing takes focus: the app is started with lpDesktop set to a desktop
  created here, windows are enumerated on that desktop and captured with PrintWindow (PW_RENDERFULLCONTENT).
  The process lives in a Job object with KILL_ON_JOB_CLOSE, so a crash or Ctrl+C of this script cannot leave an
  orphaned SystemInformer.exe behind. Desktop, job and process are released in a finally block.

  ALWAYS runs with -nosettings, or with -Settings <absolute path of an EXISTING scratch json>. Never the user's file.

  Screenshots go to one folder per run under $env:TEMP\tmplus-hidden\<run id>; older run folders are deleted.
  Each started PID is recorded in $env:TEMP\tmplus-hidden\pids.txt; -Cleanup kills those (and only those, checked by
  name and start time), never the user's own instance.

.EXAMPLE
  tools\run-hidden.ps1 -Name dark -Commands 10226          # -nosettings, expand all, capture main window
  tools\run-hidden.ps1 -Name old -Settings C:\scratch\old.json -Dialogs yes,no
  tools\run-hidden.ps1 -Cleanup
#>
param(
    [string]$Name = "main",            # base name of the png written to the run folder
    [string]$Settings = "",            # absolute path of an EXISTING scratch settings json; "" = -nosettings
    [int]$Wait = 9,                    # seconds to wait after launch
    [int[]]$Commands = @(),            # WM_COMMAND ids posted to the main window before the capture
    [int]$Wait2 = 3,                   # seconds after the commands
    [string[]]$Dialogs = @(),          # answers (yes/no) for modal prompts at startup; yes = Return, no = Escape
    [int]$OptionsClick = 0,            # open Options and click the button with this control id
    [string[]]$Dialogs2 = @(),         # answers for prompts raised by that click
    [switch]$CaptureDialog,            # also capture the first dialog (e.g. About: -Commands 10106) to <Name>-dialog.png
    [string[]]$Steps = @(),          # scripted UI steps run after -Commands, see the Steps section below
    [string]$StepClass = "TaskManagerPlusQuickActionsPalette", # window class used by the type, key and shot steps
    [switch]$ExitCmd,                  # end with File > Exit so the app saves its settings, then wait
    [int]$Width = 1000, [int]$Height = 1150,
    [switch]$Cleanup
)

$ErrorActionPreference = "Stop"
$root = Join-Path $env:TEMP "tmplus-hidden"
$pidFile = Join-Path $root "pids.txt"
New-Item -ItemType Directory -Force -Path $root | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class H {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public delegate bool EnumDeskProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)]
  public struct JOBOBJECT_BASIC_LIMIT_INFORMATION { public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public int LimitFlags;
    public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public int ActiveProcessLimit; public UIntPtr Affinity;
    public int PriorityClass, SchedulingClass; }
  [StructLayout(LayoutKind.Sequential)]
  public struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
  [StructLayout(LayoutKind.Sequential)]
  public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION { public JOBOBJECT_BASIC_LIMIT_INFORMATION Basic; public IO_COUNTERS Io;
    public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed; }

  public static bool SetKillOnClose(IntPtr job) {
    var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
    info.Basic.LimitFlags = 0x2000; // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (set in C#: a nested struct assigned from PowerShell is a copy)
    return SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)));
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr dm, int flags, uint access, IntPtr sa);
  [DllImport("user32.dll", SetLastError = true)] public static extern bool CloseDesktop(IntPtr d);
  [DllImport("user32.dll")] public static extern bool EnumDesktopWindows(IntPtr d, EnumDeskProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string s);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] public static extern uint ResumeThread(IntPtr t);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr CreateJobObjectW(IntPtr sa, string name);
  [DllImport("kernel32.dll")] public static extern bool SetInformationJobObject(IntPtr job, int cls, ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION info, int len);
  [DllImport("kernel32.dll")] public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr proc);
  [DllImport("kernel32.dll")] public static extern bool TerminateProcess(IntPtr p, uint code);
}
"@

function Stop-Started {
    if (-not (Test-Path $pidFile)) { return }
    foreach ($line in Get-Content $pidFile) {
        $f = $line -split ","
        if ($f.Count -lt 2) { continue }
        $pr = Get-Process -Id ([int]$f[0]) -ErrorAction SilentlyContinue
        # same name and same start time = the process this tool started, not a reused PID and not the user's copy
        if ($pr -and $pr.ProcessName -eq "SystemInformer" -and $pr.StartTime.ToFileTimeUtc().ToString() -eq $f[1]) {
            Stop-Process -Id $pr.Id -Force
            "killed leftover pid $($pr.Id)"
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

if ($Cleanup) { Stop-Started; "cleanup done"; return }

if ($Settings -ne "") {
    if (-not [System.IO.Path]::IsPathRooted($Settings)) { throw "-Settings must be an absolute path: $Settings" }
    if (-not (Test-Path -LiteralPath $Settings)) { throw "scratch settings file does not exist: $Settings" }
    $appArgs = "-settings `"$Settings`""
} else { $appArgs = "-nosettings" }

$exe = Join-Path $PSScriptRoot "..\bin\Release64\SystemInformer.exe"
$exe = (Resolve-Path $exe).Path

# one folder per run; delete the earlier ones
Get-ChildItem $root -Directory -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
$runDir = Join-Path $root (Get-Date -Format "yyyyMMdd-HHmmss")
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

if ($Dialogs.Count -eq 1) { $Dialogs = $Dialogs[0] -split "," }
if ($Dialogs2.Count -eq 1) { $Dialogs2 = $Dialogs2[0] -split "," }
if ($Steps.Count -eq 1) { $Steps = $Steps[0] -split ";" }   # a -File invocation delivers one string

$deskName = "tmplus-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
$desk = [IntPtr]::Zero; $job = [IntPtr]::Zero; $pi = New-Object H+PROCESS_INFORMATION
$started = $false

function Get-DeskWindows {
    $list = New-Object System.Collections.ArrayList
    $cb = [H+EnumDeskProc]{ param($h, $l)
        if ([H]::IsWindowVisible($h)) {
            $t = New-Object System.Text.StringBuilder 256; [void][H]::GetWindowText($h, $t, 256)
            $c = New-Object System.Text.StringBuilder 256; [void][H]::GetClassName($h, $c, 256)
            [void]$list.Add([pscustomobject]@{ H = $h; Title = $t.ToString(); Class = $c.ToString() })
        }
        return $true }
    [void][H]::EnumDesktopWindows($desk, $cb, [IntPtr]::Zero)
    return $list
}
function Get-Main { (Get-DeskWindows | Where-Object { $_.Title -like "*TaskManagerPlus*" -and $_.Class -notlike "#32770" } | Select-Object -First 1).H }
function Find-ChildByClass($parent, $class) {
    $script:fc = [IntPtr]::Zero; $script:fcn = $class
    $cb = [H+EnumProc]{ param($c, $m) $t = New-Object System.Text.StringBuilder 256; [void][H]::GetClassName($c, $t, 256); if ($t.ToString() -eq $script:fcn -and $script:fc -eq [IntPtr]::Zero) { $script:fc = $c }; return $true }
    [void][H]::EnumChildWindows($parent, $cb, [IntPtr]::Zero)
    return $script:fc
}
function Get-Dialog { (Get-DeskWindows | Where-Object { $_.Class -eq "#32770" } | Select-Object -First 1).H }
function Save-Window($h, $path) {
    $r = New-Object H+RECT; [void][H]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][H]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "saved $path ${w}x${ht}"
}
function Answer($h, $a) {
    if ($a -eq "yes") { [void][H]::PostMessage($h, 0x100, [IntPtr]13, [IntPtr]::Zero) }   # Return
    else { [void][H]::PostMessage($h, 0x100, [IntPtr]27, [IntPtr]::Zero) }                  # Escape
}
function Find-ButtonParent($id) {
    $script:bp = [IntPtr]::Zero; $script:bid = $id
    foreach ($w in Get-DeskWindows) {
        if ([H]::GetDlgItem($w.H, $id) -ne [IntPtr]::Zero) { $script:bp = $w.H }
        $ch = [H+EnumProc]{ param($c, $m) if ([H]::GetDlgItem($c, $script:bid) -ne [IntPtr]::Zero) { $script:bp = $c }; return $true }
        [void][H]::EnumChildWindows($w.H, $ch, [IntPtr]::Zero)
    }
    return $script:bp
}

try {
    # GENERIC_ALL desktop access
    $desk = [H]::CreateDesktopW($deskName, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
    if ($desk -eq [IntPtr]::Zero) { throw "CreateDesktop failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }

    $job = [H]::CreateJobObjectW([IntPtr]::Zero, $null)
    if (-not [H]::SetKillOnClose($job)) { throw "SetInformationJobObject failed" }

    $si = New-Object H+STARTUPINFO
    $si.cb = [Runtime.InteropServices.Marshal]::SizeOf($si)
    $si.lpDesktop = $deskName
    $cmd = New-Object System.Text.StringBuilder ("`"$exe`" $appArgs")
    # CREATE_SUSPENDED (4) | CREATE_NEW_PROCESS_GROUP (0x200): join the job before the first instruction runs
    if (-not [H]::CreateProcessW($exe, $cmd, [IntPtr]::Zero, [IntPtr]::Zero, $false, 0x204, [IntPtr]::Zero, (Split-Path $exe), [ref]$si, [ref]$pi)) {
        throw "CreateProcess failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    $started = $true
    if (-not [H]::AssignProcessToJobObject($job, $pi.hProcess)) { [void][H]::TerminateProcess($pi.hProcess, 1); throw "AssignProcessToJobObject failed" }
    $proc = Get-Process -Id $pi.dwProcessId
    Add-Content $pidFile "$($pi.dwProcessId),$($proc.StartTime.ToFileTimeUtc())"
    [void][H]::ResumeThread($pi.hThread)
    "started pid $($pi.dwProcessId) on desktop $deskName"

    Start-Sleep -Seconds $Wait

    $i = 0
    foreach ($a in $Dialogs) {
        $d = Get-Dialog
        if (-not $d) { "dialog $i not found"; break }
        [void](Save-Window $d (Join-Path $runDir "$Name.dlg$i.png"))
        Answer $d $a; "dialog $i answered $a"; $i++
        Start-Sleep -Seconds 4
    }

    $main = Get-Main
    if (-not $main) { throw "no main window on the hidden desktop" }
    [void][H]::MoveWindow($main, 20, 20, $Width, $Height, $true)
    Start-Sleep -Milliseconds 800

    if ($OptionsClick -gt 0) {
        [void][H]::PostMessage($main, 0x111, [IntPtr]10083, [IntPtr]::Zero)   # ID_HACKER_OPTIONS
        Start-Sleep -Seconds 3
        $bp = Find-ButtonParent $OptionsClick
        if (-not $bp -or $bp -eq [IntPtr]::Zero) { "button $OptionsClick not found" }
        else {
            [void](Save-Window $bp (Join-Path $runDir "$Name.options.png"))
            [void][H]::PostMessage([H]::GetDlgItem($bp, $OptionsClick), 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
            Start-Sleep -Seconds 2
            $j = 0
            foreach ($a in $Dialogs2) {
                $d = Get-Dialog
                if (-not $d) { "dialog2 $j not found"; break }
                [void](Save-Window $d (Join-Path $runDir "$Name.odlg$j.png"))
                Answer $d $a; "dialog2 $j answered $a"; $j++
                Start-Sleep -Seconds 3
            }
        }
    }

    foreach ($c in $Commands) { [void][H]::PostMessage($main, 0x111, [IntPtr]$c, [IntPtr]::Zero); Start-Sleep -Milliseconds 700 }
    if ($Commands.Count) { Start-Sleep -Seconds $Wait2 }

    # Scripted UI steps (-Steps), each "op" or "op:arg". Everything is posted to windows on the hidden desktop.
    #   wait:<seconds>                 sleep
    #   msg:<registered message name>[,wParam[,lParam]]  post RegisterWindowMessage(name) to the main window (decimal params, default 0) (e.g. the QuickActions open message,
    #                                  used instead of Ctrl+K because posted key messages do not carry Ctrl state)
    #   type:<text>                    WM_CHAR for every character to the Edit of the window class given by -StepClass
    #   key:<vk>                       WM_KEYDOWN/WM_KEYUP (decimal virtual key) to that Edit (13 = Return, 27 = Escape, 40 = Down)
    #   shot:<name>                    capture the window of class -StepClass (or the main window if it is not open)
    #   shotmain:<name>                capture the main window
    #   dialog:yes|no                  answer the first #32770 dialog (yes = Return, no = Escape)
    foreach ($s in $Steps) {
        $op, $arg = $s -split ":", 2
        switch ($op) {
            "wait" { Start-Sleep -Seconds ([double]$arg) }
            "msg" { $mn, $wp, $lp = $arg -split ",", 3; $m = [H]::RegisterWindowMessageW($mn); [void][H]::PostMessage($main, $m, [IntPtr][int]$wp, [IntPtr][int]$lp); "posted $mn ($m)" }
            "type" {
                $w = (Get-DeskWindows | Where-Object { $_.Class -eq $StepClass } | Select-Object -First 1).H
                if (-not $w) { "step type: no $StepClass window"; break }
                $e = Find-ChildByClass $w "Edit"
                foreach ($ch in $arg.ToCharArray()) { [void][H]::PostMessage($e, 0x102, [IntPtr][int]$ch, [IntPtr]::Zero); Start-Sleep -Milliseconds 40 }
            }
            "key" {
                $w = (Get-DeskWindows | Where-Object { $_.Class -eq $StepClass } | Select-Object -First 1).H
                if (-not $w) { "step key: no $StepClass window"; break }
                $e = Find-ChildByClass $w "Edit"
                [void][H]::PostMessage($e, 0x100, [IntPtr][int]$arg, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
                [void][H]::PostMessage($e, 0x101, [IntPtr][int]$arg, [IntPtr]::Zero)
            }
            "shot" {
                $w = (Get-DeskWindows | Where-Object { $_.Class -eq $StepClass } | Select-Object -First 1).H
                if (-not $w) { "step shot: no $StepClass window, capturing the main window"; $w = $main }
                Start-Sleep -Milliseconds 500
                Save-Window $w (Join-Path $runDir "$arg.png")
            }
            "shotmain" { Start-Sleep -Milliseconds 500; Save-Window $main (Join-Path $runDir "$arg.png") }
            "dialog" { $d = Get-Dialog; if ($d) { [void](Save-Window $d (Join-Path $runDir "step-dialog.png")); Answer $d $arg; "dialog answered $arg" } else { "step dialog: none" } }
            default { "unknown step $s" }
        }
        Start-Sleep -Milliseconds 300
    }

    Save-Window $main (Join-Path $runDir "$Name.png")
    if ($CaptureDialog) { Start-Sleep -Seconds 8; $dlg = Get-Dialog; if ($dlg) { Save-Window $dlg (Join-Path $runDir "$Name-dialog.png") } else { "no dialog to capture" } }

    if ($ExitCmd) {
        [void][H]::PostMessage($main, 0x111, [IntPtr]10001, [IntPtr]::Zero)   # ID_HACKER_EXIT
        [void]$proc.WaitForExit(15000)
        "exited=$($proc.HasExited)"
    }
    "run folder: $runDir"
}
finally {
    # the job (KILL_ON_JOB_CLOSE) takes the app down even if the kill below is skipped
    if ($started) { try { [void][H]::TerminateProcess($pi.hProcess, 0) } catch {} }
    if ($pi.hThread -ne [IntPtr]::Zero) { [void][H]::CloseHandle($pi.hThread) }
    if ($pi.hProcess -ne [IntPtr]::Zero) { [void][H]::CloseHandle($pi.hProcess) }
    if ($job -ne [IntPtr]::Zero) { [void][H]::CloseHandle($job) }
    if ($desk -ne [IntPtr]::Zero) { [void][H]::CloseDesktop($desk) }
    if ($started) { Remove-Item $pidFile -Force -ErrorAction SilentlyContinue }
}
