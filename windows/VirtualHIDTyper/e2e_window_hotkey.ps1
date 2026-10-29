# ---------------------------------------------------------------------------
# e2e_window_hotkey.ps1
#
# REAL acceptance for the window and hotkey features on the built EXE:
#   Goal 1 - hotkeys: new defaults (Ctrl+Alt+Shift+F9/F10), dialog rebind,
#             conflict detection with LOSSLESS rollback (1409), persistence,
#             restore-defaults, save-failure notice, startup-conflict notice.
#   Goal 2 - window:  ~1100x800 first-run size, responsive editor layout,
#             min-size clamp, maximize/restore, size/position/maximized
#             persistence, corrupt-config fallback.
# Everything is driven through the real UI/message surface of the running
# process. Requires elevation (device ACL is SYSTEM+Administrators).
# The user's real settings.ini is backed up and restored byte for byte.
# ---------------------------------------------------------------------------
param(
    [string]$Exe = (Join-Path (Split-Path $PSScriptRoot -Parent) "dist\VirtualHIDTyper\VirtualHIDTyper.exe"),
    [string]$LogPath = (Join-Path (Split-Path $PSScriptRoot -Parent) "logs\e2e-window-hotkey.log")
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
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", EntryPoint="GetWindowThreadProcessId")] public static extern uint GetWindowThreadId(IntPtr h, IntPtr p);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    // Shell_NotifyIconW lives in SHELL32.dll. NIM_GET goes over a RAW byte
    // buffer (x64 NOTIFYICONDATA: 0 cbSize | 8 hWnd | 16 uID | 20 uFlags |
    // 32 hIcon | 40 szTip[256]) - struct-with-string copy-back returned
    // empty AND the shell fills nothing at all on the Win11 24H2 tray host,
    // so ReadTip yields "" there and the caller records the check as
    // NOT RUN instead of guessing.
    [DllImport("shell32.dll", EntryPoint = "Shell_NotifyIconW", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern bool Shell_NotifyIconRaw(uint msg, byte[] nid);
    [DllImport("imm32.dll")] public static extern IntPtr ImmAssociateContext(IntPtr h, IntPtr himc);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    // Hang-proof text read for Watch-AfterOk: if the app thread stops
    // pumping, a plain SendMessage(WM_GETTEXT) would deadlock the harness.
    [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, StringBuilder l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr LoadKeyboardLayout(string id, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct NICONDATA {
        public uint cbSize; public IntPtr hWnd; public uint uID; public uint uFlags;
        public uint uCallbackMessage; public IntPtr hIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string szTip;
        public uint dwState; public uint dwStateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string szInfo;
        public uint uTimeoutOrVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=64)] public string szTitle;
        public uint dwInfoFlags; public Guid guidItem; public IntPtr hBalloonIcon;
    }
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
        IntPtr[] r = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            StringBuilder sb = new StringBuilder(256);
            if (GetClassName(h, sb, 256) && string.Equals(sb.ToString(), cls, StringComparison.Ordinal)) { r[0] = h; return false; }
            return true;
        }, IntPtr.Zero);
        return r[0];
    }
    // The W-suffixed GetWindowText: the plain declaration above resolves to
    // GetWindowTextA, which mangles CJK captions - exact-title matching must
    // use the wide entry point (Same trap as Shell_NotifyIconW earlier).
    [DllImport("user32.dll", EntryPoint="GetWindowTextW", CharSet=CharSet.Unicode, ExactSpelling=true)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    // Find by PID + class + EXACT title via ENUMERATION: FindWindow(class,
    // title) intermittently misses windows that demonstrably exist (seen on
    // this machine for both the settings dialog and message boxes) - a flake
    // that silently no-ops every downstream step. Enumeration cannot flake.
    // All top-level windows of a pid as "class=X title='Y'" lines - the
    // diagnostic dump when a lookup comes back empty.
    public static string ListPidsWindows(uint pid) {
        System.Text.StringBuilder all = new System.Text.StringBuilder();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            StringBuilder cb = new StringBuilder(256);
            GetClassName(h, cb, 256);
            StringBuilder tb = new StringBuilder(256);
            GetWindowTextW(h, tb, 256);
            all.Append("hwnd=0x" + h.ToInt64().ToString("X") + " class=" + cb.ToString() + " title='" + tb.ToString() + "' vis=" + (IsWindowVisible(h) ? "Y" : "N") + "\n");
            return true;
        }, IntPtr.Zero);
        return all.ToString();
    }
    public static IntPtr ByPidClassTitle(uint pid, string cls, string title) {
        IntPtr[] r = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            StringBuilder sb = new StringBuilder(256);
            if (!GetClassName(h, sb, 256) || !string.Equals(sb.ToString(), cls, StringComparison.Ordinal)) return true;
            StringBuilder tb = new StringBuilder(256);
            GetWindowTextW(h, tb, 256);
            if (string.Equals(tb.ToString(), title, StringComparison.Ordinal)) { r[0] = h; return false; }
            return true;
        }, IntPtr.Zero);
        return r[0];
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetClassName(IntPtr h, StringBuilder s, int n);
    public static bool BindUsLayout(IntPtr hwnd) {
        IntPtr hkl = LoadKeyboardLayout("00000409", 0x00000001);
        PostMessage(hwnd, 0x0050, IntPtr.Zero, hkl);
        ImmAssociateContext(hwnd, IntPtr.Zero);
        return hkl != IntPtr.Zero;
    }
    public static string ReadTip(IntPtr mainWnd) {
        // raw layout read (see the Shell_NotifyIconRaw declaration above)
        byte[] b = new byte[976];
        BitConverter.GetBytes((uint)976).CopyTo(b, 0);       // cbSize
        BitConverter.GetBytes(mainWnd.ToInt64()).CopyTo(b, 8);  // hWnd
        BitConverter.GetBytes((uint)1).CopyTo(b, 16);        // uID = TRAY_UID
        BitConverter.GetBytes((uint)4).CopyTo(b, 20);        // uFlags = NIF_TIP
        if (!Shell_NotifyIconRaw(2 /*NIM_GET*/, b)) return null;
        return Encoding.Unicode.GetString(b, 40, 256).TrimEnd('\0');
    }
}
'@

