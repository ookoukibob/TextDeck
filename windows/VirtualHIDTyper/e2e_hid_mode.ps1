# ---------------------------------------------------------------------------
# e2e_hid_mode.ps1
#
# REAL end-to-end acceptance for VirtualHIDTyper's Virtual HID mode.
#
# Unlike e2e_software_mode.ps1 (SendInput, no driver), this proves the KERNEL
# path: the KMDF/VHF driver is loaded, Windows enumerated a HID keyboard, the
# app opens \\.\VirtualHIDDevice and submits IOCTL_KEYBOARD_INPUT_EX, and
# characters actually land in a dedicated target window.
#
# Preconditions (checked, never assumed):
#   - service VirtualHIDDriver exists and is RUNNING
#   - device ROOT\VIRTUALHIDDRIVER\0000 exists, Status OK, Problem CM_PROB_NONE
#   - a HID keyboard under HID\HID_DEVICE_SYSTEM_VHF is enumerated
#   - this script runs ELEVATED: the INF grants the device to SY+BA only, so an
#     unelevated app gets ERROR_ACCESS_DENIED by design - that is NOT a bug.
#
# Safety: starts its OWN VirtualHIDTyper.exe and its OWN target hosts, tracks
# those exact PIDs and closes only them. Never kills same-named processes it
# did not create. Never touches the clipboard. Never changes BCD/certs/HVCI.
#
# Exit code 0 = every executed assertion passed.
# ---------------------------------------------------------------------------
param(
    [string]$Exe = (Join-Path $PSScriptRoot 'bin\VirtualHIDTyper.exe'),
    [string]$LogPath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'logs\e2e-hid.log')
)

$ErrorActionPreference = 'Stop'

$CRLF = [string][char]13 + [string][char]10
$LF   = [string][char]10
$DQ   = [string][char]34

$script:Failures = 0
$script:LogLines = New-Object System.Collections.ArrayList
function Log([string]$msg) {
    [void]$script:LogLines.Add($msg)
    Write-Output $msg
}
function Assert-True([bool]$cond, [string]$name, [string]$detail = '') {
    if ($cond) { Log "  [PASS] $name :: $detail" }
    else { Log "  [FAIL] $name :: $detail"; $script:Failures++ }
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class HNative {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", EntryPoint = "GetWindowThreadProcessId")]
    public static extern uint GetWindowThreadId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")]
    public static extern bool AttachThreadInput(uint a, uint b, bool f);
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);

    public static IntPtr FindByTitlePid(string title, uint pid) {
        IntPtr[] box = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wpid = 0; GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            StringBuilder sb = new StringBuilder(512);
            GetWindowText(h, sb, 512);
            if (string.Equals(sb.ToString(), title, StringComparison.Ordinal)) { box[0] = h; return false; }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }
    public static IntPtr FindByPidClass(uint pid, string cls) {
        IntPtr[] box = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wpid = 0; GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            StringBuilder sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (string.Equals(sb.ToString(), cls, StringComparison.Ordinal)) { box[0] = h; return false; }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }
    public static void AltTap() { keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero); }

    // Keyboard layout + IME control for the TARGET window only.
    // A virtual HID keyboard emits raw HID usages (a US-layout scan-code
    // stream). If the target window runs under a Chinese IME the characters
    // are transformed by the IME and the comparison would measure the IME,
    // not the HID path. Binding the target to en-US (0x0409) with the IME
    // detached makes the observed text a faithful read-out of the HID report
    // stream. Nothing global is changed: only these windows are affected.
    [DllImport("user32.dll")]
    public static extern IntPtr PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("imm32.dll")]
    public static extern IntPtr ImmAssociateContext(IntPtr h, IntPtr himc);
    [DllImport("user32.dll")]
    public static extern IntPtr LoadKeyboardLayout(string id, uint flags);

    public static bool BindUsLayout(IntPtr hwnd) {
        IntPtr hkl = LoadKeyboardLayout("00000409", 0x00000001); // KLF_ACTIVATE
        PostMessage(hwnd, 0x0050, IntPtr.Zero, hkl);             // WM_INPUTLANGCHANGEREQUEST
        IntPtr prev = ImmAssociateContext(hwnd, IntPtr.Zero);     // detach IME
        return hkl != IntPtr.Zero;
    }
}
'@

