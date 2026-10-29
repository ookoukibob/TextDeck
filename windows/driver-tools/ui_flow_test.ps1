# ---------------------------------------------------------------------------
# ui_flow_test.ps1
# Drives VirtualHIDTyper.exe via window messages (no typing injection):
#   A) own window in the foreground -> after 3 s countdown the app must refuse
#      to start ("focus is still on this app" guard) and send NO input.
#   B) notepad in the foreground + text set -> countdown runs, then the app
#      fails cleanly with "cannot open virtual HID device (error 2)" because
#      the driver is not installed; notepad must stay empty.
#
# Notes:
#   - Child-control text is read with GetWindowText (SendMessageTimeout
#     WM_GETTEXT returns 0 cross-process on this system - diagnosed 2026-10-16).
#   - SetForegroundWindow is blocked by the foreground lock; we unlock it by
#     a brief ALT key press/release (modifier only, no text input) and verify
#     the result, retrying a few times.
#   - No text is injected anywhere by this test; the app itself cannot inject
#     because the virtual HID driver is not installed.
# Writes logs\ui-flow-test.log. Safe to run without admin rights.
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'VirtualHIDTyper\bin\VirtualHIDTyper.exe'
$log = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'build-logs\ui-flow-test.log'
$script:Lines = @()
function T($m) { $script:Lines += $m; Write-Host $m }

$cs = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Win32 {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder sb);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, int extra);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll", EntryPoint = "GetWindowThreadProcessId")]
    public static extern uint GetWindowThreadProcessId2(IntPtr h, out uint pid);

    public static string GetText(IntPtr h) {
        StringBuilder sb = new StringBuilder(4096);
        GetWindowTextW(h, sb, sb.Capacity);
        return sb.ToString();
    }
    public static string GetClassName(IntPtr h) {
        StringBuilder sb = new StringBuilder(256);
        GetClassNameW(h, sb, 256);
        return sb.ToString();
    }
    public static IntPtr FindChildById(IntPtr parent, int id) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
            if (GetDlgCtrlID(h) == id) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static List<string> AllChildTexts(IntPtr parent) {
        var list = new List<string>();
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr l) {
            string t = GetText(h);
            if (t.Length > 0) list.Add(t);
            return true;
        }, IntPtr.Zero);
        return list;
    }
    // Foreground lock workaround: attach our input queue to the current
    // foreground window's thread, hold ALT (modifier only, no text input),
    // and request foreground. Verified by reading GetForegroundWindow.
    public static bool ForceForeground(IntPtr h) {
        if (IsIconic(h)) ShowWindow(h, 9 /*SW_RESTORE*/);
        uint cur = GetCurrentThreadId();
        uint tgt = GetWindowThreadProcessId(h, IntPtr.Zero);
        uint fgThread = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        bool a1 = false, a2 = false;
        try {
            if (tgt != cur) a1 = AttachThreadInput(cur, tgt, true);
            if (fgThread != cur && fgThread != tgt)
                a2 = AttachThreadInput(cur, fgThread, true);
            for (int i = 0; i < 5; i++) {
                keybd_event(0x12, 0, 0, 0);      // ALT down (keeps menu from activating)
                BringWindowToTop(h);
                SetForegroundWindow(h);
                System.Threading.Thread.Sleep(150);
                keybd_event(0x12, 0, 2, 0);      // ALT up
                System.Threading.Thread.Sleep(100);
                if (GetForegroundWindow() == h) return true;
            }
        } finally {
            try { keybd_event(0x12, 0, 2, 0); } catch {}
            if (a1) AttachThreadInput(cur, tgt, false);
            if (a2) AttachThreadInput(cur, fgThread, false);
        }
        return GetForegroundWindow() == h;
    }
    public static string DescribeWindow(IntPtr h) {
        if (h == IntPtr.Zero) return "<none>";
        uint pid; GetWindowThreadProcessId2(h, out pid);
        string proc = "?";
        try { proc = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
        return h + " cls=" + GetClassName(h) + " title='" + GetText(h) + "' pid=" + pid + "(" + proc + ")";
    }
}
'@
Add-Type -TypeDefinition $cs

function Get-Status([IntPtr]$main) {
    $h = [Win32]::FindChildById($main, 1007)
    if ($h -eq [IntPtr]::Zero) { return '<status control not found>' }
    return ([Win32]::GetText($h) -replace "`r`n", ' | ')
}
function Expect($label, $actual, $needle) {
    if ($actual -like "*$needle*") { T "$label PASS (contains '$needle')" }
    else { T "$label FAIL (wanted '$needle', got '$actual')" }
}

