# ---------------------------------------------------------------------------
# chord_e2e.ps1 -- real application-level verification of keyboard CHORDS.
#
# WHY THIS EXISTS
#   The previous round recorded Ctrl+A as FAIL on the strength of
#   `SelectionLength == 0` after submitting a report. That measurement was not
#   trustworthy: a control experiment showed SendInput's own Ctrl+A also failed
#   to select in the same harness, and SendInput could not even deliver a plain
#   letter there. "The command did not select" and "the harness cannot observe a
#   selection" produced identical output, so the verdict proved nothing.
#
#   This suite therefore separates four distinct claims, and only the last one
#   counts as the chord working:
#     1. the IOCTL returned success
#     2. the driver generated the intended HID report
#     3. the report reached the OS input stack (observed with WH_KEYBOARD_LL)
#     4. the target application performed the command
#   Step 3 is the new evidence layer; without it, a silent submit and a rejected
#   submit look the same.
#
# WHERE IT MUST RUN
#   Session 1 (a logged-on interactive session). vmrun's runProgramInGuest lands
#   in session 0, where the device opens, IOCTLs succeed, and no keystroke
#   reaches any window. It must also run ELEVATED: the INF grants the device
#   object to SY+BA only.
#
# EXIT CODES
#   0 all checks passed
#   1 at least one check failed
#   2 could not open the device (nothing was verified -- not a pass)
#
# The report is written to -LogPath so a scheduled task can collect it.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$LogPath = 'C:\TextDeckTest\chord-e2e.txt',
    [string]$DevicePath = '\\.\TextDeckHidDevice'
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$script:Lines = New-Object System.Collections.Generic.List[string]
$script:Fail = 0
function Emit($m) { $script:Lines.Add([string]$m); Write-Host $m }
function Check($name, $cond, $detail) {
    if ($cond) { Emit ('  [PASS] ' + $name + ' :: ' + $detail) }
    else       { Emit ('  [FAIL] ' + $name + ' :: ' + $detail); $script:Fail++ }
    # Flush per assertion: a watchdog kill must not lose the evidence.
    $script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8
}
function Info($m) { Emit ('  ' + $m) }

# ---- native surface --------------------------------------------------------
$cs = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class TdChord {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string n, uint acc, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] inBuf, uint inSize,
                                       byte[] outBuf, uint outSize, out uint ret, IntPtr ov);
    const uint GRW = 0xC0000000u, SHARE = 3u, OPEN = 3u;
    const uint GET_INFO = 0x00222000u, SUBMIT = 0x00222004u, REL = 0x00222008u, GET_LED = 0x0022200Cu;

    static SafeFileHandle h;
    public static string Open(string path) {
        h = CreateFileW(path, GRW, SHARE, IntPtr.Zero, OPEN, 0, IntPtr.Zero);
        if (h == null || h.IsInvalid) { h = null; return "OPEN_FAIL err=" + Marshal.GetLastWin32Error(); }
        return "OK";
    }
    public static string Close() { if (h != null) { h.Close(); h = null; } return "OK"; }
    public static bool IsOpen() { return h != null && !h.IsInvalid; }

    public static string Info() {
        if (!IsOpen()) return "NO_HANDLE";
        byte[] o = new byte[8]; uint r;
        bool ok = DeviceIoControl(h, GET_INFO, null, 0, o, 8, out r, IntPtr.Zero);
        return "ok=" + ok + " err=" + Marshal.GetLastWin32Error() +
               " maj=" + BitConverter.ToUInt16(o,0) + " min=" + BitConverter.ToUInt16(o,2) +
               " desc=" + o[4] + " maxkeys=" + o[5];
    }
    // action: 0=Down 1=Up 2=ReleaseAll 3=Modifiers
    public static string Submit(byte mods, byte usage, byte action) {
        if (!IsOpen()) return "NO_HANDLE";
        byte[] ev = new byte[4]; ev[0]=mods; ev[1]=usage; ev[2]=action; ev[3]=0;
        uint r;
        bool ok = DeviceIoControl(h, SUBMIT, ev, 4, null, 0, out r, IntPtr.Zero);
        return "ok=" + ok + " err=" + Marshal.GetLastWin32Error();
    }
    public static string ReleaseAll() {
        if (!IsOpen()) return "NO_HANDLE";
        uint r;
        bool ok = DeviceIoControl(h, REL, null, 0, null, 0, out r, IntPtr.Zero);
        return "ok=" + ok + " err=" + Marshal.GetLastWin32Error();
    }
    public static string Leds() {
        if (!IsOpen()) return "NO_HANDLE";
        byte[] o = new byte[4]; uint r;
        bool ok = DeviceIoControl(h, GET_LED, null, 0, o, 4, out r, IntPtr.Zero);
        return "ok=" + ok + " err=" + Marshal.GetLastWin32Error() + " leds=0x" + o[0].ToString("X2");
    }

    // ---- WH_KEYBOARD_LL: evidence that a report reached the input stack ----
    public delegate IntPtr LowLevelProc(int code, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)]
    public struct KBDLLHOOKSTRUCT {
        public uint vkCode; public uint scanCode; public uint flags; public uint time; public IntPtr dwExtraInfo;
    }
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SetWindowsHookExW(int id, LowLevelProc fn, IntPtr hMod, uint tid);
    [DllImport("user32.dll", SetLastError=true)] static extern bool UnhookWindowsHookEx(IntPtr h);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr CallNextHookEx(IntPtr h, int code, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr GetModuleHandleW(string n);

    public static List<string> Events = new List<string>();
    static LowLevelProc proc = HookProc;
    static IntPtr hook = IntPtr.Zero;
    public static string Install() {
        hook = SetWindowsHookExW(13, proc, GetModuleHandleW(null), 0);
        return hook == IntPtr.Zero ? "HOOK_FAIL err=" + Marshal.GetLastWin32Error() : "OK";
    }
    public static void Remove() { if (hook != IntPtr.Zero) { UnhookWindowsHookEx(hook); hook = IntPtr.Zero; } }
    static IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam) {
        if (code >= 0) {
            KBDLLHOOKSTRUCT k = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(KBDLLHOOKSTRUCT));
            int msg = (int)wParam;
            string kind = (msg == 256 || msg == 260) ? "DOWN" : "UP";
            string sys  = (msg == 260 || msg == 261) ? "SYS" : "   ";
            Events.Add(string.Format("{0} {1} vk=0x{2:X2}", kind, sys, k.vkCode));
        }
        return CallNextHookEx(hook, code, wParam, lParam);
    }
}
'@
Add-Type -TypeDefinition $cs -ErrorAction Stop