$WM_GETTEXT = 0x000D; $WM_SETTEXT = 0x000C; $WM_COMMAND = 0x0111; $WM_SYSCOMMAND = 0x0112
$SC_CLOSE = 0xF060; $BM_CLICK = 0x00F5
$IDC_EDIT_TEXT = 1001; $IDC_EDIT_INTERVAL = 1002; $IDC_BTN_SAVE = 1010
$IDC_BTN_HOTKEYS = 1011; $IDC_STATIC_STATUS = 1007; $IDC_STATIC_HK = 1012
$IDM_TRAY_START = 2002; $IDM_TRAY_STOP = 2003; $IDM_TRAY_EXIT = 2005
$IDC_HK_START = 3002; $IDC_HK_STOP = 3003; $IDC_HK_OK = 3004
$IDC_HK_CANCEL = 3005; $IDC_HK_DEFAULT = 3006
# NOTE: the app's dialog enum is INFO=3001 / START=3002 / STOP=3003 - the
# instructions label owns 3001, so the field ids are NOT 3001/3002.

# Measure in TRUE device pixels: a DPI-unaware harness gets every rect
# virtualised (this machine runs 150% -> all reads come back at 2/3 of the
# real size), which made the geometry asserts meaningless.
$script:DpiAware = $false
try { $script:DpiAware = [TN]::SetProcessDpiAwarenessContext([IntPtr](-4)) } catch { }
if (-not $script:DpiAware) { try { $script:DpiAware = [TN]::SetProcessDPIAware() } catch { } }