$WM_GETTEXT = 0x000D
$WM_SETTEXT = 0x000C
$WM_COMMAND = 0x0111
$WM_CLOSE   = 0x0010
$BM_SETCHECK = 0x00F1
$BST_UNCHECKED = 0
$IDC_EDIT_TEXT     = 1001
$IDC_EDIT_INTERVAL = 1002
$IDC_CHK_SHIFT_NL  = 1003
$IDC_BTN_START     = 1004
$IDC_BTN_STOP      = 1005
$IDC_STATIC_STATUS = 1007
$IDC_RADIO_HID     = 1008
$BM_CLICK   = 0x00F5

function Get-WmText([IntPtr]$hwnd, [int]$cap = 2048) {
    if ($hwnd -eq [IntPtr]::Zero) { return '' }
    $sb = New-Object System.Text.StringBuilder $cap
    [void][HNative]::SendMessage($hwnd, $WM_GETTEXT, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Set-WmText([IntPtr]$hwnd, [string]$text) {
    [void][HNative]::SendMessage($hwnd, $WM_SETTEXT, [IntPtr]::Zero, $text)
}
function Send-Cmd([IntPtr]$hwnd, [int]$id) {
    [void][HNative]::SendMessage($hwnd, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
}
# The app persists the selected input mode in settings.ini, so a previous
# software-mode run leaves MODE_SOFTWARE as the startup default. This test MUST
# drive the real kernel path, so the HID radio is clicked explicitly and the
# resulting title is asserted - a software-mode run would prove nothing here.
function Set-ModeRadio([IntPtr]$typer, [int]$id) {
    $rb = [HNative]::GetDlgItem($typer, $id)
    if ($rb -eq [IntPtr]::Zero) { return $false }
    [void][HNative]::SendMessage($rb, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 250
    return $true
}
function Get-Title([IntPtr]$hwnd) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][HNative]::GetWindowText($hwnd, $sb, 512)
    return $sb.ToString()
}
function Force-Fg([IntPtr]$h) {
    if (-not [HNative]::IsWindow($h)) { return $false }
    for ($i = 0; $i -lt 25; $i++) {
        if ([HNative]::GetForegroundWindow() -eq $h) { return $true }
        [void][HNative]::ShowWindow($h, 9)
        $fg = [HNative]::GetForegroundWindow()
        if ($fg -ne [IntPtr]::Zero) {
            $fgTid = [HNative]::GetWindowThreadId($fg, [IntPtr]::Zero)
            $selfTid = [HNative]::GetCurrentThreadId()
            [void][HNative]::AttachThreadInput($selfTid, $fgTid, $true)
            [void][HNative]::SetForegroundWindow($h)
            [void][HNative]::BringWindowToTop($h)
            [void][HNative]::AttachThreadInput($selfTid, $fgTid, $false)
        } else { [void][HNative]::SetForegroundWindow($h) }
        if ([HNative]::GetForegroundWindow() -eq $h) { return $true }
        [HNative]::AltTap()
        [void][HNative]::SetForegroundWindow($h)
        if ([HNative]::GetForegroundWindow() -eq $h) { return $true }
        Start-Sleep -Milliseconds 60
    }
    return ([HNative]::GetForegroundWindow() -eq $h)
}
function Norm([string]$s) { if ($null -eq $s) { return '' }; return ($s -replace $CRLF, $LF) }
function Test-Equal([string]$a, [string]$b) { return ((Norm $a) -ceq (Norm $b)) }
function Read-Snap([string]$path) {
    for ($i = 0; $i -lt 12; $i++) {
        try { return [System.IO.File]::ReadAllText($path) } catch { Start-Sleep -Milliseconds 60 }
    }
    return $null
}
function Wait-SnapSettled([string]$path, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $last = $null
    while ((Get-Date) -lt $deadline) {
        $cur = Read-Snap $path
        if ($cur -cne $last) { $last = $cur; Start-Sleep -Milliseconds 350; continue }
        return $cur
    }
    return $last
}
function Wait-Status([IntPtr]$typer, [string]$pattern, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $st = ''
    do {
        $st = Get-WmText ([HNative]::GetDlgItem($typer, $IDC_STATIC_STATUS)) 2048
        if ($st -match $pattern) { return $st }
        Start-Sleep -Milliseconds 120
    } while ((Get-Date) -lt $deadline)
    return $st
}

$script:Targets = @()
function Start-Target([string]$title, [string]$snap) {
    $host1 = Join-Path $PSScriptRoot 'e2e_target_host.ps1'
    $p = Start-Process powershell -ArgumentList @(
        '-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-File', ($DQ + $host1 + $DQ),
        '-Title', ($DQ + $title + $DQ), '-Snapshot', ($DQ + $snap + $DQ), '-Control', 'RichTextBox'
    ) -PassThru
    $script:Targets += $p
    $deadline = (Get-Date).AddSeconds(12)
    $hwnd = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $hwnd = [HNative]::FindByTitlePid($title, [uint32]$p.Id)
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "target window '$title' did not appear (own PID $($p.Id))" }
    # Bind this target to en-US and detach its IME so the received text is a
    # faithful read-out of the HID usages rather than an IME transformation.
    $bound = [HNative]::BindUsLayout($hwnd)
    Start-Sleep -Milliseconds 400
    if (-not $bound) { Log "  [WARN] could not bind en-US layout to '$title'" }
    return @{ Pid = $p; Hwnd = $hwnd; Snap = $snap }
}
function Stop-Target($t) {
    try {
        if ($t -and $t.Pid -and -not $t.Pid.HasExited) { [void]$t.Pid.CloseMainWindow() }
        if ($t -and $t.Pid -and -not $t.Pid.WaitForExit(3000)) { $t.Pid.Kill() }
    } catch {
        try { if ($t -and $t.Pid) { Stop-Process -Id $t.Pid.Id -Force -ErrorAction SilentlyContinue } } catch { }
    }
}

$logDir = Split-Path $LogPath -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$evidenceDir = Join-Path $logDir 'e2e-hid-evidence'
if (-not (Test-Path $evidenceDir)) { New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null }

$script:TyperPid = 0
$script:TyperProc = $null
$typer = [IntPtr]::Zero

try {
    Log "=== VirtualHIDTyper REAL HID-mode E2E  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
    Log "EXE: $Exe"

    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Assert-True $isAdmin 'P1-elevated' "elevated=$isAdmin (INF grants device to SY+BA only)"
    if (-not $isAdmin) { throw 'must run elevated to reach the device interface' }

    $svc = Get-CimInstance Win32_SystemDriver -Filter "Name='VirtualHIDDriver'" -ErrorAction SilentlyContinue
    Assert-True ($null -ne $svc -and $svc.State -eq 'Running') 'P2-service-running' "state=$($svc.State) started=$($svc.Started) path=$($svc.PathName)"

    $dev = Get-PnpDevice -InstanceId 'ROOT\VIRTUALHIDDRIVER\0000' -ErrorAction SilentlyContinue
    Assert-True ($null -ne $dev -and $dev.Status -eq 'OK' -and $dev.Problem -eq 'CM_PROB_NONE') 'P2-device-ok' "instance=$($dev.InstanceId) status=$($dev.Status) problem=$($dev.Problem)"

    $vhfKb = @(Get-PnpDevice -Class Keyboard -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'HID\HID_DEVICE_SYSTEM_VHF*' })
    Assert-True ($vhfKb.Count -gt 0) 'P2-vhf-keyboard-enumerated' "count=$($vhfKb.Count) ids=$(($vhfKb | ForEach-Object { $_.InstanceId }) -join ';')"

    $vhfDev = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'VHF\HID_DEVICE_SYSTEM_VHF*' })
    Assert-True ($vhfDev.Count -gt 0) 'P2-vhf-child-enumerated' "count=$($vhfDev.Count)"

    if (-not (Test-Path $Exe)) { throw "exe not found: $Exe" }

    # T3 needs a SPECIFIC stop binding (Ctrl+Alt+X): the app reads its hotkey
    # pair from %APPDATA%\VirtualHIDTyper\settings.ini at startup via
    # GetPrivateProfileStringW, and this harness must not depend on whatever
    # the ambient INI happens to contain. The user's real file (if any) is
    # backed up here and restored byte for byte in the outer finally.
    $iniPath = Join-Path $env:APPDATA 'VirtualHIDTyper\settings.ini'
    $iniBackup = $null
    if (Test-Path -LiteralPath $iniPath) {
        try { $iniBackup = [System.IO.File]::ReadAllBytes($iniPath) } catch { $iniBackup = $null }
    } else {
        $iniDir = Split-Path $iniPath -Parent
        if (-not (Test-Path $iniDir)) { New-Item -ItemType Directory -Force -Path $iniDir | Out-Null }
    }
    # UTF-16 LE + BOM: the encoding GetPrivateProfileStringW reads natively.
    # HotkeySchema=2 skips the legacy-default migration in LoadSettings.
    [System.IO.File]::WriteAllText($iniPath,
        "[Main]`r`nHotkeyStart=7,120`r`nHotkeyStop=3,88`r`nHotkeySchema=2`r`n",
        [System.Text.Encoding]::Unicode)
    Log 'settings.ini prepared: start=Ctrl+Alt+Shift+F9 stop=Ctrl+Alt+X (T3 premise)'

    $typerProc = Start-Process $Exe -PassThru
    $script:TyperProc = $typerProc
    $script:TyperPid = [uint32]$typerProc.Id
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and $typer -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $typer = [HNative]::FindByPidClass($script:TyperPid, 'VirtualHIDTyperWnd')
    }
    if ($typer -eq [IntPtr]::Zero) { throw "our typer (PID $($typerProc.Id)) exposed no window within 10s" }
    $ownPid = [uint32]0
    [void][HNative]::GetWindowThreadProcessId($typer, [ref]$ownPid)
    if ($ownPid -ne $script:TyperPid) { throw "window belongs to PID $ownPid, not ours $($typerProc.Id)" }

    $titleAtLaunch = Get-Title $typer
    Log "Typer HWND=0x$($typer.ToString('X')) PID=$($typerProc.Id) title=$titleAtLaunch"

    # Force the REAL kernel path. settings.ini persists the last selected mode,
    # so an earlier software-mode run leaves SendInput as the startup default.
    # Without this click the test would silently measure SendInput instead of HID.
    $null = Set-ModeRadio $typer $IDC_RADIO_HID
    $title = Get-Title $typer
    Log "title after clicking the HID radio: $title"
    Assert-True ($title -match '\[Virtual HID 模式\]') 'P3-hid-mode-selected' "title=$title"
    Assert-True (-not ($title -match 'SendInput')) 'P3-not-software-mode' 'title carries no SendInput marker'

    Log '--- T1: real HID text (EN letters, digits, punctuation, Enter) ---'
    $t1 = ("Hello World 12345!" + $CRLF + "Punct: ,.;:'" + $DQ + "[]{}()<>/?+-=_@#$%&*" + $CRLF + "AbCdEf 67890 END")
    $snap1 = Join-Path $evidenceDir 't1-actual.txt'
    $t1w = $null
    try {
        $t1w = Start-Target 'HID Target T1' $snap1
        Assert-True (Force-Fg $t1w.Hwnd) 'T1-foreground' "hwnd=0x$($t1w.Hwnd.ToString('X'))"
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t1
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '25'
        [void][HNative]::SendMessage([HNative]::GetDlgItem($typer, $IDC_CHK_SHIFT_NL), $BM_SETCHECK, [IntPtr]$BST_UNCHECKED, [IntPtr]::Zero)
        Send-Cmd $typer $IDC_BTN_START
        $st = Wait-Status $typer '(完成【|已停止【|焦点离开|错误|失败|无法打开)' 120
        Log "  status: $st"
        Assert-True ($st -match '完成【Virtual HID 模式】') 'T1-completed-status' "status=$st"
        $actual1 = Wait-SnapSettled $snap1 10
        Assert-True (Test-Equal $t1 $actual1) 'T1-text-identical' "expected=$(($t1 -replace $CRLF,'<NL>')) | actual=$(($actual1 -replace $CRLF,'<NL>'))"
        if ($st -match '实际输入\s*(\d+)\s*字符') {
            $n = [int]$Matches[1]
            # The app normalizes CRLF -> LF before typing, so the reported count is
            # the normalized length (each CRLF pair collapses to one LF char).
            $normLen = (Norm $t1).Length
            Assert-True ($n -eq $normLen) 'T1-sent-count' "app reported $n chars, expected $normLen (normalized)"
        } else { Assert-True $false 'T1-sent-count' "no count in status=$st" }
    } finally { if ($t1w) { Stop-Target $t1w } }

    Log '--- T2: no residual/stuck keys after completion ---'
    $snap2 = Join-Path $evidenceDir 't2-actual.txt'
    $t2w = $null
    try {
        $t2w = Start-Target 'HID Target T2' $snap2
        Assert-True (Force-Fg $t2w.Hwnd) 'T2-foreground' "hwnd=0x$($t2w.Hwnd.ToString('X'))"
        Start-Sleep -Milliseconds 1500
        $after = Wait-SnapSettled $snap2 6
        Assert-True ([string]::IsNullOrEmpty($after)) 'T2-no-residual-input' "target still empty: '$after'"
    } finally { if ($t2w) { Stop-Target $t2w } }

    Log '--- T3: Ctrl+Alt+X global stop mid-typing ---'
    $longText = ('ABCDEFGHIJKLMNOPQRSTUVWXYZ' * 6)
    $snap3 = Join-Path $evidenceDir 't3-actual.txt'
    $t3w = $null
    try {
        $t3w = Start-Target 'HID Target T3' $snap3
        Assert-True (Force-Fg $t3w.Hwnd) 'T3-foreground' "hwnd=0x$($t3w.Hwnd.ToString('X'))"
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $longText
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '60'
        Send-Cmd $typer $IDC_BTN_START
        # The app runs a 3 second countdown; Ctrl+Alt+X during the countdown
        # would CANCEL (0 chars) instead of stopping a live run. Wait until the
        # status reports an active run before sending the hotkey.
        $started3 = Wait-Status $typer '输入中' 12
        Assert-True ($started3 -match '输入中') 'T3-run-started' 'typing loop is active before the stop hotkey'
        Start-Sleep -Milliseconds 1500
        [HNative]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)
        [HNative]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
        [HNative]::keybd_event(0x58, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [HNative]::keybd_event(0x58, 0, 2, [UIntPtr]::Zero)
        [HNative]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
        [HNative]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero)
        $st3 = Wait-Status $typer '(已停止【|完成【|错误)' 20
        Log "  status: $st3"
        Assert-True ($st3 -match '已停止【Virtual HID 模式】') 'T3-stopped-status' "status=$st3"
        $a3 = Wait-SnapSettled $snap3 8
        Assert-True ($a3.Length -gt 0) 'T3-some-output-arrived' "typed $($a3.Length) chars through the kernel before stop"
        Assert-True ($a3.Length -lt $longText.Length) 'T3-partial-output' "typed $($a3.Length) of $($longText.Length) chars"
        Assert-True ($longText.StartsWith(($a3 -replace $CRLF,'' -replace $LF,''))) 'T3-prefix-consistent' 'output is a clean prefix of the input'
        Start-Sleep -Milliseconds 1200
        $a3b = Wait-SnapSettled $snap3 6
        Assert-True (Test-Equal $a3 $a3b) 'T3-no-input-after-stop' "stable after stop: '$a3b'"
    } finally { if ($t3w) { Stop-Target $t3w } }

    Log '--- T4: Stop button mid-typing ---'
    $snap4 = Join-Path $evidenceDir 't4-actual.txt'
    $t4w = $null
    try {
        $t4w = Start-Target 'HID Target T4' $snap4
        Assert-True (Force-Fg $t4w.Hwnd) 'T4-foreground' "hwnd=0x$($t4w.Hwnd.ToString('X'))"
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $longText
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '60'
        Send-Cmd $typer $IDC_BTN_START
        $started4 = Wait-Status $typer '输入中' 12
        Assert-True ($started4 -match '输入中') 'T4-run-started' 'typing loop is active before the Stop button'
        Start-Sleep -Milliseconds 1500
        Send-Cmd $typer $IDC_BTN_STOP
        $st4 = Wait-Status $typer '(已停止【|完成【|错误)' 20
        Log "  status: $st4"
        Assert-True ($st4 -match '已停止【Virtual HID 模式】') 'T4-stopped-status' "status=$st4"
        $a4 = Wait-SnapSettled $snap4 8
        Assert-True ($a4.Length -gt 0) 'T4-some-output-arrived' "typed $($a4.Length) chars through the kernel before stop"
        Assert-True ($a4.Length -lt $longText.Length) 'T4-partial-output' "typed $($a4.Length) of $($longText.Length)"
        Start-Sleep -Milliseconds 1200
        $a4b = Wait-SnapSettled $snap4 6
        Assert-True (Test-Equal $a4 $a4b) 'T4-no-input-after-stop' "stable after stop: '$a4b'"
    } finally { if ($t4w) { Stop-Target $t4w } }

    Log '--- T5: post-stop key-release sanity (fresh run types cleanly) ---'
    $t5 = 'clean'
    $snap5 = Join-Path $evidenceDir 't5-actual.txt'
    $t5w = $null
    try {
        $t5w = Start-Target 'HID Target T5' $snap5
        Assert-True (Force-Fg $t5w.Hwnd) 'T5-foreground' "hwnd=0x$($t5w.Hwnd.ToString('X'))"
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t5
        Set-WmText ([HNative]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '40'
        Send-Cmd $typer $IDC_BTN_START
        $st5 = Wait-Status $typer '(完成【|已停止【|错误)' 40
        Log "  status: $st5"
        Assert-True ($st5 -match '完成【Virtual HID 模式】') 'T5-completed' "status=$st5"
        $a5 = Wait-SnapSettled $snap5 8
        Assert-True (Test-Equal $t5 $a5) 'T5-no-modifier-residue' "got '$a5' (a stuck Shift would give 'CLEAN')"
    } finally { if ($t5w) { Stop-Target $t5w } }
}
catch {
    Log "FATAL: $($_.Exception.Message)"
    Log $_.ScriptStackTrace
    $script:Failures++
}
finally {
    try {
        if ($typer -ne [IntPtr]::Zero -and [HNative]::IsWindow($typer)) {
            [void][HNative]::SendMessage($typer, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    } catch { }
    try {
        if ($script:TyperProc -and -not $script:TyperProc.HasExited) {
            if (-not $script:TyperProc.WaitForExit(3000)) { $script:TyperProc.Kill() }
        }
    } catch { }
    foreach ($t in $script:Targets) { Stop-Target $t }
    # Restore the user's settings.ini byte for byte (the app is dead by now,
    # so nothing can rewrite it behind our back).
    try {
        if ($null -ne $iniBackup) { [System.IO.File]::WriteAllBytes($iniPath, $iniBackup) }
        elseif (Test-Path -LiteralPath $iniPath) { Remove-Item -LiteralPath $iniPath -Force }
    } catch { }
}

Log ''
if ($script:Failures -eq 0) { Log 'RESULT: ALL HID E2E CHECKS PASSED' }
else { Log "RESULT: $($script:Failures) HID E2E FAILURE(S)" }
[System.IO.File]::WriteAllLines($LogPath, [string[]]$script:LogLines, (New-Object System.Text.UTF8Encoding($true)))
if ($script:Failures -eq 0) { exit 0 } else { exit 1 }