function Submit([int]$mods, [int]$usage, [int]$action) {
    return [string][TdChord]::Submit([byte]$mods, [byte]$usage, [byte]$action)
}

Emit '==== CHORD E2E (application-level) ===='
Emit ('time    = ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Emit ('session = ' + [System.Diagnostics.Process]::GetCurrentProcess().SessionId)
Emit ('elevated= ' + ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
Emit ''

if ([System.Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0) {
    Emit 'REFUSING to run in session 0: keystrokes cannot reach a window there.'
    Emit 'Run this from an interactive session (scheduled task with /IT).'
    $script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8
    exit 1
}

$openRes = [string][TdChord]::Open($DevicePath)
Emit ('device open = ' + $openRes)
if ($openRes -ne 'OK') {
    Emit 'ABORT: cannot open the device; nothing was verified.'
    $script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8
    exit 2
}
Info ('GET_INFO = ' + [string][TdChord]::Info())
Check 'driver reports protocol 1.1' ((([string][TdChord]::Info()) -match 'maj=1 min=1')) ([string][TdChord]::Info())
Emit ''

$hookRes = [string][TdChord]::Install()
Emit ('input hook = ' + $hookRes)
Check 'a WH_KEYBOARD_LL hook could be installed (evidence layer available)' ($hookRes -eq 'OK') $hookRes
Emit ''

# ---- harness window --------------------------------------------------------
# A RichTextBox, not a TextBox. WinForms TextBox does NOT implement Ctrl+A as
# "select all" -- it receives the keystroke (verified: KeyDown A mods=Control)
# and does nothing with it. Judging the chord against a control that cannot
# perform it makes a correct driver look broken, which is exactly how the
# previous round's Ctrl+A verdict went wrong. RichTextBox implements the standard
# editing commands, so "did the application act?" becomes answerable.
$form = New-Object System.Windows.Forms.Form
$form.Text = 'TextDeck chord E2E'
$form.Width = 760; $form.Height = 340
$form.StartPosition = 'CenterScreen'
$tb = New-Object System.Windows.Forms.RichTextBox
$tb.Font = New-Object System.Drawing.Font('Consolas', 12)
$tb.Dock = 'Fill'
$form.Controls.Add($tb)
$form.Show(); $form.Activate(); $tb.Focus()
[System.Windows.Forms.Application]::DoEvents()
Start-Sleep -Milliseconds 900
[System.Windows.Forms.Application]::DoEvents()
Check 'the harness edit control has keyboard focus' $tb.Focused ('focused=' + $tb.Focused)
Emit ''

function Pump([int]$ms) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 15 }
}
function Reset([string]$text) {
    $tb.Text = $text
    if ($text.Length -gt 0) { $tb.Select(0,0) } else { $tb.Select(0,0) }
    Pump 220
}
function Events() { return ([TdChord]::Events -join ' | ') }
# RichTextBox appends a trailing newline on paste; compare on the trimmed value
# so the assertion is about the content the chord delivered.
function TrimmedText() { return ($tb.Text -replace "`r", '' -replace "`n", '') }

# Every chord is sent the way a real keyboard sends it: a modifier-only report
# to hold the modifier, then the key, then the key release that KEEPS the
# modifier, then a modifier-only report that releases it.
function Chord([int]$mods, [int]$usage) {
    [TdChord]::Events.Clear()
    $a = Submit $mods 0 3          # hold modifier, no key
    Pump 60
    $b = Submit $mods $usage 0     # key down with modifier held
    Pump 70
    $c = Submit $mods $usage 1     # key up, modifier still held
    Pump 60
    $d = Submit 0 0 3              # release modifier
    Pump 240
    return @{ Hold=$a; Down=$b; Up=$c; Release=$d }
}

# ---- 1. Ctrl alone, then Ctrl+A -------------------------------------------
Emit '--- 1. Ctrl held on its own, then A pressed and released ---'
Reset 'zzz'
$r = Chord 0x01 0x04
Info ('hold=' + $r.Hold + '  down=' + $r.Down + '  up=' + $r.Up + '  release=' + $r.Release)
Info ('input stack saw: ' + (Events))
Check 'modifier-only report is accepted (protocol 1.1)' (($r.Hold -match 'ok=True') -and ($r.Release -match 'ok=True')) $r.Hold
Check 'the input stack received Ctrl down, A down, A up, Ctrl up' ((Events) -match 'vk=0xA2[\s\S]*vk=0x41[\s\S]*vk=0x41[\s\S]*vk=0xA2') (Events)
Check 'Ctrl+A selected the whole text' ($tb.SelectionLength -eq 3) ('SelectionLength=' + $tb.SelectionLength + ' text=[' + $tb.Text + ']')
$null = [TdChord]::ReleaseAll(); Pump 120
Emit ''

# ---- 2. Ctrl+C / Ctrl+V round trip ----------------------------------------
Emit '--- 2. Ctrl+A, Ctrl+C, then Ctrl+V into an empty box ---'
Reset 'hello'
$null = Chord 0x01 0x04          # select all
$selLen = $tb.SelectionLength
$null = Chord 0x01 0x06          # copy
Reset ''
$null = Chord 0x01 0x19          # paste (V = 0x19)
$pasted = TrimmedText
Info ('selection before copy = ' + $selLen + '  pasted text = [' + $pasted + ']')
Check 'Ctrl+C copied the selection' ($selLen -eq 5) ('SelectionLength=' + $selLen)
Check 'Ctrl+V pasted the same text' ($pasted -eq 'hello') ('text=[' + $pasted + ']')
$null = [TdChord]::ReleaseAll(); Pump 120
Emit ''

# ---- 3. Ctrl+Shift combination -------------------------------------------
Emit '--- 3. Ctrl+Shift held together, then a key ---'
Reset 'abc'
$mods = 0x01 -bor 0x02           # Ctrl + Left Shift
$r = Chord $mods 0x04
Info ('input stack saw: ' + (Events))
Check 'a two-modifier report is accepted' (($r.Hold -match 'ok=True') -and ($r.Down -match 'ok=True')) $r.Hold
Check 'both modifiers reached the stack (Ctrl then Shift, or Shift then Ctrl)' `
      (((Events) -match 'vk=0xA2') -and ((Events) -match 'vk=0xA0')) (Events)
$null = [TdChord]::ReleaseAll(); Pump 120
Emit ''

# ---- 4. Alt+Tab between two windows --------------------------------------
Emit '--- 4. Alt+Tab switches between two recoverable windows ---'
$form2 = New-Object System.Windows.Forms.Form
$form2.Text = 'TextDeck chord E2E - second window'
$form2.Width = 420; $form2.Height = 200
$form2.StartPosition = 'CenterScreen'
$lbl = New-Object System.Windows.Forms.Label
$lbl.Text = 'second window'
$lbl.Dock = 'Fill'
$form2.Controls.Add($lbl)
$form2.Show()
Pump 400
$form.Activate(); $tb.Focus(); Pump 400
$before = [System.Windows.Forms.Form]::ActiveForm
Info ('active form before = ' + $(if ($before) { $before.Text } else { '<none>' }))
$alt = 0x04                      # Left Alt
[TdChord]::Events.Clear()
$null = Submit $alt 0 3          # Alt down
Pump 80
$null = Submit $alt 0x2B 0       # Tab down (0x2B)
Pump 120
$null = Submit $alt 0x2B 1       # Tab up
Pump 120
$null = Submit 0 0 3             # Alt up
Pump 600
$after = [System.Windows.Forms.Form]::ActiveForm
Info ('active form after  = ' + $(if ($after) { $after.Text } else { '<none>' }))
Info ('input stack saw: ' + (Events))
Check 'Alt+Tab reached the input stack' (((Events) -match 'vk=0xA4') -and ((Events) -match 'vk=0x09')) (Events)
# Assert the specific target, not merely "some other window". `Text -ne Text`
# would also pass if the chord switched to a third window, or to the desktop.
Check 'Alt+Tab activated the second window specifically' ($after -eq $form2) `
      ('before=' + $(if ($before) { $before.Text } else { '-' }) + ' after=' + $(if ($after) { $after.Text } else { '-' }) + ' expected=' + $form2.Text)
if ($after -and $after -ne $form) { $after.Hide() }
$form2.Close()
$form.Activate(); $tb.Focus(); Pump 350
Emit ''

# ---- 5. modifier state after a chord -------------------------------------
Emit '--- 5. no modifier is left latched after a chord ---'
Reset ''
$null = Chord 0x02 0x04          # Shift+A
$afterShift = $tb.Text
Reset ''
$null = Submit 0 0x04 0; Pump 60; $null = Submit 0 0x04 1; Pump 200
Info ('after Shift+A the box held [' + $afterShift + ']; a following plain key produced [' + $tb.Text + ']')
Check 'Shift+A produced an uppercase A' ($afterShift -eq 'A') ('[' + $afterShift + ']')
Check 'a plain key afterwards is lowercase (no modifier latched)' ($tb.Text -eq 'a') ('[' + $tb.Text + ']')
$null = [TdChord]::ReleaseAll(); Pump 120
Emit ''

# ---- 6. regressions that must keep passing -------------------------------
Emit '--- 6. regressions ---'
Reset ''
foreach ($u in @(0x04,0x05,0x06)) {
    $null = Submit 0 $u 0; Pump 45
    $null = Submit 0 $u 1; Pump 45
}
Check 'plain a/b/c still type in order' ($tb.Text -eq 'abc') ('[' + $tb.Text + ']')

Reset ''
$null = Submit 0 0x04 2; Pump 150     # Shift+A
$null = Submit 0x02 0x04 0; Pump 70
$null = Submit 0x02 0x04 1; Pump 70
$null = Submit 0 0 3; Pump 200
Check 'Shift+A still produces uppercase' ($tb.Text -eq 'A') ('[' + $tb.Text + ']')

$ra = [string][TdChord]::ReleaseAll()
Check 'RELEASE_ALL still succeeds' ($ra -match 'ok=True') $ra
$led = [string][TdChord]::Leds()
Check 'GET_LED still readable' ($led -match 'ok=True') $led

$bad = Submit 0 0x03 0
Check 'out-of-range usage still rejected' (-not ($bad -match 'ok=True')) $bad
# Action 0xFF, not 0. The previous version sent Action=0 with usage 0, which is
# rejected because a Down needs a usage -- so it "passed" for a reason unrelated
# to unknown actions. This sends an action number the driver does not define.
$badAction = Submit 0 0x04 0xFF
Check 'unknown action (0xFF) is rejected' (-not ($badAction -match 'ok=True')) $badAction
# And the modifier-only action must reject a non-zero usage, which is the new
# validation protocol 1.1 introduced.
$badModUsage = Submit 0x01 0x04 3
Check 'modifiers-only action rejects a non-zero usage' (-not ($badModUsage -match 'ok=True')) $badModUsage
Emit ''

# ---- teardown -------------------------------------------------------------
$null = [TdChord]::ReleaseAll()
[void][TdChord]::Close()
[TdChord]::Remove()
$form.Close()

Emit ('==== ' + $(if ($script:Fail -eq 0) { 'ALL CHORD CHECKS PASSED' } else { "$($script:Fail) CHECK(S) FAILED" }) + ' ====')
$script:Lines -join "`r`n" | Set-Content -LiteralPath $LogPath -Encoding UTF8
exit $(if ($script:Fail -eq 0) { 0 } else { 1 })