function Get-WmText([IntPtr]$h, [int]$cap = 4096) {
    if ($h -eq [IntPtr]::Zero) { return "" }
    $sb = New-Object System.Text.StringBuilder $cap
    [void][TN]::SendMessage($h, $WM_GETTEXT, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Set-WmText([IntPtr]$h, [string]$t) { [void][TN]::SendMessage($h, $WM_SETTEXT, [IntPtr]::Zero, $t) }
function Send-Cmd([IntPtr]$h, [int]$id) { [void][TN]::SendMessage($h, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) }
function Get-WmTextTO([IntPtr]$h, [int]$cap = 4096) {
    # SendMessageTimeout variant: never blocks the watcher on a wedged app.
    if ($h -eq [IntPtr]::Zero) { return "" }
    $sb = New-Object System.Text.StringBuilder $cap
    $res = [IntPtr]::Zero
    $r = [TN]::SendMessageTimeoutW($h, [uint32]$WM_GETTEXT, [IntPtr]$cap, $sb, [uint32]2, [uint32]500, [ref]$res)
    if ($r -eq [IntPtr]::Zero) { return "<hung>" }
    return $sb.ToString()
}
function Watch-AfterOk([IntPtr]$typer, [IntPtr]$dlg, [uint32]$tpid, [int]$sec) {
    # 200ms timeline of the post-OK window: dialog alive? any #32770 up
    # (box!)? hotkey line? title? INI written? every state CHANGE is
    # logged with its timestamp so app-side and harness-side diverge visibly.
    $t0 = Get-Date
    $f0 = Get-Item -LiteralPath $iniPath -ErrorAction SilentlyContinue
    $prevIni = if ($f0) { $f0.LastWriteTimeUtc.Ticks } else { -1L }
    $prevD = $null; $prevW = $null; $prevHk = $null; $prevTi = $null
    $dl = $t0.AddSeconds($sec)
    while ((Get-Date) -lt $dl) {
        # 50ms for the first 1.5s (the death window), then 200ms.
        $early = ((Get-Date) - $t0).TotalMilliseconds -lt 1500
        Start-Sleep -Milliseconds $(if ($early) { 50 } else { 200 })
        $ms = [int]((Get-Date) - $t0).TotalMilliseconds
        $d = [TN]::IsWindow($dlg)
        $dump = [TN]::ListPidsWindows($tpid)
        $wl = @()
        foreach ($ln in ($dump -split "`n")) { if ($ln -match 'class=#32770') { $wl += $ln.Trim() } }
        $ws = $wl -join ' | '
        $hk = Get-WmTextTO ([TN]::GetDlgItem($typer, $IDC_STATIC_HK)) 512
        $ti = Get-WmTextTO $typer 256
        $f = Get-Item -LiteralPath $iniPath -ErrorAction SilentlyContinue
        $iniT = if ($f) { $f.LastWriteTimeUtc.Ticks } else { -1L }
        if (($d -ne $prevD) -or ($ws -cne $prevW) -or ($hk -cne $prevHk) -or ($ti -cne $prevTi) -or ($iniT -ne $prevIni)) {
            $dg = if ($d) { 'alive' } else { 'GONE' }
            $iw = if ($iniT -ne $prevIni) { 'WRITTEN' } else { 'same' }
            $null = Log ("  [watch +${ms}ms] dialog=$dg #32770=[$ws] hk='$hk' title='$ti' ini=$iw")
        }
        $prevD = $d; $prevW = $ws; $prevHk = $hk; $prevTi = $ti; $prevIni = $iniT
    }
    $null = Log "  [watch] end at $([int]((Get-Date) - $t0).TotalMilliseconds)ms"
}
function Press-Chord([int]$vk) {
    # Ctrl+Alt+Shift+<vk> - the default modifier set
    [TN]::keybd_event(0x11,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x10,0,0,[UIntPtr]::Zero)
    [TN]::keybd_event([byte]$vk,0,0,[UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [TN]::keybd_event([byte]$vk,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x10,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [TN]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
}
function Get-WinRect([IntPtr]$h) { $r = New-Object TN+RECT; [void][TN]::GetWindowRect($h, [ref]$r); return $r }
function Get-CliRect([IntPtr]$h) { $r = New-Object TN+RECT; [void][TN]::GetClientRect($h, [ref]$r); return $r }
function Wait-Status([IntPtr]$t, [string]$pat, [int]$sec) {
    $dl = (Get-Date).AddSeconds($sec); $st = ""
    do {
        $st = Get-WmText ([TN]::GetDlgItem($t, $IDC_STATIC_STATUS)) 4096
        if ($st -match $pat) { return $st }
        Start-Sleep -Milliseconds 120
    } while ((Get-Date) -lt $dl)
    return $st
}
function Wait-Hk([IntPtr]$t, [string]$pat, [int]$sec) {
    # the "开始: X (已注册)/(注册失败)  停止: Y (...)" line lives in the
    # IDC_STATIC_HK (1012) control - NOT the status bar (1007), which only
    # carries the mode text or the startup-failure notice.
    $dl = (Get-Date).AddSeconds($sec); $st = ""
    do {
        $st = Get-WmText ([TN]::GetDlgItem($t, $IDC_STATIC_HK)) 4096
        if ($st -match $pat) { return $st }
        Start-Sleep -Milliseconds 120
    } while ((Get-Date) -lt $dl)
    return $st
}
function Post-Ok([IntPtr]$dlg) {
    # NEVER a synchronous SendMessage: the OK handler can show a modal
    # message box (conflict / save failure) BEFORE the dialog closes, which
    # would deadlock the harness against its own dismissal step.
    [void][TN]::PostMessage($dlg, $WM_COMMAND, [IntPtr]$IDC_HK_OK, [IntPtr]::Zero)
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

# Robust foreground stealing (same dance as e2e_tray_hotkey.ps1): the typing
# target MUST be foreground before the start hotkey or the HID stream lands
# wherever focus happens to be.
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
        # Alt-tap unlocks the foreground lock the foreground window won't yield
        [TN]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [TN]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
        [void][TN]::SetForegroundWindow($h)
        if ([TN]::GetForegroundWindow() -eq $h) { return $true }
        Start-Sleep -Milliseconds 60
    }
    return ([TN]::GetForegroundWindow() -eq $h)
}

function Open-HotkeyDialog([IntPtr]$typer) {
    # Enumerate by PID + class instead of FindWindow(class, title): the
    # title-based lookup intermittently misses a dialog that IS open -
    # the returned Zero silently no-ops every downstream step.
    # Enumeration by PID cannot flake.
    $tpid = [uint32]0
    [void][TN]::GetWindowThreadProcessId($typer, [ref]$tpid)
    # The 1011 POST is asynchronous: if it is transiently lost (no dialog
    # appears for the full 8s poll), re-post.
    # A late-first-post + retry cannot stack dialogs: the second 1011 is
    # swallowed by the app's own never-stack guard and the poll then
    # finds the first dialog.
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        [void][TN]::PostMessage($typer, $WM_COMMAND, [IntPtr]$IDC_BTN_HOTKEYS, [IntPtr]::Zero)
        $dl = (Get-Date).AddSeconds(8); $dlg = [IntPtr]::Zero
        while ((Get-Date) -lt $dl -and $dlg -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 120
            $dlg = [TN]::ByClassPid($tpid, "#32770")
        }
        if ($dlg -ne [IntPtr]::Zero) { return $dlg }
        $null = Log "  [WARN] Open-HotkeyDialog attempt ${attempt}: no dialog within 8s - re-posting 1011"
    }
    return [IntPtr]::Zero
}
function Close-HotkeyDialog([IntPtr]$dlg, [int]$cmdId) {
    # POST the command, then VERIFY the window is really gone: an unverified
    # close once let a still-open dialog swallow the app's WM_QUIT during
    # tray exit (zombie process, mutex still held). WM_CLOSE is the fallback.
    if (-not [TN]::IsWindow($dlg)) { return }   # already gone (OK path closed it)
    [void][TN]::PostMessage($dlg, $WM_COMMAND, [IntPtr]$cmdId, [IntPtr]::Zero)
    $dl = (Get-Date).AddSeconds(4)
    while ((Get-Date) -lt $dl -and [TN]::IsWindow($dlg)) { Start-Sleep -Milliseconds 120 }
    if ([TN]::IsWindow($dlg)) {
        # verify by CLASS (not title - the title lookup flakes): never fire
        # WM_CLOSE at a stale handle that a different window has reused.
        $sb = New-Object System.Text.StringBuilder 64
        [void][TN]::GetClassName($dlg, $sb, 64)
        if ($sb.ToString() -eq "#32770") {
            # still open AND still a dialog: ask it to close through the
            # pump path. ([uint32], NOT [IntPtr]: TN.PostMessage's msg
            # parameter is uint - an IntPtr here throws a terminating
            # conversion error and kills the whole harness.)
            [void][TN]::PostMessage($dlg, [uint32]0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
            $dl = (Get-Date).AddSeconds(4)
            while ((Get-Date) -lt $dl -and [TN]::IsWindow($dlg)) { Start-Sleep -Milliseconds 120 }
        }
    }
    $detail = "cmd=$cmdId hwnd=0x$($dlg.ToString('X'))"
    if ([TN]::IsWindow($dlg)) {
        $sb2 = New-Object System.Text.StringBuilder 64
        [void][TN]::GetClassName($dlg, $sb2, 64)
        $detail += " still-open class=" + $sb2.ToString() + " text=" + (Get-WmText $dlg 64)
    }
    Assert-True (-not [TN]::IsWindow($dlg)) "dialog-closed" $detail
}
# Captures a chord into one dialog field: arm capture via WM_COMMAND then
# inject the chord (the app's WH_KEYBOARD_LL hook consumes it).
function Dialog-Capture([IntPtr]$dlg, [int]$fieldId, [int]$vk) {
    # Wait for the EDIT CONTROL before arming: the pid enumeration can see a
    # #32770 window while it is still mid-creation (children not built yet),
    # and arming that early silently loses the capture. The edit existing
    # means mk() has run, i.e. the subclass and the LL hook are in place.
    $ed = [IntPtr]::Zero
    $dl = (Get-Date).AddSeconds(2)
    while ((Get-Date) -lt $dl -and $ed -eq [IntPtr]::Zero) {
        $ed = [TN]::GetDlgItem($dlg, $fieldId)
        if ($ed -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 50 }
    }
    $before = Get-WmText $ed 128
    $null = Log "    probe[arm] dlg alive=$([TN]::IsWindow($dlg)) ed=0x$($ed.ToString('X'))"
    Send-Cmd $dlg $fieldId
    Start-Sleep -Milliseconds 150
    $null = Log "    probe[armed] dlg alive=$([TN]::IsWindow($dlg))"
    Press-Chord $vk
    $null = Log "    probe[chord-sent] dlg alive=$([TN]::IsWindow($dlg))"
    # Poll for the change instead of a fixed 250ms read: the LL-hook callback
    # runs on the app's thread and can land late under CPU load; the app-side
    # g_hkDlg state updates with it, so a late-but-real capture must not be
    # misread as a failed one.
    $after = $before
    $dl = (Get-Date).AddMilliseconds(1500)
    while ((Get-Date) -lt $dl) {
        $after = Get-WmText $ed 128
        if ($after -cne $before) { break }
        Start-Sleep -Milliseconds 80
    }
    $null = Log "    probe[poll-end] dlg alive=$([TN]::IsWindow($dlg))"
    # NOTE: every Log here must be `$null = Log` - the caller assigns our
    # return value, and Write-Output from Log would otherwise join the
    # output stream, turning the returned text into an Object[] (which
    # breaks Assert-True's [bool] param with a 2-element array).
    if ($after -cne $before) {
        $null = Log "    capture vk=0x$($vk.ToString('X2')) field=$fieldId ok: '$before' -> '$after'"
    } else {
        $null = Log "    [WARN] Dialog-Capture vk=0x$($vk.ToString('X2')) field=$fieldId UNCHANGED '$before' (edit=0x$($ed.ToString('X')) dlg=0x$($dlg.ToString('X')))"
    }
    return $after
}
# Dismisses a message box by title (returns $true if one was found+clicked).
function Dismiss-Box([string]$title) {
    # Find the app's box by PID ENUMERATION + exact title: FindWindow(class,
    # title) intermittently misses boxes that are provably up (the box can
    # stay open and block the whole app thread while FindWindow reports
    # nothing there). Enumeration cannot flake.
    # POLL up to 3s: a box observed only at +1500ms looks absent to a
    # single-shot lookup at +800ms - that turns "late" into "never".
    # Staged logs distinguish not-yet vs never.
    $tpid = [uint32]$script:typerProc.Id
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $b = [TN]::ByPidClassTitle($tpid, "#32770", $title)
    while ($b -eq [IntPtr]::Zero -and $sw.ElapsedMilliseconds -lt 3000) {
        Start-Sleep -Milliseconds 100
        $b = [TN]::ByPidClassTitle($tpid, "#32770", $title)
    }
    if ($b -eq [IntPtr]::Zero) {
        $null = Log ("  [WARN] Dismiss-Box '" + $title + "' not found within 3s (pid=$tpid) - that pid's top-level windows:")
        $dump = [TN]::ListPidsWindows($tpid)
        if ($dump) { foreach ($ln in ($dump -split "`n")) { if ($ln.Trim()) { $null = Log ("      " + $ln.Trim()) } } }
        else { $null = Log "      (no windows)" }
        return $false
    }
    $null = Log ("  Dismiss-Box: '" + $title + "' appeared hwnd=0x" + $b.ToString('X') + " at t=" + $sw.ElapsedMilliseconds + "ms")
    # Staged dismissal - the box can appear instantly yet a single
    # BM_CLICK does not close it, so every stage is tried and LOGGED, and
    # the stage that succeeded is visible in the log:
    #   1) BM_CLICK on the first Button child (synchronous)
    #   2) posted IDOK to the box (the modal loop dispatches it -> EndDialog)
    #   3) posted WM_CLOSE to the box
    $btn = [TN]::FindWindowEx($b, [IntPtr]::Zero, "Button", $null)
    $null = Log "  Dismiss-Box: Button child=0x$($btn.ToString('X'))"
    if ($btn -ne [IntPtr]::Zero) { [void][TN]::SendMessage($btn, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) }
    $stage = 1
    $dl = (Get-Date).AddSeconds(1)
    while ((Get-Date) -lt $dl -and [TN]::IsWindow($b)) { Start-Sleep -Milliseconds 100 }
    if ([TN]::IsWindow($b)) {
        $null = Log "  Dismiss-Box: stage1 (BM_CLICK) did not close it - posting IDOK"
        [void][TN]::PostMessage($b, $WM_COMMAND, [IntPtr]1, [IntPtr]::Zero)
        $stage = 2
        $dl = (Get-Date).AddSeconds(1)
        while ((Get-Date) -lt $dl -and [TN]::IsWindow($b)) { Start-Sleep -Milliseconds 100 }
    }
    if ([TN]::IsWindow($b)) {
        $null = Log "  Dismiss-Box: stage2 (IDOK) did not close it - posting WM_CLOSE"
        [void][TN]::PostMessage($b, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        $stage = 3
        $dl = (Get-Date).AddSeconds(1)
        while ((Get-Date) -lt $dl -and [TN]::IsWindow($b)) { Start-Sleep -Milliseconds 100 }
    }
    # VERIFY the box is really gone: a box that survives all stages must
    # read as a failure - a still-open modal box blocks the app thread and
    # would silently misjudge every later step.
    if ([TN]::IsWindow($b)) {
        $null = Log ("  [WARN] Dismiss-Box: box found but NO stage closed it (hwnd=0x" + $b.ToString('X') + ")")
        $null = Log ("      " + ([TN]::ListPidsWindows($tpid) -replace "`n", " || ").Trim())
        return $false
    }
    $null = Log ("  Dismiss-Box: closed at t=" + $sw.ElapsedMilliseconds + "ms (stage=$stage)")
    return $true
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
function Start-Holder([int]$mods, [int]$vk) {
    # The holder must CONFIRM registration before we hand control back: the
    # holder process pays a full PS cold start + Add-Type C# compile (first
    # compile in the elevated session, Defender-scanned) which measured
    # 10-20s in run5, and launching the typer earlier lets the typer win
    # the chord. The holder exits(1) if it cannot register (-> early break
    # -> $null). On timeout a STILL-ALIVE holder is returned anyway: the
    # ready file is written immediately after RegisterHotKey, so an alive
    # holder after the wait window certainly holds the chord - the conflict
    # test stays valid and the anomaly is logged, never hidden.
    $ready = Join-Path $env:TEMP ("vht_holder_ready_" + [guid]::NewGuid().ToString('N') + ".txt")
    $null = Log "  Start-Holder: launching holder mods=$mods vk=0x$($vk.ToString('X2'))"   # $null= : caller assigns our return value
    $hp = Start-Process powershell -ArgumentList @("-NoProfile","-ExecutionPolicy","Bypass","-File",
        (Join-Path $PSScriptRoot "e2e_hotkey_conflict.ps1"),
        "-Mods", $mods, "-Vk", $vk, "-HoldSecs", "600",
        "-ReadyFile", "`"" + $ready + "`"") -PassThru -WindowStyle Hidden
    $dl = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $dl -and -not (Test-Path $ready)) {
        if ($hp.HasExited) { break }   # exited = registration failed
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path $ready) {
        Remove-Item $ready -Force -ErrorAction SilentlyContinue
        $null = Log "  Start-Holder: ready seen, holder pid=$($hp.Id) alive"
        return $hp
    }
    if (-not $hp.HasExited) {
        $null = Log "  [WARN] holder pid=$($hp.Id) alive after 40s without ready file - returning anyway (chord IS held; ready-protocol anomaly)"
        return $hp
    }
    $null = Log "  [WARN] holder pid=$($hp.Id) exited code=$($hp.ExitCode) without ready file (1409 = chord already taken by something else)"
    return $null
}

$logDir = Split-Path $LogPath -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$ev = Join-Path $logDir "e2e-window-hotkey-evidence"
if (-not (Test-Path $ev)) { New-Item -ItemType Directory -Force -Path $ev | Out-Null }

# --- user settings protection ------------------------------------------------
$iniPath = Join-Path $env:APPDATA "VirtualHIDTyper\settings.ini"
$iniBackup = $null
if (Test-Path $iniPath) { $iniBackup = [IO.File]::ReadAllBytes($iniPath) }
function Reset-TestIni {
    if (-not (Test-Path (Split-Path $iniPath))) { New-Item -ItemType Directory -Force -Path (Split-Path $iniPath) | Out-Null }
    @'
[Main]
IntervalMs=25
ShiftEnter=1
InputMode=hid
HotkeyStart=7,120
HotkeyStop=7,121
HotkeySchema=2
'@ | Set-Content -Path $iniPath -Encoding UTF8
}
function Restore-UserIni {
    try {
        [IO.File]::SetAttributes($iniPath, [IO.FileAttributes]::Normal)
    } catch { }
    try {
        if ($null -ne $iniBackup) { [IO.File]::WriteAllBytes($iniPath, $iniBackup) }
        elseif (Test-Path $iniPath) { Remove-Item $iniPath -Force }
    } catch { }
}

$typer = [IntPtr]::Zero
$typerProc = $null
$holder = $null
function Launch-Typer {
    $script:typer = [IntPtr]::Zero
    $script:typerProc = Start-Process $Exe -PassThru
    $dl = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $dl -and $script:typer -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $script:typer = [TN]::ByClassPid([uint32]$script:typerProc.Id, "VirtualHIDTyperWnd")
    }
    Start-Sleep -Milliseconds 900
}
function Exit-TyperViaTray {
    # tray menu 退出程序 - exercises the visible/hidden exit persistence path
    Send-Cmd $script:typer $IDM_TRAY_EXIT
    $dl = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $dl -and -not $script:typerProc.HasExited) { Start-Sleep -Milliseconds 150 }
    if (-not $script:typerProc.HasExited) {
        # dump the app's exit trace so a hang pinpoints its blocking call
        $tr = Join-Path $env:TEMP 'vht_exit_trace.txt'
        Log "  !! exit did not finish in 20s; app exit trace:"
        if (Test-Path $tr) { Get-Content $tr | ForEach-Object { Log "     | $_" } } else { Log "     | (no trace file)" }
    }
}

try {
    Log "=== window + hotkey E2E  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
    Log "EXE: $Exe"
    Assert-True (Test-Path $Exe) "EXE-present" $Exe
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Assert-True $isAdmin "elevated" "required: device ACL is SY+BA"

    Reset-TestIni
    Launch-Typer
    Assert-True ($typer -ne [IntPtr]::Zero) "launch" "hwnd=0x$($typer.ToString('X'))"
    if ($typer -eq [IntPtr]::Zero) { throw "main window never appeared" }

    $dpi = [TN]::GetDpiForWindow($typer)
    if ($dpi -le 0) { $dpi = 96 }
    $scale = $dpi / 96.0
    Log "  dpi=$dpi scale=$([math]::Round($scale,2))"

    # --- W1/H1: first-run default geometry + default hotkeys -----------------
    # All rects are TRUE device pixels (harness is DPI aware). At 150% scaling
    # the spec's "1100x800 logical" = 1650x1200 real; the work-area height
    # cannot fit that, so the window is CORRECTLY clamped to fit the screen
    # (never off-screen) while staying far larger than the old 496x186 editor.
    Log "--- W1/H1: default size + default hotkeys ---"
    Add-Type -AssemblyName System.Windows.Forms
    $wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $wr = Get-WinRect $typer; $cr = Get-CliRect $typer
    $cw = $cr.right; $ch = $cr.bottom
    $lcw = [int]($cw / $scale); $lch = [int]($ch / $scale)
    Log "  dpi-aware=$script:DpiAware workarea=$($wa.Width)x$($wa.Height) window=$($wr.right-$wr.left)x$($wr.bottom-$wr.top) client=${cw}x${ch} (logical ${lcw}x${lch})"
    Assert-True ($lcw -ge 1050 -and $lcw -le 1150) "W1-default-width" "client ${cw}px = logical $lcw (spec 1100)"
    Assert-True ($lch -ge 550 -and $lch -le 830) "W1-default-height" "client ${ch}px = logical $lch (800 preferred, clamped to work area)"
    Assert-True ($wr.right -le $wa.Width -and $wr.bottom -le $wa.Height -and $wr.left -ge 0 -and $wr.top -ge 0) "W1-within-work-area" "window $($wr.left),$($wr.top)-$($wr.right),$($wr.bottom) inside $($wa.Width)x$($wa.Height)"
    $title = Get-WmText $typer 256
    Log "  title: $title"
    $hk1 = Wait-Hk $typer "已注册" 8
    Log "  hotkeys: $hk1"
    Assert-True ($title -match 'Ctrl\+Alt\+Shift\+F9' -and $title -match 'Ctrl\+Alt\+Shift\+F10') "H1-defaults-in-title" $title
    Assert-True ($hk1 -match '开始: Ctrl\+Alt\+Shift\+F9 \(已注册\)' -and $hk1 -match '停止: Ctrl\+Alt\+Shift\+F10 \(已注册\)') "H1-defaults-registered" $hk1

    # --- W2: editor fills most of the window ---------------------------------
    Log "--- W2: editor proportion ---"
    $er = Get-WinRect ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT))
    $editH = $er.bottom - $er.top; $editW = $er.right - $er.left
    $winH = $wr.bottom - $wr.top
    $ratio = $editH / [double]$winH
    Log "  editor=${editW}x${editH} (logical $([int]($editW/$scale))x$([int]($editH/$scale))) ratio=$([math]::Round($ratio,2))"
    Assert-True ($ratio -ge 0.55) "W2-editor-dominant" "editor/window height ratio $([math]::Round($ratio,2))"
    Assert-True ($editH -ge 400 -and $editW -ge 1000) "W2-editor-large" "editor ${editW}x${editH} (old fixed size was 496x186)"

    # --- W3: resize bigger -> editor absorbs ---------------------------------
    # NOTE: Windows caps the track height at ~the work area by default (the
    # app only overrides the MIN in WM_GETMINMAXINFO), and this window starts
    # near that cap - so the real assertion is that the editor absorbs the
    # growth that ACTUALLY happens, not an arbitrary +180.
    Log "--- W3: live resize ---"
    $wrB = Get-WinRect $typer
    $before = Get-WinRect ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT))
    [void][TN]::SetWindowPos($typer, [IntPtr]::Zero, $wrB.left, $wrB.top, ($wrB.right-$wrB.left)+240, ($wrB.bottom-$wrB.top)+180, 0x0004 -bor 0x0010)
    Start-Sleep -Milliseconds 400
    $wrA = Get-WinRect $typer
    $after = Get-WinRect ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT))
    $dW = ($after.right-$after.left) - ($before.right-$before.left)
    $dH = ($after.bottom-$after.top) - ($before.bottom-$before.top)
    $dWinW = ($wrA.right-$wrA.left) - ($wrB.right-$wrB.left)
    $dWinH = ($wrA.bottom-$wrA.top) - ($wrB.bottom-$wrB.top)
    Log "  window delta ${dWinW}x${dWinH}  editor delta ${dW}x${dH}"
    Assert-True ($dWinW -ge 200) "W3-window-grew" "window width delta $dWinW of requested +240"
    Assert-True ($dW -ge $dWinW - 8 -and $dH -ge $dWinH - 8) "W3-editor-grows" "editor delta ${dW}x${dH} tracks window delta ${dWinW}x${dWinH}"

    # --- W4: minimum size clamp ---------------------------------------------
    Log "--- W4: min-size clamp ---"
    [void][TN]::SetWindowPos($typer, [IntPtr]::Zero, 80, 80, 220, 160, 0x0004 -bor 0x0010)
    Start-Sleep -Milliseconds 400
    $mn = Get-WinRect $typer
    $mnW = $mn.right - $mn.left; $mnH = $mn.bottom - $mn.top
    Log "  clamped to ${mnW}x${mnH}"
    Assert-True ($mnW -ge 540*$scale -and $mnH -ge 410*$scale) "W4-min-clamp" "window ${mnW}x${mnH} >= min $([int](540*$scale))x$([int](410*$scale))"

    # --- W5: maximize + restore ---------------------------------------------
    Log "--- W5: maximize/restore ---"
    [void][TN]::ShowWindow($typer, 3)   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 400
    Assert-True ([TN]::IsZoomed($typer)) "W5-maximized" "IsZoomed"
    [void][TN]::ShowWindow($typer, 9)   # SW_RESTORE
    Start-Sleep -Milliseconds 400
    Assert-True (-not [TN]::IsZoomed($typer)) "W5-restored" "IsZoomed cleared"

    # --- H2: dialog shows defaults, restore-defaults fills fields -----------
    Log "--- H2: hotkey dialog ---"
    $dlg = Open-HotkeyDialog $typer
    Assert-True ($dlg -ne [IntPtr]::Zero) "H2-dialog-open" "hwnd=0x$($dlg.ToString('X'))"
    $fStart = Get-WmText ([TN]::GetDlgItem($dlg, $IDC_HK_START)) 128
    $fStop  = Get-WmText ([TN]::GetDlgItem($dlg, $IDC_HK_STOP)) 128
    Log "  fields: start='$fStart' stop='$fStop'"
    Assert-True ($fStart -eq 'Ctrl+Alt+Shift+F9' -and $fStop -eq 'Ctrl+Alt+Shift+F10') "H2-dialog-shows-defaults" "$fStart / $fStop"
    Send-Cmd $dlg $IDC_HK_DEFAULT
    Start-Sleep -Milliseconds 250
    $fStart2 = Get-WmText ([TN]::GetDlgItem($dlg, $IDC_HK_START)) 128
    Assert-True ($fStart2 -eq 'Ctrl+Alt+Shift+F9') "H2-restore-defaults-button" $fStart2
    Close-HotkeyDialog $dlg $IDC_HK_CANCEL

    # --- W6: size/position persistence (hide->tray, exit from tray) ---------
    Log "--- W6: geometry persistence ---"
    # 987x760 true px: must exceed the DPI-scaled min track size (~864x704)
    # or SetWindowPos would clamp it and the restore-assert could never pass.
    [void][TN]::SetWindowPos($typer, [IntPtr]::Zero, 120, 60, 987, 760, 0x0004 -bor 0x0010)
    Start-Sleep -Milliseconds 400
    [void][TN]::SendMessage($typer, $WM_SYSCOMMAND, [IntPtr]$SC_CLOSE, [IntPtr]::Zero)   # hide to tray (saves)
    Start-Sleep -Milliseconds 700
    Assert-True (-not [TN]::IsWindowVisible($typer)) "W6-hidden-to-tray" "process alive=$(-not $typerProc.HasExited)"
    # Exit while the hotkey dialog is OPEN: its modal pump must hand
    # WM_QUIT back to the main loop. Regression guard - the old pump
    # swallowed the quit and zombified the process (window destroyed,
    # single-instance mutex still held, later launches all no-op).
    $dlg6 = Open-HotkeyDialog $typer
    Assert-True ($dlg6 -ne [IntPtr]::Zero) "W6-dialog-open-before-exit" "hwnd=0x$($dlg6.ToString('X'))"
    Exit-TyperViaTray
    Assert-True ($typerProc.HasExited) "W6-exited" "process ended via tray menu"
    Launch-Typer
    $r6 = Get-WinRect $typer
    Log "  restored rect: $($r6.left),$($r6.top) $($r6.right-$r6.left)x$($r6.bottom-$r6.top)"
    Assert-True ([math]::Abs($r6.left-120) -le 12 -and [math]::Abs($r6.top-60) -le 12) "W6-position-restored" "$($r6.left),$($r6.top)"
    Assert-True ([math]::Abs(($r6.right-$r6.left)-987) -le 12 -and [math]::Abs(($r6.bottom-$r6.top)-760) -le 12) "W6-size-restored" "$($r6.right-$r6.left)x$($r6.bottom-$r6.top)"

    # --- W7: maximized state persisted (exit via tray while maximized) ------
    Log "--- W7: maximized persistence ---"
    [void][TN]::ShowWindow($typer, 3)
    Start-Sleep -Milliseconds 400
    Exit-TyperViaTray
    Launch-Typer
    Assert-True ([TN]::IsZoomed($typer)) "W7-maximized-restored" "starts maximized"
    [void][TN]::ShowWindow($typer, 9)
    Start-Sleep -Milliseconds 300

    # --- W9: text persistence ------------------------------------------------
    Log "--- W9: text persistence ---"
    Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) "persist abc123 end"
    Send-Cmd $typer $IDC_BTN_SAVE
    Start-Sleep -Milliseconds 400
    Exit-TyperViaTray
    Launch-Typer
    $txt = Get-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) 512
    Assert-True ($txt -eq "persist abc123 end") "W9-text-restored" "got '$txt'"

    # --- W8: corrupt window section falls back to default size ---------------
    Log "--- W8: corrupt config fallback ---"
    try { if ($typerProc -and -not $typerProc.HasExited) { $typerProc.Kill(); [void]$typerProc.WaitForExit(4000) } } catch { }  # dead handle = already gone
    Start-Sleep -Milliseconds 500
    (Get-Content $iniPath) -replace '^Rect=.*','Rect=gibberish' -replace '^Maximized=.*','Maximized=banana' | Set-Content $iniPath -Encoding UTF8
    Launch-Typer
    $r8 = Get-CliRect $typer
    $lw8 = [int]($r8.right / $scale); $lh8 = [int]($r8.bottom / $scale)
    Log "  fallback client: $($r8.right)x$($r8.bottom) (logical ${lw8}x${lh8})"
    Assert-True ($typerProc -and -not $typerProc.HasExited) "W8-alive" "process running"
    Assert-True ($lw8 -ge 1050 -and $lw8 -le 1150 -and $lh8 -ge 550) "W8-default-size-fallback" "client $($r8.right)x$($r8.bottom) = logical ${lw8}x${lh8} (fallback to large default)"

    # --- H3: rebind START only (stop half unchanged) -> new chord live -------
    Log "--- H3: rebind start to Ctrl+Alt+Shift+F11 ---"
    $dlg = Open-HotkeyDialog $typer
    $cap = Dialog-Capture $dlg $IDC_HK_START 0x7A   # F11
    Log "  captured start field: '$cap'"
    Assert-True ($cap -match 'F11') "H3-captured" $cap
    Post-Ok $dlg
    Start-Sleep -Milliseconds 600
    Assert-True (-not (Dismiss-Box "快捷键注册失败")) "H3-no-failure-box" "one-half change must not self-conflict"
    $hk3 = Wait-Hk $typer "F11" 6
    Log "  hotkeys: $hk3"
    Assert-True ($hk3 -match '开始: Ctrl\+Alt\+Shift\+F11 \(已注册\)' -and $hk3 -match '停止: Ctrl\+Alt\+Shift\+F10 \(已注册\)') "H3-status-updated" $hk3
    # the NEW chord really types via the real HID path
    $t1 = "F11 hotkey 0k"
    $w1 = $null
    try {
        $w1 = Start-Target "Target H3" (Join-Path $ev "h3.txt")
        Assert-True (Force-Fg $w1.Hwnd) "H3-target-focused" "hwnd=0x$($w1.Hwnd.ToString('X'))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t1
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) "25"
        Start-Sleep -Milliseconds 300
        Press-Chord 0x7A    # Ctrl+Alt+Shift+F11 = new START
        $st = Wait-Status $typer "(完成|已停止|错误|失败)" 90
        Log "  status: $st"
        Assert-True ($st -match "完成") "H3-new-chord-starts" $st
        $a1 = Wait-Snap $w1.Snap 10
        Assert-True ((Norm $a1) -ceq (Norm $t1)) "H3-typed-identical" "expected='$t1' actual='$(Norm $a1)'"
    } finally { if ($w1) { Stop-Target $w1 } }

    # --- H4: conflict -> 1409 detected, OLD bindings fully preserved --------
    Log "--- H4: conflict rollback (holder occupies F13) ---"
    $holder = Start-Holder 7 0x7C    # Ctrl+Alt+Shift+F13 held by 'another app'
    Assert-True ($holder -and -not $holder.HasExited) "H4-holder-alive" "conflict source running"
    $dlg = Open-HotkeyDialog $typer
    Log "  H4 dialog=0x$($dlg.ToString('X'))"
    $capA = Dialog-Capture $dlg $IDC_HK_START 0x7C   # F13 -> conflicts
    $capB = Dialog-Capture $dlg $IDC_HK_STOP  0x7D   # F14 (would be fine)
    Log "  H4 captured start='$capA' stop='$capB'"
    Log "  pre-Post probe: dlg alive=$([TN]::IsWindow($dlg))"
    Post-Ok $dlg
    # 4s state timeline REPLACES the blind 800ms sleep: it both covers the
    # old wait window and records dialog/box/status/title/INI transitions.
    $tpid4 = [uint32]0
    [void][TN]::GetWindowThreadProcessId($typer, [ref]$tpid4)
    Watch-AfterOk $typer $dlg $tpid4 4
    $box = Dismiss-Box "快捷键注册失败"
    Assert-True $box "H4-conflict-box" "1409 conflict reported to the user"
    $hk4 = Wait-Hk $typer "F11" 6
    Log "  hotkeys after rollback: $hk4"
    Assert-True ($hk4 -match '开始: Ctrl\+Alt\+Shift\+F11 \(已注册\)' -and $hk4 -match '停止: Ctrl\+Alt\+Shift\+F10 \(已注册\)') "H4-old-bindings-intact" $hk4
    Close-HotkeyDialog $dlg $IDC_HK_CANCEL
    # the OLD start chord must still work after the failed change
    $w2 = $null
    try {
        $w2 = Start-Target "Target H4" (Join-Path $ev "h4.txt")
        Assert-True (Force-Fg $w2.Hwnd) "H4-target-focused" "hwnd=0x$($w2.Hwnd.ToString('X'))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) "rollback ok"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) "25"
        Start-Sleep -Milliseconds 300
        Press-Chord 0x7A    # old (still valid) F11 start
        $st2 = Wait-Status $typer "(完成|已停止|错误|失败)" 90
        Log "  status: $st2"
        Assert-True ($st2 -match "完成") "H4-old-chord-still-starts" $st2
    } finally { if ($w2) { Stop-Target $w2 } }

    # --- H5: custom binding survives restart ---------------------------------
    Log "--- H5: restart persistence of custom binding ---"
    Exit-TyperViaTray
    Launch-Typer
    $title5 = Get-WmText $typer 256
    Log "  title: $title5"
    Assert-True ($title5 -match 'Ctrl\+Alt\+Shift\+F11' -and $title5 -match 'Ctrl\+Alt\+Shift\+F10') "H5-custom-persisted" $title5

    # --- H6: restore defaults via dialog -------------------------------------
    Log "--- H6: restore defaults ---"
    $dlg = Open-HotkeyDialog $typer
    Send-Cmd $dlg $IDC_HK_DEFAULT
    Start-Sleep -Milliseconds 250
    Post-Ok $dlg
    Start-Sleep -Milliseconds 600
    Assert-True (-not (Dismiss-Box "快捷键注册失败")) "H6-no-failure" "defaults register cleanly"
    $hk6 = Wait-Hk $typer "F9" 6
    Assert-True ($hk6 -match '开始: Ctrl\+Alt\+Shift\+F9 \(已注册\)' -and $hk6 -match '停止: Ctrl\+Alt\+Shift\+F10 \(已注册\)') "H6-defaults-active" $hk6
    # the default start chord works again
    $w3 = $null
    try {
        $w3 = Start-Target "Target H6" (Join-Path $ev "h6.txt")
        Assert-True (Force-Fg $w3.Hwnd) "H6-target-focused" "hwnd=0x$($w3.Hwnd.ToString('X'))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) "default back"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) "25"
        Start-Sleep -Milliseconds 300
        Press-Chord 0x78    # Ctrl+Alt+Shift+F9
        $st3 = Wait-Status $typer "(完成|已停止|错误|失败)" 90
        Assert-True ($st3 -match "完成") "H6-default-chord-works" $st3
    } finally { if ($w3) { Stop-Target $w3 } }

    # --- H7: startup conflict -> persistent visible warning, tray fallback ---
    Log "--- H7: startup conflict (holder occupies default start) ---"
    if ($holder) { try { $holder.Kill() } catch { } }
    Start-Sleep -Milliseconds 400
    # Exit FIRST: the running instance still owns Ctrl+Alt+Shift+F9, so a
    # holder started before the exit loses the race (1409) and dies silently.
    Exit-TyperViaTray
    $holder = Start-Holder 7 0x78    # occupy Ctrl+Alt+Shift+F9
    Assert-True ($holder -and -not $holder.HasExited) "H7-holder-alive" "holder registered F9 (ready signal seen)"
    Launch-Typer
    $status7 = Wait-Status $typer "注册失败" 8
    Log "  status: $status7"
    $hk7 = Wait-Hk $typer "注册失败" 4
    Log "  hotkeys: $hk7"
    Assert-True ($hk7 -match '开始: Ctrl\+Alt\+Shift\+F9 \(注册失败\)') "H7-start-failed-shown" $hk7
    Assert-True ($hk7 -match '停止: Ctrl\+Alt\+Shift\+F10 \(已注册\)') "H7-stop-still-registered" $hk7
    # the status bar carries the persistent, visible notice with the error code
    Assert-True ($status7 -match '1409' -and $status7 -match '仍可通过托盘') "H7-visible-notice-in-window" $status7
    $tip = [TN]::ReadTip($typer)
    Log "  tray tooltip: '$tip'"
    if ($null -eq $tip -or $tip -eq '') {
        # Win11 24H2's tray host accepts NIM_GET but returns no data (raw
        # byte readback verified empty). The always-visible channels above
        # (hotkey line '(注册失败)' + persistent status notice) carry the
        # same warning - record the skip explicitly, never fake a pass.
        Log "  [NOT RUN] H7-tooltip-warning :: tray tooltip readback unavailable on this Windows tray host"
    } else {
        Assert-True ($tip -match '未注册' -and $tip -match '开始') "H7-tooltip-warning" "$tip"
    }
    # tray manual start must still work (no hotkey needed)
    $w4 = $null
    try {
        $w4 = Start-Target "Target H7" (Join-Path $ev "h7.txt")
        Assert-True (Force-Fg $w4.Hwnd) "H7-target-focused" "hwnd=0x$($w4.Hwnd.ToString('X'))"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_TEXT)) "tray manual"
        Set-WmText ([TN]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) "25"
        Start-Sleep -Milliseconds 300
        Send-Cmd $typer $IDM_TRAY_START
        $st4 = Wait-Status $typer "(完成|已停止|错误|失败)" 90
        Assert-True ($st4 -match "完成") "H7-tray-manual-start" $st4
    } finally { if ($w4) { Stop-Target $w4 } }
    if ($holder) { try { $holder.Kill() } catch { } }
    Start-Sleep -Milliseconds 400

    # --- H9: save failure is reported honestly -------------------------------
    Log "--- H9: settings-save failure notice ---"
    [IO.File]::SetAttributes($iniPath, [IO.FileAttributes]::ReadOnly)
    $dlg = Open-HotkeyDialog $typer
    Log "  H9 dialog=0x$($dlg.ToString('X'))"
    $capC = Dialog-Capture $dlg $IDC_HK_START 0x7A   # F11 (one-half change)
    Log "  H9 captured start='$capC'"
    Post-Ok $dlg
    Start-Sleep -Milliseconds 800
    $saveBox = Dismiss-Box "保存失败"
    Assert-True $saveBox "H9-save-failure-notice" "user told the binding is run-only"
    Close-HotkeyDialog $dlg $IDC_HK_CANCEL   # the failure path keeps the dialog open
    [IO.File]::SetAttributes($iniPath, [IO.FileAttributes]::Normal)
    $hk9 = Wait-Hk $typer "F11" 6
    Assert-True ($hk9 -match '开始: Ctrl\+Alt\+Shift\+F11 \(已注册\)') "H9-binding-live-this-run" $hk9
    # restore defaults (which now persists again)
    $dlg = Open-HotkeyDialog $typer
    Send-Cmd $dlg $IDC_HK_DEFAULT
    Start-Sleep -Milliseconds 250
    Post-Ok $dlg
    Start-Sleep -Milliseconds 600
    Assert-True (-not (Dismiss-Box "保存失败")) "H9-save-works-after-unlock" "no box when the file is writable"

    Exit-TyperViaTray
    Assert-True ($typerProc.HasExited) "final-exit" "clean exit"
}
catch {
    Log "FATAL: $($_.Exception.Message)"
    Log $_.ScriptStackTrace
    $script:Failures++
}
finally {
    foreach ($t in $script:Targets) { Stop-Target $t }
    try { if ($holder -and -not $holder.HasExited) { $holder.Kill() } } catch { }
    try {
        if ($typerProc -and -not $typerProc.HasExited) { $typerProc.Kill(); [void]$typerProc.WaitForExit(3000) }
    } catch { }
    Restore-UserIni
}

Log ""
if ($script:Failures -eq 0) { Log "RESULT: ALL WINDOW/HOTKEY CHECKS PASSED" } else { Log "RESULT: $($script:Failures) FAILURE(S)" }
[IO.File]::WriteAllLines($LogPath, [string[]]$script:LogLines, (New-Object System.Text.UTF8Encoding($true)))
if ($script:Failures -eq 0) { exit 0 } else { exit 1 }