try {
    Remove-Item $log -ErrorAction SilentlyContinue
    # clean up only leftovers of THIS app (never other users' windows)
    Get-Process -Name VirtualHIDTyper -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    # remember notepads that existed before the test; we will only ever
    # stop notepad processes we start ourselves (see $startedNotepads)
    $preexistingNotepads = @((Get-Process -Name notepad -ErrorAction SilentlyContinue).Id)
    $script:startedNotepads = @()
    Start-Sleep -Milliseconds 500

    $proc = Start-Process -FilePath $exe -PassThru
    Start-Sleep -Seconds 2
    $main = $proc.MainWindowHandle
    if ($main -eq [IntPtr]::Zero) { throw "app main window not found" }
    T ("app pid=$($proc.Id) main=$main")

    # give the app a non-empty text so only the focus guard / device error
    # branches can trigger (WM_SETTEXT to the app's own edit box, no injection)
    $edit = [Win32]::FindChildById($main, 1001)
    [Win32]::SendMessage($edit, 0x000C, [IntPtr]::Zero,   # WM_SETTEXT
        (New-Object Text.StringBuilder "ui flow sentinel text")) | Out-Null
    T ("edit text now: '" + [Win32]::GetText($edit) + "'")

    # --- Case A: our own window in front -> focus guard --------------------
    $okA = [Win32]::ForceForeground($main)
    T ("CaseA foreground=" + [Win32]::DescribeWindow([Win32]::GetForegroundWindow()))
    T ("CaseA target=" + [Win32]::DescribeWindow($main) + " forced=$okA")
    $p = [IntPtr]0
    $wp = [IntPtr](1004)
    [Win32]::SendMessage($main, 0x0111, $wp, $null) | Out-Null   # WM_COMMAND / BN_CLICKED
    Start-Sleep -Seconds 1
    T ("CaseA status(after click): " + (Get-Status $main))
    Start-Sleep -Seconds 4
    $stA = Get-Status $main
    T ("CaseA status(after countdown): " + $stA)
    Expect "CaseA focus-guard" $stA "焦点仍在本程序窗口"

    # --- Case B: notepad in front -> clean device error --------------------
    $npProc = Start-Process notepad -PassThru
    $script:startedNotepads += $npProc.Id
    $npHwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 30 -and $npHwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 500
        $npProc.Refresh()
        if ($npProc.HasExited -or $npProc.MainWindowHandle -eq [IntPtr]::Zero) {
            $p2 = Get-Process -Name notepad -ErrorAction SilentlyContinue |
                  Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and
                                 $preexistingNotepads -notcontains $_.Id } |
                  Select-Object -First 1
            if ($p2) {
                if ($script:startedNotepads -notcontains $p2.Id) { $script:startedNotepads += $p2.Id }
                $npProc = $p2
            }
        }
        if ($npProc.MainWindowHandle -ne [IntPtr]::Zero) { $npHwnd = $npProc.MainWindowHandle }
    }
    if ($npHwnd -eq [IntPtr]::Zero) {
        T "CaseB FAIL: notepad window not found"
    } else {
        $okB = [Win32]::ForceForeground($npHwnd)
        T ("CaseB foreground=" + [Win32]::DescribeWindow([Win32]::GetForegroundWindow()))
        T ("CaseB target=" + [Win32]::DescribeWindow($npHwnd) + " forced=$okB")
        [Win32]::SendMessage($main, 0x0111, $wp, $null) | Out-Null
        Start-Sleep -Seconds 5
        $stB = Get-Status $main
        T ("CaseB status(after countdown): " + $stB)
        Expect "CaseB device-error" $stB "错误码 2"
        $prog = [Win32]::GetText([Win32]::FindChildById($main, 1006))
        T ("CaseB progress: " + ($prog -replace "`r`n", ' | '))

        # no injected text anywhere in notepad's window tree
        $texts = [Win32]::AllChildTexts($npHwnd)
        $joined = ($texts -join ' || ')
        if ($joined -like "*sentinel*") { T "Notepad content check FAIL: sentinel found -> $joined" }
        else { T "Notepad content check PASS (no injected text)" }
        Stop-Process -Id $npProc.Id -Force -ErrorAction SilentlyContinue
      }
      # stop only notepad processes this test started
      foreach ($id in ($script:startedNotepads | Select-Object -Unique)) {
        if ($preexistingNotepads -notcontains $id) {
            Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
        }
      }

    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    T "UI FLOW TEST DONE"
    if (($script:Lines -match ' FAIL').Count -gt 0) { exit 1 }
    exit 0
} finally {
    $script:Lines | Out-File $log -Encoding utf8
}
