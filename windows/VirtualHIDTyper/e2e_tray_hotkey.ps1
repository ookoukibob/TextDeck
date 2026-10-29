# ---------------------------------------------------------------------------
# e2e_tray_hotkey.ps1
#
# REAL acceptance for the shipped, user-facing behaviour:
#   * the app lives in the system tray (window hidden)
#   * a global hotkey starts typing into whatever window is in front
#   * the hotkey does NOT steal the foreground window
#   * tray menu / hotkey / Stop button all stop the run and release keys
#
# Runs the OFFICIAL EXE from the delivery folder (not a dev binary).
# Requires elevation (the device object is ACLed to SYSTEM+Administrators).
# Runs the app in SOFTWARE mode with an isolated profile: hardware and HID modes
# need a board or a driver, and the machine's real profile may select either.
# ---------------------------------------------------------------------------
param(
    [string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) "dist\VirtualHIDTyper\VirtualHIDTyper.exe"),
    [string]$LogPath = (Join-Path (Split-Path $PSScriptRoot -Parent) "logs\e2e-tray.log")
)

$ErrorActionPreference = "Stop"
$script:Failures = 0
$script:LogLines = New-Object System.Collections.ArrayList
function Log([string]$m) { [void]$script:LogLines.Add($m); Write-Output $m }
function Assert-True([bool]$c, [string]$n, [string]$d = "") {
    if ($c) { Log "  [PASS] $n :: $d" } else { Log "  [FAIL] $n :: $d"; $script:Failures++ }
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class TN {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", EntryPoint="GetWindowThreadProcessId")] public static extern uint GetWindowThreadId(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    public static IntPtr ByTitlePid(string title, uint pid) {
        IntPtr[] r = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            StringBuilder sb = new StringBuilder(512); GetWindowText(h, sb, 512);
            if (string.Equals(sb.ToString(), title, StringComparison.Ordinal)) { r[0] = h; return false; }
            return true;
        }, IntPtr.Zero);
        return r[0];
    }
    public static IntPtr ByClassPid(uint pid, string cls) {
        // The app derives its window class from the data directory:
        // "VirtualHIDTyperWnd" for the default profile and
        // "VirtualHIDTyperWnd_<hex>" when --data-dir is used. That per-profile
        // class is deliberate (two profiles must not collide), so a lookup by
        // exact name would fail as soon as the run uses an isolated profile.
        IntPtr[] r = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            StringBuilder sb = new StringBuilder(256); GetClassName(h, sb, 256);
            string got = sb.ToString();
            if (got.Equals(cls, StringComparison.Ordinal) ||
                got.StartsWith(cls + "_", StringComparison.Ordinal)) { r[0] = h; return false; }
            return true;
        }, IntPtr.Zero);
        return r[0];
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    public static void AltTap() { keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero); }

    // Bind the TARGET window to en-US and detach its IME. The virtual HID
    // keyboard emits a US scan-code stream; if the target runs under a Chinese
    // IME the characters are transformed and the comparison would measure the
    // IME instead of the HID path. Only these test windows are affected.
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("imm32.dll")] public static extern IntPtr ImmAssociateContext(IntPtr h, IntPtr himc);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr LoadKeyboardLayout(string id, uint flags);
    public static bool BindUsLayout(IntPtr hwnd) {
        IntPtr hkl = LoadKeyboardLayout("00000409", 0x00000001);
        PostMessage(hwnd, 0x0050, IntPtr.Zero, hkl);
        ImmAssociateContext(hwnd, IntPtr.Zero);
        return hkl != IntPtr.Zero;
    }
}
'@

$WM_GETTEXT = 0x000D
$WM_SETTEXT = 0x000C
$WM_COMMAND = 0x0111
$IDC_EDIT_TEXT = 1001
$IDC_BTN_START = 1004
$IDC_BTN_STOP = 1005
$IDC_STATIC_STATUS = 1007

function Get-WmText([IntPtr]$h, [int]$cap = 2048) {
    if ($h -eq [IntPtr]::Zero) { return "" }
    $sb = New-Object System.Text.StringBuilder $cap
    [void][TN]::SendMessage($h, $WM_GETTEXT, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Set-WmText([IntPtr]$h, [string]$t) { [void][TN]::SendMessage($h, $WM_SETTEXT, [IntPtr]::Zero, $t) }
function Send-Cmd([IntPtr]$h, [int]$id) { [void][TN]::SendMessage($h, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) }
function Force-Fg([IntPtr]$h) {
    if (-not [TN]::IsWindow($h)) { return $false }
    for ($i = 0; $i -lt 30; $i++) {
        if ([TN]::GetForegroundWindow() -eq $h) { return $true }
        [void][TN]::ShowWindow($h, 9)
        $fg = [TN]::GetForegroundWindow()
        if ($fg -ne [IntPtr]::Zero) {
            $t1 = [TN]::GetWindowThreadId($fg, [IntPtr]::Zero)
            $t2 = [TN]::GetCurrentThreadId()
            [void][TN]::AttachThreadInput($t2, $t1, $true)
            [void][TN]::SetForegroundWindow($h)
            [void][TN]::BringWindowToTop($h)
            [void][TN]::AttachThreadInput($t2, $t1, $false)
        } else { [void][TN]::SetForegroundWindow($h) }
        if ([TN]::GetForegroundWindow() -eq $h) { return $true }
        [TN]::AltTap()
        [void][TN]::SetForegroundWindow($h)
        if ([TN]::GetForegroundWindow() -eq $h) { return $true }
        Start-Sleep -Milliseconds 60
    }
    return ([TN]::GetForegroundWindow() -eq $h)
}
function Wait-Status([IntPtr]$t, [string]$pat, [int]$sec) {
    $dl = (Get-Date).AddSeconds($sec); $st = ""
    do {
        $st = Get-WmText ([TN]::GetDlgItem($t, $IDC_STATIC_STATUS)) 2048
        if ($st -match $pat) { return $st }
        Start-Sleep -Milliseconds 120
    } while ((Get-Date) -lt $dl)
    return $st
}
function Read-Snap([string]$p) { for ($i=0; $i -lt 12; $i++) { try { return [IO.File]::ReadAllText($p) } catch { Start-Sleep -Milliseconds 60 } }; return $null }
function Wait-Snap([string]$p, [int]$sec) {
    $dl = (Get-Date).AddSeconds($sec); $last = $null
    while ((Get-Date) -lt $dl) {
        $cur = Read-Snap $p
        if ($cur -cne $last) { $last = $cur; Start-Sleep -Milliseconds 350; continue }
        return $cur
    }
    return $last
}
function Norm([string]$s) { if ($null -eq $s) { return "" }; return ($s -replace "`r`n", "`n") }

# The default global hotkeys are Ctrl+Alt+Shift+F9 (start) and
# Ctrl+Alt+Shift+F10 (stop) - the extra Shift and the two distinct function
# keys make accidental collisions with other software far less likely.
function Press-StartHotkey {
    [TN]::keybd_event(0x11,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x10,0,0,[UIntPtr]::Zero)
    [TN]::keybd_event(0x78,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [TN]::keybd_event(0x78,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x10,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
}
function Press-StopHotkey {
    [TN]::keybd_event(0x11,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x10,0,0,[UIntPtr]::Zero)
    [TN]::keybd_event(0x79,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [TN]::keybd_event(0x79,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x10,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
}

$script:Targets = @()
function Start-Target([string]$title, [string]$snap) {
    $host1 = Join-Path $PSScriptRoot "e2e_target_host.ps1"
    $p = Start-Process powershell -ArgumentList @("-NoProfile","-STA","-ExecutionPolicy","Bypass","-File",("`"" + $host1 + "`""),"-Title",("`"" + $title + "`""),"-Snapshot",("`"" + $snap + "`""),"-Control","RichTextBox") -PassThru
    $script:Targets += $p
    $dl = (Get-Date).AddSeconds(12); $h = [IntPtr]::Zero
    while ((Get-Date) -lt $dl -and $h -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $h = [TN]::ByTitlePid($title, [uint32]$p.Id)
    }
    if ($h -eq [IntPtr]::Zero) { throw "target did not appear" }
    # measure the HID stream, not an IME transformation
    $null = [TN]::BindUsLayout($h)
    Start-Sleep -Milliseconds 400
    return @{ Pid = $p; Hwnd = $h; Snap = $snap }
}
function Stop-Target($t) {
    try {
        if ($t -and $t.Pid -and -not $t.Pid.HasExited) { [void]$t.Pid.CloseMainWindow() }
        if ($t -and $t.Pid -and -not $t.Pid.WaitForExit(3000)) { $t.Pid.Kill() }
    } catch { }
}

$logDir = Split-Path $LogPath -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$ev = Join-Path $logDir "e2e-tray-evidence"
if (-not (Test-Path $ev)) { New-Item -ItemType Directory -Force -Path $ev | Out-Null }

$typer = [IntPtr]::Zero
$typerProc = $null
# The harness needs DETERMINISTIC default bindings; back up the user's real
# settings.ini and restore it verbatim on exit so no user configuration is
# ever lost (requirement: never destroy user data).
$iniPath = Join-Path $env:APPDATA "VirtualHIDTyper\settings.ini"
$iniBackup = $null
if (Test-Path $iniPath) { $iniBackup = [IO.File]::ReadAllBytes($iniPath) }
try {
    if (-not (Test-Path (Split-Path $iniPath))) { New-Item -ItemType Directory -Force -Path (Split-Path $iniPath) | Out-Null }
    @'
[Main]
HotkeyStart=7,120
HotkeyStop=7,121
HotkeySchema=2
'@ | Set-Content -Path $iniPath -Encoding UTF8
} catch { }
try {
    Log "=== tray + global hotkey E2E  $(Get-Date -Format "yyyy-MM-dd HH:mm:ss") ==="
    Log "EXE: $Exe"
    Assert-True (Test-Path $Exe) "EXE-present" $Exe

    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Assert-True $isAdmin "elevated" "required: device ACL is SY+BA only"

    # Launch with an ISOLATED profile, forced into software mode.
    #
    # WHY: launched bare, the app reads the machine's real
    # %APPDATA%\VirtualHIDTyper\settings.ini. On this host that profile is
    # InputMode=esp32, so the run came up in ESP32 mode with no board attached
    # and typed nothing at all - every "did text arrive" assertion then failed
    # for a reason that has nothing to do with the tray or the hotkeys this
    # suite is meant to test. Software mode needs no driver and no hardware, so
    # it is the only mode that can be exercised here.
    #
    # The isolated directory also keeps the test from reading or rewriting the
    # user's real settings.
    $typerData = Join-Path (Split-Path $LogPath -Parent) 'e2e-tray-typerdata'
    if (-not (Test-Path $typerData)) { New-Item -ItemType Directory -Force -Path $typerData | Out-Null }
    $typerProc = Start-Process $Exe -ArgumentList ("--mode=software --data-dir=" + $typerData) -PassThru
    $dl = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $dl -and $typer -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $typer = [TN]::ByClassPid([uint32]$typerProc.Id, "VirtualHIDTyperWnd")
    }
    Assert-True ($typer -ne [IntPtr]::Zero) "main-window" "hwnd=0x$($typer.ToString("X")) pid=$($typerProc.Id)"
    Start-Sleep -Milliseconds 800

    $status = Get-WmText ([TN]::GetDlgItem($typer, $IDC_STATIC_STATUS)) 2048
    Log "  initial status: $status"

    # --- A: window hides to tray on close, process survives -----------------
    Log "--- A: close-to-tray (defaults: Ctrl+Alt+Shift+F9 / Ctrl+Alt+Shift+F10) ---"
    # WM_SYSCOMMAND + SC_CLOSE is what the title-bar X sends. (Sending
    # WM_COMMAND with this id would be meaningless and prove nothing.)
    [void][TN]::SendMessage($typer, 0x0112, [IntPtr]0xF060, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900
    Assert-True (-not $typerProc.HasExited) "A-process-alive" "still running after close"
    Assert-True (-not [TN]::IsWindowVisible($typer)) "A-window-hidden" "main window is not visible"

    # --- B: hotkey start with the window hidden, target keeps focus ----------
    Log "--- B: global hotkey start, no focus theft ---"
    $t1 = "Round12 abc XYZ 123 , . ; : " + [char]39 + " " + [char]34 + " ()[]{}`r`nLine2 END"
    $snap1 = Join-Path $ev "b1.txt"
    $w1 = $null
    try {
        $w1 = Start-Target "Target B" $snap1
        Assert-True (Force-Fg $w1.Hwnd) "B-target-focused" "hwnd=0x$($w1.Hwnd.ToString("X"))"
        # load the text into the (hidden) editor without showing the window
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t1
        Set-WmText ([TN]::GetDlgItem($typer, 1002)) "25"
        $fgBefore = [TN]::GetForegroundWindow()
        # press the GLOBAL start hotkey Ctrl+Alt+Shift+F9
        Press-StartHotkey
        $st = Wait-Status $typer "(完成|已停止|错误|失败)" 90
        Log "  status: $st"
        Assert-True ([TN]::GetForegroundWindow() -eq $fgBefore) "B-no-focus-theft" "foreground unchanged by the hotkey"
        Assert-True ($st -match "完成") "B-completed" $st
        $a1 = Wait-Snap $snap1 10
        Assert-True ((Norm $a1) -ceq (Norm $t1)) "B-text-identical" ("expected=" + (Norm $t1) + " | actual=" + (Norm $a1))
    } finally { if ($w1) { Stop-Target $w1 } }

    # --- C: emergency stop hotkey mid-run -------------------------------
    Log "--- C: Ctrl+Alt+Shift+F10 emergency stop ---"
    $long = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 6)
    $snap2 = Join-Path $ev "c1.txt"
    $w2 = $null
    try {
        $w2 = Start-Target "Target C" $snap2
        Assert-True (Force-Fg $w2.Hwnd) "C-target-focused" "hwnd=0x$($w2.Hwnd.ToString("X"))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $long
        Set-WmText ([TN]::GetDlgItem($typer, 1002)) "60"
        Press-StartHotkey
        Start-Sleep -Milliseconds 2000
        # emergency stop
        Press-StopHotkey
        $st2 = Wait-Status $typer "(已停止|完成|错误)" 20
        Log "  status: $st2"
        Assert-True ($st2 -match "已停止") "C-stopped" $st2
        $a2 = Wait-Snap $snap2 8
        Assert-True ($a2.Length -gt 0) "C-partial-output" "typed $($a2.Length) of $($long.Length)"
        Assert-True ($a2.Length -lt $long.Length) "C-partial-stop" "stopped before the end"
        Start-Sleep -Milliseconds 1200
        $a2b = Wait-Snap $snap2 6
        Assert-True ((Norm $a2) -ceq (Norm $a2b)) "C-no-input-after-stop" "stable: $a2b"
    } finally { if ($w2) { Stop-Target $w2 } }

    # --- D: repeat hotkey while running must not start a second task -------
    Log "--- D: repeat start hotkey is ignored ---"
    $snap3 = Join-Path $ev "d1.txt"
    $w3 = $null
    try {
        $w3 = Start-Target "Target D" $snap3
        Assert-True (Force-Fg $w3.Hwnd) "D-target-focused" "hwnd=0x$($w3.Hwnd.ToString("X"))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $long
        Set-WmText ([TN]::GetDlgItem($typer, 1002)) "60"
        Press-StartHotkey
        Start-Sleep -Milliseconds 1200
        # press start again several times during the run
        for ($i = 0; $i -lt 3; $i++) {
            Press-StartHotkey
            Start-Sleep -Milliseconds 200
        }
        $stD = Wait-Status $typer "(完成|已停止|错误)" 60
        Log "  status: $stD"
        $aD = Wait-Snap $snap3 10
        # A duplicated start would produce MORE characters than the input.
        Assert-True ($aD.Length -le $long.Length) "D-no-duplicate" "typed $($aD.Length) of $($long.Length) (never more)"
        Assert-True ($stD -match "完成") "D-completed-once" $stD
    } finally { if ($w3) { Stop-Target $w3 } }

    # --- E: Stop button works after all of the above ---------------------
    Log "--- E: Stop button mid-run ---"
    $snap4 = Join-Path $ev "e1.txt"
    $w4 = $null
    try {
        $w4 = Start-Target "Target E" $snap4
        Assert-True (Force-Fg $w4.Hwnd) "E-target-focused" "hwnd=0x$($w4.Hwnd.ToString("X"))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $long
        Set-WmText ([TN]::GetDlgItem($typer, 1002)) "60"
        Press-StartHotkey
        Start-Sleep -Milliseconds 1800
        Send-Cmd $typer $IDC_BTN_STOP
        $stE = Wait-Status $typer "(已停止|完成|错误)" 20
        Log "  status: $stE"
        Assert-True ($stE -match "已停止") "E-stopped" $stE
        $aE = Wait-Snap $snap4 8
        Start-Sleep -Milliseconds 1000
        $aEb = Wait-Snap $snap4 6
        Assert-True ((Norm $aE) -ceq (Norm $aEb)) "E-no-input-after-stop" "stable"
    } finally { if ($w4) { Stop-Target $w4 } }
}
catch {
    Log "FATAL: $($_.Exception.Message)"
    Log $_.ScriptStackTrace
    $script:Failures++
}
finally {
    foreach ($t in $script:Targets) { Stop-Target $t }
    try {
        if ($typerProc -and -not $typerProc.HasExited) {
            $typerProc.Kill()
            [void]$typerProc.WaitForExit(3000)
        }
    } catch { }
    # Put the user's original settings file back, byte for byte.
    try {
        if ($null -ne $iniBackup) { [IO.File]::WriteAllBytes($iniPath, $iniBackup) }
        elseif (Test-Path $iniPath) { Remove-Item $iniPath -Force }
    } catch { }
}

Log ""
if ($script:Failures -eq 0) { Log "RESULT: ALL TRAY/HOTKEY CHECKS PASSED" } else { Log "RESULT: $($script:Failures) FAILURE(S)" }
[IO.File]::WriteAllLines($LogPath, [string[]]$script:LogLines, (New-Object System.Text.UTF8Encoding($true)))
if ($script:Failures -eq 0) { exit 0 } else { exit 1 }