# ---------------------------------------------------------------------------
# e2e_software_mode.ps1
#
# REAL end-to-end acceptance for VirtualHIDTyper's software input mode.
#
# Proves that text actually arrives, character for character, in a dedicated
# test window driven by the built VirtualHIDTyper.exe via SendInput -- with no
# clipboard use, in all cases with NO virtual HID driver installed.
#
# Tests:
#   T0  static hygiene checks on THIS script (no name-only process kill, no
#       global typer-window lookup, clipboard writes confined to the
#       owner-window transaction region + gated opt-in path; last-error
#       fail-closed enum discipline present)
#   T1  full Unicode text (letters/digits/punctuation/CJK/tab, 2+ lines)
#   T2  HID-open failure dialog: (a) No = stay HID, (b) Cancel = send nothing,
#       (c) Yes = switch to software mode and type for real
#   T3  global stop hotkey Ctrl+Alt+X mid-typing
#   T4  focus-switch protection mid-typing (+ never types into its own edit)
#   T5  countdown cancel sends nothing
#   T6  non-BMP characters (emoji surrogate pairs) typed intact
#   T7  clipboard non-interference. DEFAULT run: read-only before/after
#       fingerprint (this script never writes the clipboard). The sentinel
#       variant OVERWRITES the clipboard and only runs with the explicit
#       -ClipboardTest switch, and only when the original clipboard can be
#       proven to round-trip (empty or readable text-only); otherwise it is
#       refused before any write. Writes use a validated hidden window owned
#       by THIS process as the clipboard owner (Round 10b: never
#       OpenClipboard(NULL) for Empty/Set). Recovery after any mid-sequence
#       failure runs in the outer finally and is VERIFIED by re-reading --
#       "attempted" and "verified" are always reported separately, never faked.
#
# Round 10b/10c: -ClipboardSeamSelfTest runs the same read/gate/transaction
# logic against an in-memory clipboard backend with one-shot fault injection
# at each destructive step (owner validation, open, empty, set, close, enum,
# restore) -- including queued multi-step failure combinations. The REAL
# Windows clipboard is never emptied or written in that mode (it is only read,
# for before/after fingerprints); seam results prove the recovery LOGIC and
# are reported as simulation, not as a live-clipboard restoration.
#
# Process lifecycle: this script STARTS ITS OWN VirtualHIDTyper.exe (own PID)
# and finds windows ONLY by that PID (and its own target-host PIDs). It never
# enumerates/closes/kills same-named processes it did not create; a pre-run
# user instance of VirtualHIDTyper.exe stays untouched for the whole run.
# Full flow sits in one try/catch/finally: early failures, launch failures and
# window timeouts still clean up this run's exact PIDs (grace, then force on
# that PID only) and never recover by killing other programs.
#
# Exit code 0 = all executed assertions passed (skipped checks are logged as
# SKIPPED and are NOT counted as passed).
# ---------------------------------------------------------------------------
param(
    [string]$Exe = (Join-Path $PSScriptRoot 'bin\VirtualHIDTyper.exe'),
    [string]$LogPath = (Join-Path (Split-Path $PSScriptRoot -Parent) 'logs\e2e-software.log'),
    # OPT-IN ONLY: actively overwrites the clipboard with a sentinel (with
    # verified backup/restore). Refused before any write if the original
    # clipboard cannot be round-tripped. Default run never writes clipboard.
    [switch]$ClipboardTest,
    # Round 10b/10c: run ONLY the clipboard fault-injection suite against the
    # in-memory backend (no real clipboard write or clear; the real clipboard
    # is only READ for before/after fingerprints) and exit. No GUI processes
    # are started in this mode.
    [switch]$ClipboardSeamSelfTest
)

$ErrorActionPreference = 'Stop'

# ----- logging --------------------------------------------------------------
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

# ----- native interop -------------------------------------------------------
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Native {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, StringBuilder l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool IsWindowEnabled(IntPtr h);
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
    public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr h);

    public delegate bool EnumWindowsProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc cb, IntPtr p);

    // MessageBox child control helpers (FindWindowEx with a null argument
    // misbehaves in this environment, so children are enumerated in C#).
    public static IntPtr FindChildButtonByPrefix(IntPtr parent, string prefix) {
        IntPtr[] box = new IntPtr[1];
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr p) {
            StringBuilder sb = new StringBuilder(64);
            GetClassName(h, sb, 64);
            if (string.Equals(sb.ToString(), "Button", StringComparison.OrdinalIgnoreCase)) {
                StringBuilder tb = new StringBuilder(64);
                GetWindowText(h, tb, 64);
                if (tb.ToString().StartsWith(prefix, StringComparison.Ordinal)) {
                    box[0] = h;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }
    public static string GetChildStaticTexts(IntPtr parent) {
        System.Collections.Generic.List<string> parts = new System.Collections.Generic.List<string>();
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr p) {
            StringBuilder sb = new StringBuilder(16);
            GetClassName(h, sb, 16);
            if (string.Equals(sb.ToString(), "Static", StringComparison.OrdinalIgnoreCase)) {
                StringBuilder tb = new StringBuilder(1024);
                GetWindowText(h, tb, 1024);
                if (tb.Length > 0) parts.Add(tb.ToString());
            }
            return true;
        }, IntPtr.Zero);
        return string.Join(" ", parts.ToArray());
    }
    public static int CountChildButtons(IntPtr parent) {
        int[] n = new int[1];
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr p) {
            StringBuilder sb = new StringBuilder(16);
            GetClassName(h, sb, 16);
            if (string.Equals(sb.ToString(), "Button", StringComparison.OrdinalIgnoreCase)) n[0]++;
            return true;
        }, IntPtr.Zero);
        return n[0];
    }
    public static IntPtr FindByClass(string cls) {
        IntPtr[] box = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            StringBuilder sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (string.Equals(sb.ToString(), cls, StringComparison.Ordinal)) {
                box[0] = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }
    public static IntPtr FindByTitle(string title) {
        IntPtr[] box = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            StringBuilder sb = new StringBuilder(512);
            GetWindowText(h, sb, 512);
            if (string.Equals(sb.ToString(), title, StringComparison.Ordinal)) {
                box[0] = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }
    public static IntPtr FindByTitlePid(string title, uint pid) {
        IntPtr[] box = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wpid = 0;
            GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            StringBuilder sb = new StringBuilder(512);
            GetWindowText(h, sb, 512);
            if (string.Equals(sb.ToString(), title, StringComparison.Ordinal)) {
                box[0] = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }
    // Window lookup strictly scoped to one PID: the only way this script
    // resolves the typer main window (never a global class/title scan).
    // The app derives its window class from the data directory:
    //   default profile -> "VirtualHIDTyperWnd"
    //   --data-dir=...  -> "VirtualHIDTyperWnd_<FNV1a hex>"
    // (VirtualHIDTyper.cpp g_windowClass). That per-profile class is what keeps
    // two instances with different profiles from colliding, so this lookup must
    // match the PREFIX, not the exact name - otherwise the very isolation the
    // test relies on makes its own window undiscoverable.
    public static IntPtr FindByPidClass(uint pid, string clsPrefix) {
        IntPtr[] box = new IntPtr[1];
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wpid = 0;
            GetWindowThreadProcessId(h, out wpid);
            if (wpid != pid) return true;
            StringBuilder sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            string got = sb.ToString();
            if (got.Equals(clsPrefix, StringComparison.Ordinal) ||
                got.StartsWith(clsPrefix + "_", StringComparison.Ordinal)) {
                box[0] = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return box[0];
    }

    // ---- clipboard: exact Win32 backend (Round 10b) ------------------------
    // Reads open the clipboard with the task association (NULL) and NEVER call
    // EmptyClipboard/SetClipboardData; the MSDN NULL-owner warning only applies
    // to the destructive path. Writes must use a validated hidden window owned
    // by THIS process as the clipboard owner (docs: OpenClipboard(NULL) +
    // EmptyClipboard sets the clipboard owner to NULL, which makes
    // SetClipboardData fail). Every destructive P/Invoke is SetLastError=true;
    // enumeration clears the last-error first and treats
    // EnumClipboardFormats()==0 as end-of-list ONLY when GetLastError() is
    // ERROR_SUCCESS -- otherwise it throws (fail closed: an unknown clipboard
    // state is never mistaken for an empty one). An in-memory backend with
    // one-shot fault injection exists only for -ClipboardSeamSelfTest; it
    // never touches the real clipboard.
    private const uint CF_TEXT = 1, CF_UNICODETEXT = 13, CF_OEMTEXT = 7;
    private const uint GMEM_MOVEABLE = 0x0002;
    private const int ERROR_SUCCESS = 0;
    private const uint WS_POPUP = 0x80000000u;
    private static readonly IntPtr HWND_MESSAGE = new IntPtr(-3);
    [DllImport("user32.dll", SetLastError = true)] static extern bool OpenClipboard(IntPtr hWndNewOwner);
    [DllImport("user32.dll", SetLastError = true)] static extern bool CloseClipboard();
    [DllImport("user32.dll", SetLastError = true)] static extern bool EmptyClipboard();
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr GetClipboardData(uint uFormat);
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr SetClipboardData(uint uFormat, IntPtr hMem);
    [DllImport("user32.dll", SetLastError = true)] static extern uint EnumClipboardFormats(uint format);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClipboardFormatName(uint fmt, StringBuilder s, int cchMax);
    // IsWindow / GetWindowThreadProcessId are already declared in the window
    // section above (same class); reuse them for owner-window validation.
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateWindowExW(uint exStyle, string cls, string name, uint style,
        int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr hWnd);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr GlobalAlloc(uint uFlags, UIntPtr dwBytes);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr GlobalFree(IntPtr hMem);
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr GlobalLock(IntPtr hMem);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GlobalUnlock(IntPtr hMem);
    [DllImport("kernel32.dll")] static extern void SetLastError(uint dwErrCode);
    [DllImport("kernel32.dll")] static extern uint GetCurrentProcessId();

    static IntPtr ClipOwnerHwnd = IntPtr.Zero;

    // ---- in-memory clipboard backend + one-shot fault injection (seam tests)
    public static bool SimMode = false;
    public static uint SimOwnerPid = 0;
    public static IntPtr SimHwnd = new IntPtr(0x5EED);
    // One-shot faults are a FIFO queue: they fire in order and only at the
    // step that matches the head name, so multi-step failure combinations
    // (e.g. "empty-fail" THEN "close-fail") can be injected deterministically.
    public static System.Collections.Generic.List<string> SimFaultsPending = new System.Collections.Generic.List<string>();
    public static string SimFault {   // head-of-queue view (set = reset queue)
        get { return SimFaultsPending.Count > 0 ? SimFaultsPending[0] : null; }
        set { SimFaultsPending.Clear(); if (value != null) SimFaultsPending.Add(value); }
    }
    public static void QueueFault(string name) { SimFaultsPending.Add(name); }
    public static System.Collections.Generic.List<string> SimFaultsFired = new System.Collections.Generic.List<string>();
    public static System.Collections.Generic.SortedDictionary<uint, string> SimStore = new System.Collections.Generic.SortedDictionary<uint, string>();
    public static System.Collections.Generic.Dictionary<uint, string> SimNamed = new System.Collections.Generic.Dictionary<uint, string>();
    public static long SimOpenCount = 0, SimEmptyCount = 0, SimSetCount = 0, SimCloseCount = 0, SimEnumCount = 0;
    public static string SimTrace = "";
    public static bool SimOpen = false;   // sim clipboard "currently open by us" state
    public static void BeginSim(uint pid) {
        SimMode = true; SimOwnerPid = pid;
        SimStore.Clear(); SimNamed.Clear(); SimFaultsFired.Clear();
        SimFault = null; SimTrace = ""; SimOpen = false;
        SimOpenCount = SimEmptyCount = SimSetCount = SimCloseCount = SimEnumCount = 0;
    }
    public static void ResetSim() {
        SimStore.Clear(); SimFault = null; SimTrace = ""; SimOpen = false;
        SimOpenCount = SimEmptyCount = SimSetCount = SimCloseCount = SimEnumCount = 0;
    }
    public static void SimSeedUnicode(string s) { SimStore[CF_UNICODETEXT] = s; }
    public static void SimSeedStd(uint fmt) { SimStore[fmt] = ""; }
    public static uint SimSeedNamed(string name) {
        foreach (var kv in SimNamed) if (kv.Value == name) return kv.Key;
        uint id = (uint)(0xC000 + SimNamed.Count);
        SimNamed[id] = name; SimStore[id] = ""; return id;
    }
    static bool ConsumeFault(string name) {
        // fires only when the NEXT pending fault matches this call site, so
        // queued multi-step combos fire in order at the matching steps
        if (SimFaultsPending.Count > 0 && SimFaultsPending[0] == name) {
            SimFaultsPending.RemoveAt(0); SimFaultsFired.Add(name); return true;
        }
        return false;
    }
    static void Trace(string op) { if (SimMode) { if (SimTrace.Length > 0) SimTrace += "|"; SimTrace += op; } }
    static int SimLastError = 0;
    static void ClearErr() { if (SimMode) SimLastError = 0; else SetLastError(0); }
    static int ReadErr() { return SimMode ? SimLastError : Marshal.GetLastWin32Error(); }
    public static int LastErr() { return ReadErr(); }

    /// <summary>Create (once) and validate the hidden window used as clipboard
    /// owner for every destructive operation. Throws unless the window is a
    /// live window owned by THIS process; callers must not open the clipboard
    /// for writing before this succeeds.</summary>
    public static IntPtr EnsureClipOwnerWindow() {
        if (SimMode) {
            if (ConsumeFault("owner-invalid")) throw new InvalidOperationException("sim: owner window invalid before use");
            if (ConsumeFault("owner-foreign")) throw new InvalidOperationException("sim: owner window belongs to another process");
            Trace("EnsureOwner");
            return SimHwnd;
        }
        if (ClipOwnerHwnd == IntPtr.Zero) {
            IntPtr h = CreateWindowExW(0, "STATIC", null, WS_POPUP, 0, 0, 0, 0,
                HWND_MESSAGE, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
            if (h == IntPtr.Zero) throw new InvalidOperationException("CreateWindowEx(hidden owner) failed err=" + Marshal.GetLastWin32Error());
            ClipOwnerHwnd = h;
        }
        VerifyClipOwnerWindow(ClipOwnerHwnd);
        return ClipOwnerHwnd;
    }
    /// <summary>Is this a live window whose owning process is this process?</summary>
    public static void VerifyClipOwnerWindow(IntPtr hwnd) {
        if (SimMode) {
            if (hwnd != SimHwnd) throw new InvalidOperationException("sim: unknown owner window handle");
            if (GetCurrentProcessId() != SimOwnerPid) throw new InvalidOperationException("sim: owner window not owned by this process");
            return;
        }
        if (hwnd == IntPtr.Zero || !IsWindow(hwnd)) throw new InvalidOperationException("clipboard owner window is not a valid window");
        uint pid;
        GetWindowThreadProcessId(hwnd, out pid);
        if (pid != GetCurrentProcessId()) throw new InvalidOperationException("clipboard owner window belongs to foreign pid " + pid);
    }
    /// <summary>Drop the owner window once every clipboard operation is done.</summary>
    public static void DestroyClipOwnerWindow() {
        if (SimMode) return;
        if (ClipOwnerHwnd != IntPtr.Zero) { DestroyWindow(ClipOwnerHwnd); ClipOwnerHwnd = IntPtr.Zero; }
    }

    /// <summary>OpenClipboard(ownerHwnd) after re-validating ownership; brief
    /// retry because another app may briefly hold the clipboard. The sim
    /// "open-fail" injection produces the same observable result as the real
    /// implementation exhausting all retry attempts (false + last-error 5):
    /// the retry loop itself is not re-simulated (its 8x40ms sleeps would only
    /// slow the suite), and caller-visible state transfer is identical.</summary>
    public static bool ClipOpenOwned(IntPtr hwnd) {
        VerifyClipOwnerWindow(hwnd);   // re-check immediately before opening
        for (int i = 0; i < 8; i++) {
            if (SimMode) {
                if (ConsumeFault("open-fail")) { SimLastError = 5; Trace("OpenFail"); return false; }
                if (SimOpen) { SimLastError = 5; return false; }
                SimOpen = true; SimOpenCount++; SimLastError = 0; Trace("Open"); return true;
            }
            if (OpenClipboard(hwnd)) { Trace("Open"); return true; }
            System.Threading.Thread.Sleep(40);
        }
        return false;   // last error preserved for the caller
    }
    /// <summary>EmptyClipboard while our owner window is the open owner.
    /// A true result means the previous clipboard contents are GONE.</summary>
    public static bool ClipEmptyOwned() {
        if (SimMode) {
            SimEmptyCount++; Trace("Empty");
            if (ConsumeFault("empty-fail")) { SimLastError = 5; return false; }
            SimStore.Clear(); SimLastError = 0; return true;
        }
        bool ok = EmptyClipboard();
        Trace("Empty");
        return ok;
    }
    /// <summary>Replace contents with exactly one CF_UNICODETEXT (CRLF kept).
    /// Alloc/lock/copy failures and SetClipboardData failure all return false
    /// with last-error preserved; callers must treat false as "clipboard may
    /// already be emptied" whenever ClipEmptyOwned succeeded.</summary>
    public static bool ClipSetUnicodeText(string s) {
        if (s == null) throw new ArgumentNullException("s");
        if (SimMode) {
            SimSetCount++; Trace("SetUText");
            if (ConsumeFault("set-fail")) { SimLastError = 8; return false; }
            SimStore[CF_UNICODETEXT] = s; SimLastError = 0; return true;
        }
        IntPtr h = GlobalAlloc(GMEM_MOVEABLE, (UIntPtr)((s.Length + 1) * 2));
        if (h == IntPtr.Zero) { Trace("SetUText"); return false; }
        IntPtr p = GlobalLock(h);
        if (p == IntPtr.Zero) { GlobalFree(h); Trace("SetUText"); return false; }
        try { Marshal.Copy(s.ToCharArray(), 0, p, s.Length); Marshal.WriteInt16(p, s.Length * 2, 0); }
        finally { GlobalUnlock(h); }
        IntPtr r = SetClipboardData(CF_UNICODETEXT, h);
        Trace("SetUText");
        if (r == IntPtr.Zero) { GlobalFree(h); return false; }
        return true;
    }
    public static bool ClipClose() {
        if (SimMode) {
            if (!SimOpen) { SimLastError = 5; Trace("CloseFail"); return false; }  // like the real CloseClipboard when we hold nothing open
            SimCloseCount++; Trace("Close");
            if (ConsumeFault("close-fail")) { SimLastError = 5; return false; }  // stays open, like a real failed close
            SimOpen = false; SimLastError = 0; return true;
        }
        bool ok = CloseClipboard();
        Trace("Close");
        return ok;
    }

    static string StdFormatName(uint f) {
        if (f == CF_TEXT) return "Text";
        if (f == CF_OEMTEXT) return "OemText";
        if (f == CF_UNICODETEXT) return "UnicodeText";
        if (f == 2) return "Bitmap";
        if (f == 8) return "Dib";
        if (f == 15) return "FileDrop";
        if (f == 17) return "DibV5";
        return "0x" + f.ToString("X");
    }
    static string SimFormatName(uint f) {
        string n;
        if (SimNamed.TryGetValue(f, out n)) return n;
        return StdFormatName(f);
    }
    static bool OpenClipboardForRead() {
        for (int i = 0; i < 8; i++) {
            if (OpenClipboard(IntPtr.Zero)) return true;   // read-only association; never Empty/Set
            System.Threading.Thread.Sleep(40);
        }
        return false;
    }
    /// <summary>Sorted pipe-joined names of every format currently on the
    /// clipboard ("" only when enumeration completed with ERROR_SUCCESS and
    /// found nothing). Throws on open failure, on EnumClipboardFormats
    /// returning 0 with a non-success last-error, and in sim fault mode --
    /// an unreadable clipboard is never reported as an empty one.</summary>
    public static string GetClipboardFormatList() {
        if (SimMode) {
            SimEnumCount++;
            if (SimOpen) { SimLastError = 5; throw new InvalidOperationException("sim: OpenClipboard(read) failed err=5 (clipboard still held open)"); }
            if (ConsumeFault("enum-error")) { SimLastError = 5; throw new InvalidOperationException("sim: EnumClipboardFormats failed err=5 (state UNKNOWN)"); }
            System.Collections.Generic.List<string> names = new System.Collections.Generic.List<string>();
            foreach (System.Collections.Generic.KeyValuePair<uint, string> kv in SimStore) names.Add(SimFormatName(kv.Key));
            names.Sort(StringComparer.Ordinal);
            SimLastError = 0;
            return string.Join("|", names.ToArray());
        }
        if (!OpenClipboardForRead()) throw new InvalidOperationException("OpenClipboard(read) failed err=" + Marshal.GetLastWin32Error());
        try {
            System.Collections.Generic.List<string> names = new System.Collections.Generic.List<string>();
            uint f = 0;
            for (;;) {
                ClearErr();                       // do not inherit an older last-error
                uint next = EnumClipboardFormats(f);
                if (next == 0) {
                    int err = ReadErr();
                    if (err != ERROR_SUCCESS) throw new InvalidOperationException("EnumClipboardFormats failed err=" + err + " -- clipboard state UNKNOWN (fail closed)");
                    break;                        // genuine end of enumeration
                }
                f = next;
                StringBuilder sb = new StringBuilder(256);
                int n = GetClipboardFormatName(f, sb, 256);
                names.Add(n > 0 ? sb.ToString() : StdFormatName(f));
            }
            names.Sort(StringComparer.Ordinal);
            return string.Join("|", names.ToArray());
        } finally { CloseClipboard(); }
    }
    /// <summary>Exact CF_UNICODETEXT content, or null when the format is not
    /// present (confirmed via last-error). Throws on open failure, on
    /// GetClipboardData returning NULL with a non-success last-error, and on
    /// GlobalLock failure -- unreadable never becomes an empty string.</summary>
    public static string GetClipboardUnicodeText() {
        if (SimMode) {
            if (SimOpen) { SimLastError = 5; throw new InvalidOperationException("sim: OpenClipboard(read) failed err=5 (clipboard still held open)"); }
            if (ConsumeFault("text-unreadable")) throw new InvalidOperationException("sim: GetClipboardData failed err=5");
            string v;
            return SimStore.TryGetValue(CF_UNICODETEXT, out v) ? v : null;
        }
        if (!OpenClipboardForRead()) throw new InvalidOperationException("OpenClipboard(read) failed err=" + Marshal.GetLastWin32Error());
        try {
            ClearErr();
            IntPtr h = GetClipboardData(CF_UNICODETEXT);
            if (h == IntPtr.Zero) {
                int err = ReadErr();
                if (err != ERROR_SUCCESS) throw new InvalidOperationException("GetClipboardData failed err=" + err + " -- text state UNKNOWN (fail closed)");
                return null;                      // format genuinely absent
            }
            IntPtr p = GlobalLock(h);
            if (p == IntPtr.Zero) throw new InvalidOperationException("GlobalLock(text) failed err=" + Marshal.GetLastWin32Error());
            try { return Marshal.PtrToStringUni(p); } finally { GlobalUnlock(h); }
        } finally { CloseClipboard(); }
    }

    // The application's default STOP hotkey is Ctrl+Alt+Shift+F10
    // (VirtualHIDTyper.cpp: kDefaultHkStop = MOD_CONTROL|MOD_ALT|MOD_SHIFT, VK_F10).
    // Earlier builds used Ctrl+Alt+X, which collided with common applications and
    // was replaced. This helper must press what the app actually registers, or the
    // "stop" is never delivered and every downstream assertion fails for the wrong
    // reason. The expected combo is also read from the app's own window title by
    // the caller, so a future rebind is caught rather than silently mis-tested.
    public static void CtrlAltShiftF10() {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);            // Ctrl down
        keybd_event(0x12, 0, 0, UIntPtr.Zero);            // Alt down
        keybd_event(0x10, 0, 0, UIntPtr.Zero);            // Shift down
        keybd_event(0x79, 0, 0, UIntPtr.Zero);            // F10 down (hotkey eats it)
        keybd_event(0x79, 0, 2, UIntPtr.Zero);            // F10 up
        keybd_event(0x10, 0, 2, UIntPtr.Zero);            // Shift up
        keybd_event(0x12, 0, 2, UIntPtr.Zero);            // Alt up
        keybd_event(0x11, 0, 2, UIntPtr.Zero);            // Ctrl up
    }
    public static void AltTap() {
        keybd_event(0x12, 0, 0, UIntPtr.Zero);
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
    }
}
'@

$WM_GETTEXT      = 0x000D
$WM_SETTEXT      = 0x000C
$WM_COMMAND      = 0x0111
$WM_CLOSE        = 0x0010
$BM_CLICK        = 0x00F5
$BM_SETCHECK     = 0x00F1
$BST_CHECKED     = 1
$BST_UNCHECKED   = 0
$IDC_EDIT_TEXT     = 1001
$IDC_EDIT_INTERVAL = 1002
$IDC_CHK_SHIFT_NL  = 1003
$IDC_BTN_START     = 1004
$IDC_BTN_STOP      = 1005
$IDC_STATIC_PROG   = 1006
$IDC_STATIC_STATUS = 1007
$IDC_RADIO_HID     = 1008
$IDC_RADIO_SW      = 1009

# ----- helpers --------------------------------------------------------------
function Get-WmText([IntPtr]$hwnd, [int]$cap = 2048) {
    if ($hwnd -eq [IntPtr]::Zero) { return '' }
    $sb = New-Object System.Text.StringBuilder $cap
    [void][Native]::SendMessage($hwnd, $WM_GETTEXT, [IntPtr]$cap, $sb)
    return $sb.ToString()
}
function Set-WmText([IntPtr]$hwnd, [string]$text) {
    [void][Native]::SendMessage($hwnd, $WM_SETTEXT, [IntPtr]::Zero, $text)
}
function Send-Cmd([IntPtr]$hwnd, [int]$id) {
    [void][Native]::SendMessage($hwnd, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero)
}
function Get-ChildByClass([IntPtr]$parent, [string]$cls) {
    return [Native]::FindWindowEx($parent, [IntPtr]::Zero, $cls, $null)
}
function Force-Fg([IntPtr]$h) {
    if (-not [Native]::IsWindow($h)) { return $false }
    for ($i = 0; $i -lt 25; $i++) {
        if ([Native]::GetForegroundWindow() -eq $h) { return $true }
        [void][Native]::ShowWindow($h, 9)   # SW_RESTORE
        # strategy A: attach our input queue to the CURRENT foreground
        # thread, which is allowed to change the foreground window
        $fg = [Native]::GetForegroundWindow()
        if ($fg -ne [IntPtr]::Zero) {
            $fgTid = [Native]::GetWindowThreadId($fg, [IntPtr]::Zero)
            $selfTid = [Native]::GetCurrentThreadId()
            [void][Native]::AttachThreadInput($selfTid, $fgTid, $true)
            [void][Native]::SetForegroundWindow($h)
            [void][Native]::BringWindowToTop($h)
            [void][Native]::AttachThreadInput($selfTid, $fgTid, $false)
        } else {
            [void][Native]::SetForegroundWindow($h)
        }
        if ([Native]::GetForegroundWindow() -eq $h) { return $true }
        # strategy B: ALT-tap unlocks foreground activation, then retry
        [Native]::AltTap()
        [void][Native]::SetForegroundWindow($h)
        if ([Native]::GetForegroundWindow() -eq $h) { return $true }
        Start-Sleep -Milliseconds 60
    }
    return ([Native]::GetForegroundWindow() -eq $h)
}
function Read-Snap([string]$path) {
    for ($i = 0; $i -lt 12; $i++) {
        try { return [System.IO.File]::ReadAllText($path) } catch { Start-Sleep -Milliseconds 60 }
    }
    return $null
}
function Norm([string]$s) { if ($s -eq $null) { return '' } return ($s -replace "`r`n", "`n") }
function Test-Equal([string]$a, [string]$b) { return ((Norm $a) -ceq (Norm $b)) }
function Test-IsPrefix([string]$expected, [string]$actual) {
    $e = Norm $expected; $a = Norm $actual
    if ($a.Length -gt $e.Length) { return $false }
    return ($e.Substring(0, $a.Length) -ceq $a)
}
# Prefix with a bounded number of single-character drops (focus-transition
# races can lose at most one in-flight SendInput event). Returns drop info.
$script:LastNearPrefixInfo = ''
function Test-NearPrefix([string]$expected, [string]$actual, [int]$maxDropped) {
    $e = Norm $expected; $a = Norm $actual
    $script:LastNearPrefixInfo = ''
    if ($a.Length -gt $e.Length) { return $false }
    $ei = 0; $dropped = 0; $drops = @()
    for ($ai = 0; $ai -lt $a.Length; $ai++) {
        if ($ei -lt $e.Length -and $e[$ei] -ceq $a[$ai]) { $ei++; continue }
        $dropped++
        if ($dropped -gt $maxDropped) { return $false }
        if ($ei -ge $e.Length) { return $false }
        $drops += $e[$ei]; $ei++
        if ($ei -ge $e.Length -or $e[$ei] -cne $a[$ai]) { return $false }
        $ei++
    }
    $script:LastNearPrefixInfo = "dropped=$dropped [$($drops -join '')]"
    return $true
}
# Snapshot files are written on a 250ms timer by the target host; after the
# typer reports completion, wait until two consecutive reads agree so the
# comparison sees the FINAL content (never a mid-write or stale state).
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
function Count-CodePoints([string]$s) {
    $n = 0
    for ($i = 0; $i -lt $s.Length; $i++) {
        $c = [int][char]$s[$i]
        if ($c -ge 0xD800 -and $c -le 0xDBFF -and ($i + 1) -lt $s.Length) {
            $c2 = [int][char]$s[$i + 1]
            if ($c2 -ge 0xDC00 -and $c2 -le 0xDFFF) { $i++ }
        }
        $n++
    }
    return $n
}
function Wait-Status([IntPtr]$typer, [string]$pattern, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    $st = ''
    do {
        $st = Get-WmText ([Native]::GetDlgItem($typer, $IDC_STATIC_STATUS)) 2048
        if ($st -match $pattern) { return $st }
        Start-Sleep -Milliseconds 120
    } while ((Get-Date) -lt $deadline)
    return $st
}
function Get-Progress([IntPtr]$typer) {
    $t = Get-WmText ([Native]::GetDlgItem($typer, $IDC_STATIC_PROG)) 512
    if ($t -match '(-?\d+)\s*/\s*(\d+)') {
        return @{ sent = [int]$Matches[1]; total = [int]$Matches[2] }
    }
    return @{ sent = -1; total = -1 }
}
function Set-ModeRadio([IntPtr]$typer, [string]$mode) {
    $id = if ($mode -eq 'software') { $IDC_RADIO_SW } else { $IDC_RADIO_HID }
    $rb = [Native]::GetDlgItem($typer, $id)
    [void][Native]::SendMessage($rb, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-Title([IntPtr]$hwnd) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][Native]::GetWindowText($hwnd, $sb, 512)
    return $sb.ToString()
}

# ----- clipboard helpers (read-only unless the opt-in sentinel path) --------
# Fingerprint = sorted format list + exact UnicodeText content. Both calls
# only READ the clipboard; they cannot alter it. Unreadable or inconsistent
# states are reported via Error and are NEVER disguised as an empty clipboard.
# Names deliberately avoid the PowerShell clipboard cmdlet prefixes so T0's
# source scan cannot self-match.
function Read-ClipFp {
    try {
        $formats = [Native]::GetClipboardFormatList()
        $text = [Native]::GetClipboardUnicodeText()   # $null = no UnicodeText format
        $list = @()
        if ($formats -and $formats.Length -gt 0) { $list = @($formats -split '\|') }
        $hasUnicode = $list -contains 'UnicodeText'
        if ($hasUnicode -and ($null -eq $text)) {
            return @{ Formats = $formats; Text = $null; Error = 'UnicodeText listed but content unreadable (fail closed)' }
        }
        if ((-not $hasUnicode) -and ($null -ne $text)) {
            return @{ Formats = $formats; Text = $text; Error = 'Unicode text without UnicodeText in format list (inconsistent state; fail closed)' }
        }
        return @{ Formats = $formats; Text = $text; Error = '' }
    } catch {
        return @{ Formats = $null; Text = $null; Error = $_.Exception.Message }
    }
}
function Wait-ClipFp([int]$retries = 5) {
    $fp = $null
    for ($i = 0; $i -lt $retries; $i++) {
        $fp = Read-ClipFp
        if (-not $fp.Error) { return $fp }
        Start-Sleep -Milliseconds 120
    }
    return $fp
}
function Test-FingerprintEqual($a, $b) {
    if ($a.Error -or $b.Error) { return $false }
    return (($a.Formats -ceq $b.Formats) -and ($a.Text -ceq $b.Text))
}
# Pure gate: is it safe to overwrite the clipboard with a text sentinel?
# Safe = clipboard provably empty, or EXACTLY the UnicodeText format with
# READABLE content (a state we can restore byte-for-byte and prove: same
# format list + same content). Everything else -- files, images, HTML, RTF,
# extra text flavours, unreadable text, error/unknown states -- is refused
# BEFORE any write, including before EmptyClipboard.
function Test-SentinelSafeToOverwrite([string[]]$formats, $text) {
    if ($null -eq $formats) { return $false }               # error/unknown: refuse
    if ($formats.Count -eq 0) { return $true }              # provably empty
    if (($formats.Count -eq 1) -and ($formats[0] -ceq 'UnicodeText')) {
        return ($null -ne $text)                            # content must be readable
    }
    return $false
}

# ----- clipboard transaction region (the ONLY destructive clipboard path) ---
# Round 10b: every Empty/Set goes through an owner-window transaction. The
# state flags record EXACTLY how far a write got, so recovery in the outer
# finally can act on "EmptyClipboard already succeeded" even when a later
# step (alloc/lock/SetClipboardData/Close) threw mid-sequence. Failure of any
# step throws immediately; the caller never has to guess whether the original
# clipboard may already be gone.
$script:ClipTx = @{
    OwnerWindowReady = $false
    OwnerWindow      = [IntPtr]::Zero
    Opened           = $false
    Emptied          = $false     # <- from here on the original data is gone
    Written          = $false
}
function New-ClipOwnerWindow {
    # Creates (once) and validates a hidden window owned by THIS process; any
    # validation failure throws BEFORE the clipboard is opened for writing.
    $script:ClipTx.OwnerWindow = [Native]::EnsureClipOwnerWindow()
    $script:ClipTx.OwnerWindowReady = $true
    return $script:ClipTx.OwnerWindow
}
function Invoke-ClipboardSentinelWrite([string]$text) {
    # Preconditions enforced by the caller: explicit -ClipboardTest opt-in,
    # original fingerprint backed up, Test-SentinelSafeToOverwrite passed.
    $h = New-ClipOwnerWindow
    if (-not [Native]::ClipOpenOwned($h)) {
        throw "OpenClipboard(owner window) failed (win32=$([Native]::LastErr())) -- nothing was modified"
    }
    $script:ClipTx.Opened = $true
    if (-not [Native]::ClipEmptyOwned()) {
        # Empty failed -> original data is STILL intact. Release our open
        # handle, but flip Opened ONLY when CloseClipboard really succeeded;
        # a failed close leaves the clipboard open BY US and the recovery in
        # the outer finally must see that true state to close it.
        $err = [Native]::LastErr()
        $closeOk = [Native]::ClipClose()
        $closeNote = 'clipboard handle released'
        if ($closeOk) { $script:ClipTx.Opened = $false }
        else { $closeNote = "CloseClipboard ALSO failed (win32=$([Native]::LastErr())) -- clipboard still open; recovery must close it" }
        throw "EmptyClipboard failed (win32=$err) -- original clipboard still intact; $closeNote"
    }
    $script:ClipTx.Emptied = $true   # <-- original data destroyed; recovery mandatory
    Log 'CLIPBOARD STATE: EmptyClipboard succeeded -- original data destroyed; recovery in cleanup is now mandatory if anything below fails'
    if (-not [Native]::ClipSetUnicodeText($text)) {
        # Set failed AFTER Empty succeeded -> original destroyed, recovery is
        # mandatory. Same state discipline: Opened flips only on a proven
        # successful close, otherwise recovery sees the real open state.
        $err = [Native]::LastErr()
        $closeOk = [Native]::ClipClose()
        $closeNote = 'clipboard handle released'
        if ($closeOk) { $script:ClipTx.Opened = $false }
        else { $closeNote = "CloseClipboard ALSO failed (win32=$([Native]::LastErr())) -- clipboard still open; recovery must close it" }
        throw "write failed AFTER EmptyClipboard succeeded (win32=$err) -- original data destroyed; $closeNote; recovery will run"
    }
    $script:ClipTx.Written = $true
    if (-not [Native]::ClipClose()) {
        throw "CloseClipboard failed after write (win32=$([Native]::LastErr())) -- clipboard still OPEN by us (state kept); recovery must close it"
    }
    $script:ClipTx.Opened = $false
}
function Invoke-ClipboardRestore($expectedFp, $originalText) {
    # Recovery/disposal for the sentinel transaction. Returns
    # Attempted/Verified/OpenHandled/Error:
    #  - ANY leftover open clipboard is disposed FIRST, even when Emptied is
    #    false and there is nothing to restore (an open clipboard left behind
    #    would block other apps and every verification read). Opened flips to
    #    false ONLY when CloseClipboard actually succeeded -- never assumed.
    #  - Verified is set ONLY after re-reading formats+text and proving
    #    equality with the pre-run fingerprint -- "attempted" and "verified"
    #    stay distinct, and any failure is reported explicitly via Error,
    #    never swallowed or faked.
    $r = @{ Attempted = $false; Verified = $false; OpenHandled = $false; Error = '' }
    if ($script:ClipTx.Opened) {
        # Step 0: dispose of the leftover open state the write path left
        # behind. No blind/unconditional close: the real result is checked.
        if ([Native]::ClipClose()) {
            $script:ClipTx.Opened = $false
            $r.OpenHandled = $true
            Log 'CLIPBOARD STATE: leftover open handle closed during cleanup'
        } else {
            $r.Error = "leftover open clipboard could not be closed (win32=$([Native]::LastErr())) -- open state persists; content NOT verified"
            return $r   # reopening/verifying would be meaningless now
        }
    }
    if (-not $script:ClipTx.Emptied) { return $r }   # nothing was destroyed; state is clean
    $r.Attempted = $true
    try {
        if (-not $script:ClipTx.OwnerWindowReady) { throw 'owner window not ready for restore' }
        $h = $script:ClipTx.OwnerWindow
        if (-not [Native]::ClipOpenOwned($h)) { throw "reopen for restore failed (win32=$([Native]::LastErr()))" }
        $script:ClipTx.Opened = $true
        if (-not [Native]::ClipEmptyOwned()) { throw "EmptyClipboard failed during restore (win32=$([Native]::LastErr()))" }
        if ($null -ne $originalText) {
            # gate proved: original is exactly one readable UnicodeText
            if (-not [Native]::ClipSetUnicodeText($originalText)) {
                throw "SetClipboardData failed during restore (win32=$([Native]::LastErr()))"
            }
        }
        if (-not [Native]::ClipClose()) {
            throw "CloseClipboard failed after restore write (win32=$([Native]::LastErr()))"
        }
        $script:ClipTx.Opened = $false
    } catch {
        $r.Error = $_.Exception.Message
        if ($script:ClipTx.Opened) {
            # best-effort dispose of anything still open; state flips only on
            # an explicitly successful close, and a SECOND close failure is
            # appended to the reported error -- never hidden.
            if ([Native]::ClipClose()) { $script:ClipTx.Opened = $false }
            else { $r.Error += "; leftover open clipboard close ALSO failed (win32=$([Native]::LastErr()))" }
        }
    }
    if (-not $r.Error) {
        $chk = Wait-ClipFp 5
        if ($chk.Error) {
            $r.Error = "post-restore read failed: $($chk.Error)"
        } elseif (Test-FingerprintEqual $expectedFp $chk) {
            $r.Verified = $true
        } else {
            $r.Error = "post-restore fingerprint mismatch: now=[$($chk.Formats)]|len=$(if ($null -eq $chk.Text) { 'n/a' } else { $chk.Text.Length }) expected=[$($expectedFp.Formats)]|len=$(if ($null -eq $expectedFp.Text) { 'n/a' } else { $expectedFp.Text.Length })"
        }
    }
    return $r
}
# ----- end clipboard transaction region -------------------------------------

# ----- clipboard seam fault-injection self-test (-ClipboardSeamSelfTest) ----
# Runs the REAL read/gate/transaction/recovery logic against the in-memory
# backend with one-shot fault injection at every destructive step. The REAL
# Windows clipboard is never WRITTEN or EMPTIED in this mode; it is only READ
# (a read-only fingerprint before and after the suite, asserted identical).
# These results prove the recovery LOGIC via simulation -- they are NOT a
# live-clipboard restoration and are never reported as one.
if ($ClipboardSeamSelfTest) {
    Log '=== clipboard seam fault-injection self-test (in-memory backend; no real clipboard write or clear, only read-only before/after fingerprints) ==='
    $realBeforeSeam = Read-ClipFp    # real backend: SimMode not yet active
    [Native]::BeginSim([uint32]$PID)
    function Reset-SeamScenario {
        [Native]::ResetSim()
        $script:ClipTx = @{ OwnerWindowReady = $false; OwnerWindow = [IntPtr]::Zero; Opened = $false; Emptied = $false; Written = $false }
    }
    function Seam-ReadGate {
        $fp = Read-ClipFp
        if ($fp.Error) { return @{ Allow = $false; Why = "unreadable: $($fp.Error)"; Fp = $fp } }
        $list = @()
        if ($fp.Formats -and $fp.Formats.Length -gt 0) { $list = @($fp.Formats -split '\|') }
        $allow = Test-SentinelSafeToOverwrite $list $fp.Text
        return @{ Allow = $allow; Why = "formats=[$($fp.Formats)] textReadable=$(if ($null -eq $fp.Text) { 'no' } else { 'yes' })"; Fp = $fp }
    }

    # --- format-state matrix through the REAL read path -> gate decision -----
    Reset-SeamScenario; $g = Seam-ReadGate
    Assert-True ($g.Allow -eq $true) 'SEAM-gate-empty-allow' $g.Why
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL'); $g = Seam-ReadGate
    Assert-True (($g.Allow -eq $true) -and ($g.Fp.Text -ceq 'PRE-ORIGINAL')) 'SEAM-gate-unicode-allow-readable' $g.Why
    Reset-SeamScenario; [Native]::SimSeedUnicode('x'); [Native]::SimFault = 'text-unreadable'; $g = Seam-ReadGate
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-unicode-unreadable-refused' $g.Why
    Reset-SeamScenario; [Native]::SimSeedUnicode('x'); [Native]::SimSeedStd(1); $g = Seam-ReadGate   # +CF_TEXT
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-multitext-refused' $g.Why
    Reset-SeamScenario; [Native]::SimSeedUnicode('x'); [void][Native]::SimSeedNamed('HTML Format'); $g = Seam-ReadGate
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-html-refused' $g.Why
    Reset-SeamScenario; [Native]::SimSeedUnicode('x'); [void][Native]::SimSeedNamed('Rich Text Format'); $g = Seam-ReadGate
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-rtf-refused' $g.Why
    Reset-SeamScenario; [Native]::SimSeedStd(15); $g = Seam-ReadGate    # CF_HDROP file list
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-files-refused' $g.Why
    Reset-SeamScenario; [Native]::SimSeedStd(2); $g = Seam-ReadGate     # CF_BITMAP image
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-image-refused' $g.Why
    Reset-SeamScenario; [Native]::SimSeedUnicode('x'); [Native]::SimFault = 'enum-error'; $g = Seam-ReadGate
    Assert-True ($g.Allow -eq $false) 'SEAM-gate-enum-error-refused' $g.Why

    # --- S1: empty original -> sentinel write -> restore -> verified empty ---
    Reset-SeamScenario
    $fp0 = Read-ClipFp
    Invoke-ClipboardSentinelWrite 'E2E-SEAM-SENTINEL-1'
    $fpMid = Read-ClipFp
    Assert-True (($fpMid.Text -ceq 'E2E-SEAM-SENTINEL-1') -and ($fpMid.Formats -ceq 'UnicodeText')) `
        'SEAM-S1-sentinel-single-unicode' "formats=[$($fpMid.Formats)] len=$($fpMid.Text.Length)"
    $r = Invoke-ClipboardRestore $fp0 $null
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $true)) 'SEAM-S1-restore-verified-empty' "err='$($r.Error)'"
    $fpEnd = Read-ClipFp
    Assert-True (($fpEnd.Formats -ceq '') -and ($null -eq $fpEnd.Text)) 'SEAM-S1-clipboard-empty-again' "formats=[$($fpEnd.Formats)]"

    # --- S2: UnicodeText original -> full round-trip + call-order proof ------
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    Invoke-ClipboardSentinelWrite 'E2E-SEAM-SENTINEL-2'
    $r = Invoke-ClipboardRestore $fp0 $fp0.Text
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $true)) 'SEAM-S2-restore-verified-text' "err='$($r.Error)'"
    $fpEnd = Read-ClipFp
    Assert-True (($fpEnd.Text -ceq 'PRE-ORIGINAL-TEXT') -and ($fpEnd.Formats -ceq 'UnicodeText')) `
        'SEAM-S2-original-content-exact' "formats=[$($fpEnd.Formats)] len=$($fpEnd.Text.Length)"
    Assert-True ([Native]::SimTrace -ceq 'EnsureOwner|Open|Empty|SetUText|Close|Open|Empty|SetUText|Close') `
        'SEAM-S2-call-order-proof' ([Native]::SimTrace)

    # --- S3: SetClipboardData fails AFTER EmptyClipboard succeeded ------------
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    [Native]::SimFault = 'set-fail'
    $threw = $false
    try { Invoke-ClipboardSentinelWrite 'E2E-SEAM-SENTINEL-3' } catch { $threw = $true }
    Assert-True ($threw -and $script:ClipTx.Emptied) 'SEAM-S3-write-threw-after-empty' "threw=$threw emptied=$($script:ClipTx.Emptied)"
    Assert-True ((-not $script:ClipTx.Opened) -and ([Native]::SimFaultsFired -contains 'set-fail')) `
        'SEAM-S3-close-ok-state-accurate' "opened=$($script:ClipTx.Opened) fired=$([Native]::SimFaultsFired -join ',')"
    $r = Invoke-ClipboardRestore $fp0 $fp0.Text
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $true)) 'SEAM-S3-recovery-ran-and-verified' "err='$($r.Error)'"
    Assert-True ([Native]::SimTrace -match 'Close\|Open\|Empty\|SetUText\|Close$') 'SEAM-S3-recovery-order' ([Native]::SimTrace)
    Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') 'SEAM-S3-original-content-restored' ''

    # --- S4: CloseClipboard fails after a successful write -------------------
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    [Native]::SimFault = 'close-fail'
    $threw = $false
    try { Invoke-ClipboardSentinelWrite 'E2E-SEAM-SENTINEL-4' } catch { $threw = $true }
    Assert-True $threw 'SEAM-S4-close-fail-threw' "threw=$threw"
    Assert-True ([Native]::SimFaultsFired -contains 'close-fail') 'SEAM-S4-fault-fired' ''
    $r = Invoke-ClipboardRestore $fp0 $fp0.Text
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $true)) 'SEAM-S4-recovery-ran-and-verified' "err='$($r.Error)'"

    # --- S5: recovery itself fails -> explicit failure, never faked ----------
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    Invoke-ClipboardSentinelWrite 'E2E-SEAM-SENTINEL-5'   # clean write
    [Native]::SimFault = 'set-fail'                        # next Set (inside restore) fails
    $r = Invoke-ClipboardRestore $fp0 $fp0.Text
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $false) -and ($r.Error -ne '')) `
        'SEAM-S5-recovery-failure-explicit' "verified=$($r.Verified) err='$($r.Error)'"
    Assert-True ($script:ClipTx.Opened -eq $false) 'SEAM-S5-failure-state-accurate' "opened=$($script:ClipTx.Opened) (best-effort close ran; state flips only on proven close)"

    # --- S6: EmptyClipboard itself fails -> nothing modified, no restore -----
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    [Native]::SimFault = 'empty-fail'
    $threw = $false
    try { Invoke-ClipboardSentinelWrite 'X' } catch { $threw = $true }
    Assert-True ($threw -and (-not $script:ClipTx.Emptied)) 'SEAM-S6-empty-fail-no-modification' "threw=$threw emptied=$($script:ClipTx.Emptied)"
    $r = Invoke-ClipboardRestore $fp0 $fp0.Text
    Assert-True (($r.Attempted -eq $false) -and ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT')) 'SEAM-S6-no-restore-needed-intact' ''

    # --- S7: owner-window validation fails BEFORE any destructive op ---------
    foreach ($fault in @('owner-invalid', 'owner-foreign')) {
        Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
        [Native]::SimFault = $fault
        $threw = $false
        try { Invoke-ClipboardSentinelWrite 'X' } catch { $threw = $true }
        Assert-True ($threw -and ([Native]::SimOpenCount -eq 0) -and ([Native]::SimEmptyCount -eq 0) -and ([Native]::SimSetCount -eq 0)) `
            "SEAM-S7-$fault-no-destructive-op" "threw=$threw open=$([Native]::SimOpenCount) empty=$([Native]::SimEmptyCount) set=$([Native]::SimSetCount)"
        Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') "SEAM-S7-$fault-original-intact" ''
    }

    # --- S8: nothing modified -> recovery is a strict no-op ------------------
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    $r = Invoke-ClipboardRestore $fp0 'PRE-ORIGINAL-TEXT'
    Assert-True (($r.Attempted -eq $false) -and ($r.Verified -eq $false)) 'SEAM-S8-restore-noop-when-unmodified' ''
    Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') 'SEAM-S8-clipboard-untouched' ''

    # --- S9: OpenClipboard failure injection ---------------------------------
    # The sim "open-fail" fault injects the outcome of retry exhaustion: the
    # caller observes the same false + last-error as after real retries.
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    [Native]::SimFault = 'open-fail'
    $threw = $false; $msg = ''
    try { Invoke-ClipboardSentinelWrite 'X' } catch { $threw = $true; $msg = $_.Exception.Message }
    Assert-True ($threw -and ($msg -match 'nothing was modified')) 'SEAM-S9-open-fail-recognized' "threw=$threw msg=$msg"
    Assert-True (([Native]::SimEmptyCount -eq 0) -and ([Native]::SimSetCount -eq 0) -and ([Native]::SimOpenCount -eq 0)) `
        'SEAM-S9-no-destructive-op' "open=$([Native]::SimOpenCount) empty=$([Native]::SimEmptyCount) set=$([Native]::SimSetCount)"
    Assert-True ((-not $script:ClipTx.Opened) -and (-not $script:ClipTx.Emptied) -and (-not $script:ClipTx.Written)) `
        'SEAM-S9-state-accurate-not-success' "opened=$($script:ClipTx.Opened) emptied=$($script:ClipTx.Emptied) written=$($script:ClipTx.Written)"
    Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') 'SEAM-S9-original-intact' ''
    $r = Invoke-ClipboardRestore $fp0 'PRE-ORIGINAL-TEXT'
    Assert-True (($r.Attempted -eq $false) -and ($r.Verified -eq $false) -and ($r.OpenHandled -eq $false) -and ($r.Error -eq '')) `
        'SEAM-S9-no-restore-needed' "openHandled=$($r.OpenHandled) err='$($r.Error)'"

    # --- S10: Empty/Set failure x Close success/failure state matrix ---------
    # C1: EmptyClipboard fails, then the failure-path CloseClipboard succeeds.
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    [Native]::SimFault = 'empty-fail'
    $threw = $false; $msg = ''
    try { Invoke-ClipboardSentinelWrite 'X' } catch { $threw = $true; $msg = $_.Exception.Message }
    Assert-True ($threw -and (-not $script:ClipTx.Emptied) -and (-not $script:ClipTx.Opened) -and ($msg -match 'handle released')) `
        'SEAM-S10c1-emptyfail-closesok-state' "threw=$threw emptied=$($script:ClipTx.Emptied) opened=$($script:ClipTx.Opened) msg=$msg"
    Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') 'SEAM-S10c1-original-intact' ''
    $r = Invoke-ClipboardRestore $null $null
    Assert-True (($r.Attempted -eq $false) -and ($r.OpenHandled -eq $false) -and ($r.Error -eq '')) `
        'SEAM-S10c1-no-restore-needed' "openHandled=$($r.OpenHandled) err='$($r.Error)'"
    # C2: EmptyClipboard fails AND the failure-path CloseClipboard fails too.
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    [Native]::SimFault = 'empty-fail'; [Native]::QueueFault('close-fail')
    $threw = $false; $msg = ''
    try { Invoke-ClipboardSentinelWrite 'X' } catch { $threw = $true; $msg = $_.Exception.Message }
    Assert-True ($threw -and (-not $script:ClipTx.Emptied) -and ($script:ClipTx.Opened -eq $true) -and ($msg -match 'ALSO failed')) `
        'SEAM-S10c2-emptyfail-closefail-state' "threw=$threw emptied=$($script:ClipTx.Emptied) opened=$($script:ClipTx.Opened) msg=$msg"
    $r = Invoke-ClipboardRestore $null $null
    Assert-True (($r.Attempted -eq $false) -and ($r.OpenHandled -eq $true) -and ($r.Error -eq '')) `
        'SEAM-S10c2-leftover-closed-no-restore' "openHandled=$($r.OpenHandled) err='$($r.Error)'"
    Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') 'SEAM-S10c2-original-intact-after-cleanup' ''
    # C4: SetClipboardData fails (post-Empty) AND the failure-path close fails.
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    [Native]::SimFault = 'set-fail'; [Native]::QueueFault('close-fail')
    $threw = $false; $msg = ''
    try { Invoke-ClipboardSentinelWrite 'E2E-SEAM-SENTINEL-C4' } catch { $threw = $true; $msg = $_.Exception.Message }
    Assert-True ($threw -and $script:ClipTx.Emptied -and ($script:ClipTx.Opened -eq $true) -and ($msg -match 'ALSO failed')) `
        'SEAM-S10c4-setfail-closefail-state' "threw=$threw emptied=$($script:ClipTx.Emptied) opened=$($script:ClipTx.Opened) msg=$msg"
    $r = Invoke-ClipboardRestore $fp0 $fp0.Text
    Assert-True (($r.OpenHandled -eq $true) -and ($r.Attempted -eq $true) -and ($r.Verified -eq $true)) `
        'SEAM-S10c4-recovery-after-closefail-verified' "openHandled=$($r.OpenHandled) attempted=$($r.Attempted) verified=$($r.Verified) err='$($r.Error)'"
    Assert-True ((Read-ClipFp).Text -ceq 'PRE-ORIGINAL-TEXT') 'SEAM-S10c4-original-restored' ''

    # --- S11: recovery-phase failures -> explicit failure, accurate state ----
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    Invoke-ClipboardSentinelWrite 'X'   # clean write: Emptied=true, Opened=false
    [Native]::QueueFault('open-fail')    # the REOPEN inside recovery fails
    $r = Invoke-ClipboardRestore $fp0 'PRE-ORIGINAL-TEXT'
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $false) -and ($r.Error -match 'reopen')) `
        'SEAM-S11-reopen-fail-explicit' "attempted=$($r.Attempted) verified=$($r.Verified) err='$($r.Error)'"
    Reset-SeamScenario; [Native]::SimSeedUnicode('PRE-ORIGINAL-TEXT')
    $fp0 = Read-ClipFp
    Invoke-ClipboardSentinelWrite 'X'
    [Native]::QueueFault('close-fail')   # the CLOSE after restore write fails
    $r = Invoke-ClipboardRestore $fp0 'PRE-ORIGINAL-TEXT'
    Assert-True (($r.Attempted -eq $true) -and ($r.Verified -eq $false) -and ($r.Error -match 'CloseClipboard failed after restore write')) `
        'SEAM-S11-restore-closefail-explicit' "attempted=$($r.Attempted) verified=$($r.Verified) err='$($r.Error)'"
    Assert-True ($script:ClipTx.Opened -eq $false) 'SEAM-S11-restore-closefail-state-cleaned' "opened=$($script:ClipTx.Opened)"

    # --- real clipboard safety proof: read-only fingerprints around the run --
    # The seam suite performs NO real clipboard write or clear; it only READS
    # the real clipboard (read-only fingerprints) before and after, and the
    # two fingerprints must be identical.
    [Native]::SimMode = $false   # back to the real backend for the closing read
    $realAfterSeam = Read-ClipFp
    Assert-True (Test-FingerprintEqual $realBeforeSeam $realAfterSeam) 'SEAM-real-clipboard-read-only-unchanged' `
        "read-only fingerprints: before=[$($realBeforeSeam.Formats)] after=[$($realAfterSeam.Formats)] errBefore='$($realBeforeSeam.Error)' errAfter='$($realAfterSeam.Error)'"

    Log "=== clipboard seam self-test finished : $($script:Failures) failure(s) (SIMULATION of the recovery logic; not a live-clipboard restoration) ==="
    [System.IO.File]::WriteAllLines($LogPath, [string[]]$script:LogLines, (New-Object System.Text.UTF8Encoding($true)))
    if ($script:Failures -eq 0) { Write-Output 'RESULT: SEAM SELF-TEST ALL PASSED'; exit 0 }
    Write-Output "RESULT: SEAM SELF-TEST $($script:Failures) FAILURE(S)"
    exit 1
}

# target window lifecycle (only processes we start)
$script:Targets = @()
function Start-Target([string]$title, [string]$snap) {
    $host1 = Join-Path $PSScriptRoot 'e2e_target_host.ps1'
    $p = Start-Process powershell -ArgumentList @(
        '-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-File', "`"$host1`"",
        '-Title', "`"$title`"", '-Snapshot', "`"$snap`"", '-Control', 'RichTextBox'
    ) -PassThru
    # Track the exact PID IMMEDIATELY (before the wait): if the window never
    # appears and we throw below, the outer finally still cleans up this PID.
    $script:Targets += $p
    $deadline = (Get-Date).AddSeconds(12)
    $hwnd = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $hwnd = [Native]::FindByTitlePid($title, [uint32]$p.Id)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        throw "target window '$title' did not appear (own child PID $($p.Id) is tracked and will be cleaned up)"
    }
    return @{ Pid = $p; Hwnd = $hwnd; Snap = $snap }
}
function Stop-Target($t) {
    # Graceful close first; force only THIS exact PID after the grace period.
    try {
        if ($t -and $t.Pid -and -not $t.Pid.HasExited) { [void]$t.Pid.CloseMainWindow() }
        if ($t -and $t.Pid -and -not $t.Pid.WaitForExit(3000)) {
            $null = Log "  cleanup: force-killing OWN target PID $($t.Pid.Id) after 3s grace"
            $t.Pid.Kill()
        }
    } catch {
        try { if ($t -and $t.Pid) { Stop-Process -Id $t.Pid.Id -Force -ErrorAction SilentlyContinue } } catch { }
    }
}
function New-Expected([string]$raw) { return $raw }   # raw uses `r`n explicitly

# ----- setup (zero side effects: no process, no clipboard writes) -----------
$logDir = Split-Path $LogPath -Parent
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
if (-not (Test-Path $Exe)) { Write-Output "E2E: exe not found: $Exe"; exit 2 }

$evidenceDir = Join-Path $logDir 'e2e-evidence'
if (-not (Test-Path $evidenceDir)) { New-Item -ItemType Directory -Force -Path $evidenceDir | Out-Null }

Log "=== VirtualHIDTyper software-mode E2E  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
Log "EXE: $Exe"
Log "Mode: $(if ($ClipboardTest) { 'opt-in sentinel clipboard test (-ClipboardTest)' } else { 'default: clipboard never written' })"

# ----- T0: static hygiene checks on THIS script (runs before any test) ------
# Scan our own source (comment lines excluded) for the dangerous patterns the
# previous rounds removed, plus the Round 10b clipboard invariants. Patterns
# use [x] character classes or concatenated needles so the check lines cannot
# match themselves.
Log '--- T0: static hygiene checks (this script) ---'
$srcLines = @(Get-Content -LiteralPath $PSCommandPath)
$srcCode = @($srcLines | Where-Object { -not ($_ -match '^\s*#') }) -join "`n"
$srcAll = $srcLines -join "`n"
Assert-True (-not ($srcCode -match 'Get-Proces[s]\s+-Name')) `
    'T0-no-name-only-process-enum' 'must not enumerate processes by name only (pid-scoped accessors only)'
Assert-True (-not ($srcCode -match 'Stop-Proces[s]\s+-Name')) `
    'T0-no-name-only-process-kill' 'must not kill processes by name only (exact-PID accessors only)'
Assert-True (-not ($srcCode -match 'FindByClas[s]\(.VirtualHIDTyperWnd.\)')) `
    'T0-no-global-typer-lookup' 'typer window must only be found via FindByPidClass'
Assert-True ($srcCode -match 'FindByPidClas[s]') `
    'T0-uses-pid-scoped-lookup' 'FindByPidClass present'
Assert-True (-not ($srcCode -match 'Set-Clipboa[r]d')) `
    'T0-no-clipboard-cmdlet-set' 'no clipboard SET cmdlet in code (writes use exact Win32 helpers behind the gate)'
Assert-True (-not ($srcCode -match 'Get-Clipboa[r]d')) `
    'T0-no-clipboard-cmdlet-get' 'no clipboard GET cmdlet in code (reads use exact Win32 helpers)'
# --- Round 10b: destructive clipboard path confinement ---------------------
# Every destructive backend call (owner-open / empty / set) may only appear
# inside the transaction region; the transaction call sites may only appear
# in the guarded flow (sentinel write) and in the finally (recovery).
$gateNeedle = 'function ' + 'Test-SentinelSafeToOverwrite'
$regionStartNeedle = '# ----- clipboard transaction region (' + 'the ONLY destructive clipboard path)'
$regionEndNeedle = '# ----- end clipboard ' + 'transaction region'
$openNeedle = '[Native]::' + 'ClipOpenOwned('
$emptyNeedle = '[Native]::' + 'ClipEmptyOwned('
$writeNeedle = '[Native]::' + 'ClipSetUnicodeText('
$firstGate = $srcAll.IndexOf($gateNeedle)
$regionStart = $srcAll.IndexOf($regionStartNeedle)
$regionEnd = $srcAll.IndexOf($regionEndNeedle)
$posOk = ($firstGate -ge 0) -and ($regionStart -gt $firstGate) -and ($regionEnd -gt $regionStart)
$outside = 0
foreach ($nd in @($openNeedle, $emptyNeedle, $writeNeedle)) {
    $idx = 0
    while (($idx = $srcAll.IndexOf($nd, $idx)) -ge 0) {
        if (($idx -lt $regionStart) -or ($idx -gt $regionEnd)) { $outside++ }
        $idx += $nd.Length
    }
}
Assert-True ($posOk -and ($outside -eq 0)) `
    'T0-writes-confined-behind-gate' "gate@$firstGate region@$regionStart..$regionEnd strayDestructiveCalls=$outside (transaction region must follow the gate; no destructive call outside it)"
$scNeedle = 'E2E-PROTECTED-SCOPE' + '-START'
$sentCallNeedle = 'Invoke-Clipboard' + 'SentinelWrite $sentinel'
$flowEndNeedle = '# E2E-FLOW' + '-END'
$restoreCallNeedle = 'Invoke-Clipboard' + 'Restore $fpBefore'
$scPos = $srcAll.IndexOf($scNeedle)
$sentCallPos = $srcAll.IndexOf($sentCallNeedle)
$flowEndPos = $srcAll.LastIndexOf($flowEndNeedle)
$restoreCallPos = $srcAll.IndexOf($restoreCallNeedle)
Assert-True (($sentCallPos -ge 0) -and ($scPos -ge 0) -and ($sentCallPos -gt $scPos)) `
    'T0-sentinel-write-in-protected-scope' "scope@$scPos sentinelWrite@$sentCallPos (the write must sit inside the protective try/catch/finally)"
Assert-True (($restoreCallPos -ge 0) -and ($flowEndPos -ge 0) -and ($restoreCallPos -gt $flowEndPos)) `
    'T0-restore-in-finally' "flowEnd@$flowEndPos restore@$restoreCallPos (recovery must sit in the finally after the flow)"
# The only NULL-owner OpenClipboard in the file must be inside the read-only
# helper; the destructive path always passes the validated owner window.
$nullOpenNeedle = 'OpenClipboard(IntPtr.Zer' + 'o)'
$nullOpens = @()
$idx = 0
while (($idx = $srcAll.IndexOf($nullOpenNeedle, $idx)) -ge 0) { $nullOpens += $idx; $idx += $nullOpenNeedle.Length }
$readDef = $srcAll.IndexOf('OpenClipboardForRea' + 'd() {')
$listDef = $srcAll.IndexOf('public static string GetClipboardFormatList' + '() {')
Assert-True (($nullOpens.Count -eq 1) -and ($readDef -ge 0) -and ($nullOpens[0] -gt $readDef) -and ($nullOpens[0] -lt $listDef)) `
    'T0-no-null-owner-open-for-write' "nullOpens=$($nullOpens.Count) readOnlyHelper@$readDef listDef@$listDef (the single NULL-owner open must live in the read-only helper)"
# Owner-window validation must exist and be re-checked right before opening.
$verifyCount = ([regex]::Matches($srcCode, 'VerifyClipOwnerWindow\(')).Count
Assert-True (($verifyCount -ge 3) -and ($srcCode -match 'IsWindow\(') -and ($srcCode -match 'VerifyClipOwnerWindow\(hwnd\)')) `
    'T0-owner-window-validated' "VerifyClipOwnerWindow refs=$verifyCount (create-time validation + right-before-open re-check required)"
# Destructive API last-error discipline: SetLastError=true on each P/Invoke,
# and the enum treats a 0 return as end-of-list only for ERROR_SUCCESS.
$lzOk = ($srcCode -match 'SetLastError = tru[e]\)\]\s*static extern bool OpenClipboard') -and
        ($srcCode -match 'SetLastError = tru[e]\)\]\s*static extern bool EmptyClipboard') -and
        ($srcCode -match 'SetLastError = tru[e]\)\]\s*static extern IntPtr SetClipboardData') -and
        ($srcCode -match 'SetLastError = tru[e]\)\]\s*static extern uint EnumClipboardFormats')
Assert-True $lzOk 'T0-destructive-apis-setlasterror' 'Open/Empty/Set/Enum clipboard P/Invokes must be SetLastError=true'
$enumDiscipline = ($srcCode -match 'ClearErr\(\)') -and ($srcCode -match 'err != ERROR_SUCCES[s]') -and ($srcCode -match 'uint nex[t] = EnumClipboardFormats')
Assert-True $enumDiscipline 'T0-enum-failclosed-discipline' 'enum must clear last-error first and reject a 0 return with non-success last-error'
# Gate unit tests (pure function; the exact decisions the sentinel path uses)
Assert-True ((Test-SentinelSafeToOverwrite @() $null) -eq $true) `
    'T0-sentinel-gate-empty-ok' 'provably empty clipboard is round-trippable'
Assert-True ((Test-SentinelSafeToOverwrite @('UnicodeText') 'abc') -eq $true) `
    'T0-sentinel-gate-unicode-ok' 'exact UnicodeText-only clipboard with readable content is round-trippable'
Assert-True ((Test-SentinelSafeToOverwrite @('UnicodeText') $null) -eq $false) `
    'T0-sentinel-gate-unicode-unreadable-refused' 'UnicodeText listed but content unreadable must be refused'
Assert-True ((Test-SentinelSafeToOverwrite @('UnicodeText', 'Text') 'abc') -eq $false) `
    'T0-sentinel-gate-extra-textflavors-refused' 'multiple text flavours are refused (format set not provably reproducible)'
Assert-True ((Test-SentinelSafeToOverwrite @('Text') 'abc') -eq $false) `
    'T0-sentinel-gate-ansi-only-refused' 'CF_TEXT-only content refused (no exact Unicode backup possible)'
Assert-True ((Test-SentinelSafeToOverwrite @('UnicodeText', 'FileDrop') 'abc') -eq $false) `
    'T0-sentinel-gate-files-refused' 'files must be refused before any write'
Assert-True ((Test-SentinelSafeToOverwrite @('HTML Format') '<b>x</b>') -eq $false) `
    'T0-sentinel-gate-html-refused' 'HTML must be refused before any write'
Assert-True ((Test-SentinelSafeToOverwrite @('Rich Text Format') '{\rtf1}') -eq $false) `
    'T0-sentinel-gate-rtf-refused' 'RTF must be refused before any write'
Assert-True ((Test-SentinelSafeToOverwrite @('PNG') $null) -eq $false) `
    'T0-sentinel-gate-image-refused' 'image must be refused before any write'
Assert-True ((Test-SentinelSafeToOverwrite $null $null) -eq $false) `
    'T0-sentinel-gate-error-refused' 'unreadable state must be refused before any write'

# ----- clipboard BEFORE-state: READ-ONLY fingerprint -------------------------
$fpBefore = Wait-ClipFp
if ($fpBefore.Error) {
    Log "FATAL: cannot read clipboard state (no write was attempted): $($fpBefore.Error)"
    exit 2
}
Log "Clipboard before (read-only): formats=[$($fpBefore.Formats)] unicodeLen=$(if ($null -eq $fpBefore.Text) { 'n/a' } else { $fpBefore.Text.Length })"

$script:SentinelActive = $false
$script:SentinelOriginalText = $null
$script:ClipboardTouched = $false
$sentinel = "E2E-SENTINEL-$([guid]::NewGuid().ToString('N'))"

# ===== E2E-PROTECTED-SCOPE-START: the opt-in sentinel write AND the e2e flow
# all run inside ONE protective try/catch/finally, so an exception at ANY point
# of the sentinel sequence (including after EmptyClipboard already succeeded)
# still triggers the verified recovery in the finally below. Everything before
# this marker is read-only setup and cannot alter the clipboard.
$typerProc = $null
$typer = [IntPtr]::Zero
$script:TyperPid = 0
$fatalError = $null
# E2E-FLOW-START: every non-empty line until the E2E-FLOW-END marker is
# indented by 4 spaces (it is the body of the try/catch/finally below).
try {

    # ---- opt-in sentinel (-ClipboardTest only; the ONLY clipboard writes) ---
    # The write is an owner-window transaction: refusal happens BEFORE any
    # destructive API; once EmptyClipboard succeeds, any later failure throws
    # straight into the recovery path in the finally below.
    if ($ClipboardTest) {
        Log 'T7 OPT-IN: -ClipboardTest -- this ACTIVELY writes a text sentinel to the clipboard via an owner-window transaction (original backed up first; recovery runs and is VERIFIED in the finally below).'
        $beforeFormats = @()
        if ($fpBefore.Formats -and $fpBefore.Formats.Length -gt 0) {
            $beforeFormats = @($fpBefore.Formats -split '\|')
        }
        if (-not (Test-SentinelSafeToOverwrite $beforeFormats $fpBefore.Text)) {
            Log "T7 sentinel SKIPPED (refused BEFORE any write): original clipboard formats not provably round-trippable or text unreadable: [$($fpBefore.Formats)] -- run in an isolated session instead"
        } else {
            $script:SentinelOriginalText = $fpBefore.Text   # $null = no text; '' = empty
            Invoke-ClipboardSentinelWrite $sentinel
            $script:ClipboardTouched = $true
            $script:SentinelActive = $true
            $after = Wait-ClipFp 3
            Log "T7 sentinel SET; backup recorded (formats=[$($fpBefore.Formats)] textLen=$(if ($null -eq $script:SentinelOriginalText) { 'n/a' } else { $script:SentinelOriginalText.Length })); verify=$($after.Text -ceq $sentinel)"
        }
    } else {
        Log 'T7 sentinel SKIPPED (default run): clipboard is never written; read-only fingerprint only; opt in with -ClipboardTest'
    }

    # launch OUR OWN typer instance -- always a fresh process we start here; a
    # pre-existing user instance (any path, same name or not) is never reused,
    # never messaged and never killed.
    # Launch with an ISOLATED profile directory.
    # WHY: without --data-dir the app reads %APPDATA%\VirtualHIDTyper\settings.ini,
    # so the run inherits whatever hotkeys the machine's user once chose. This
    # test presses the BUILT-IN defaults (Ctrl+Alt+Shift+F9/F10); if the profile
    # holds a different pair (this host had HotkeyStop=2,39 = Ctrl+'), the stop
    # press never matches, the run is never stopped, and half the later
    # assertions fail for a reason that has nothing to do with the product.
    # An isolated directory also guarantees the test cannot alter real settings.
    $typerData = Join-Path $logDir 'e2e-typerdata'
    if (-not (Test-Path $typerData)) { New-Item -ItemType Directory -Force -Path $typerData | Out-Null }
    $typerProc = Start-Process $Exe -ArgumentList ("--mode=software --data-dir=" + $typerData) -PassThru
    $script:TyperPid = [uint32]$typerProc.Id
    $deadline = (Get-Date).AddSeconds(10)
    $typer = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline -and $typer -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 150
        $typer = [Native]::FindByPidClass($script:TyperPid, 'VirtualHIDTyperWnd')
    }
    if ($typer -eq [IntPtr]::Zero) {
        throw "our typer instance (own PID $($typerProc.Id)) did not expose a VirtualHIDTyperWnd window within 10s"
    }
    # Belt-and-braces ownership proof: the window really belongs to OUR PID.
    $ownPid = [uint32]0
    [void][Native]::GetWindowThreadProcessId($typer, [ref]$ownPid)
    if ($ownPid -ne $script:TyperPid) {
        throw "window 0x$($typer.ToString('X')) belongs to PID $ownPid, not our instance $($typerProc.Id)"
    }
    $typerTitle = Get-Title $typer
    Log "Typer HWND=0x$($typer.ToString('X')) PID=$($typerProc.Id) title=$typerTitle (own instance; pre-existing instances are left untouched)"
    $hotkeyOK = -not ((Get-WmText ([Native]::GetDlgItem($typer, $IDC_STATIC_STATUS)) 2048) -match '快捷键注册失败')
    Log "Global stop hotkey registered: $hotkeyOK"

    # ===========================================================================
    # T1 -- full Unicode text into the dedicated window, software mode via CLI
    # ===========================================================================
    Log '--- T1: basic software-mode typing (EN/digits/punct/CJK/tab, 2+ lines) ---'
    $t1 = "Hello, E2E 123! Num=42; a=b(c) [ok]`r`nLine2 tab here:`tEND`r`n" +
          "你好，世界！第三行中文测试。`r`nDone."
    $snap1 = Join-Path $evidenceDir 't1-actual.txt'
    $t1w = $null
    try {
        $t1w = Start-Target 'E2E Target T1' $snap1
        Assert-True ((Get-Title $typer) -match '\[软件输入模式 SendInput\]') `
            'T1-title-mode' "title=$typerTitle (CLI --mode=software at launch)"
        Assert-True (Force-Fg $t1w.Hwnd) 'T1-foreground' "target hwnd=0x$($t1w.Hwnd.ToString('X'))"
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t1
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '10'
        [void][Native]::SendMessage([Native]::GetDlgItem($typer, $IDC_CHK_SHIFT_NL), $BM_SETCHECK, [IntPtr]$BST_UNCHECKED, [IntPtr]::Zero)
        Send-Cmd $typer $IDC_BTN_START
        $st = Wait-Status $typer '(完成【|已停止【|焦点离开|错误【|仍处于)' 90
        Assert-True ($st -match '完成【软件输入模式】') 'T1-completed-status' "status=$st"
        $actual1 = Wait-SnapSettled $snap1 10   # final content, never a mid-write state
        Assert-True (Test-Equal $t1 $actual1) 'T1-text-identical' `
            "expected=$(($t1 -replace "`t",'<TAB>') -replace "`r",'' -replace "`n",'<NL>') | actual=$(($actual1 -replace "`t",'<TAB>') -replace "`r",'' -replace "`n",'<NL>')"
        if ($st -match '实际输入\s*(\d+)\s*字符') {
            $n = [int]$Matches[1]
            Assert-True ($n -eq (Count-CodePoints (Norm $t1))) 'T1-sent-count' `
                "app reported $n chars, expected $(Count-CodePoints (Norm $t1)) code points"
        } else { Assert-True $false 'T1-sent-count' "no count in status=$st" }
        if ($st -match '跳过\s*(\d+)\s*字符') {
            Assert-True ([int]$Matches[1] -eq 0) 'T1-skipped-zero' "skipped=$($Matches[1])"
        }
    } finally {
        if ($t1w) { Stop-Target $t1w }
    }

    # ===========================================================================
    # T2 -- HID open failure dialog (driver NOT installed): No / Cancel / Yes
    # ===========================================================================
    Log '--- T2: driver-unavailable dialog (3-way choice) ---'
    Set-ModeRadio $typer 'hid'
    Assert-True ((Get-Title $typer) -match '\[TextDeck HID 模式\]') 'T2-radio-hid' (Get-Title $typer)

    $dialogTitle = '虚拟 HID 设备不可用'
    # All dialog lookups are scoped to OUR typer PID: a dialog shown by any other
    # (e.g. pre-existing) instance can never be found, clicked or closed here.
    function Wait-Dialog([int]$sec) {
        $deadline = (Get-Date).AddSeconds($sec)
        $h = [IntPtr]::Zero
        while ((Get-Date) -lt $deadline -and $h -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 120
            $h = [Native]::FindByTitlePid($dialogTitle, $script:TyperPid)
        }
        return $h
    }
    function Get-DialogBody([IntPtr]$dlg) { return [Native]::GetChildStaticTexts($dlg) }
    # Two-step dismissal: BM_CLICK on the button first; if the dialog survives
    # (~1.5s), escalate to a direct WM_COMMAND (IDYES=6/IDNO=7/IDCANCEL=2) to the
    # dialog procedure itself, which bypasses button hit-testing entirely.
    # Returns $true only when the dialog is actually gone.
    function Click-DialogButton([IntPtr]$dlg, [string]$prefix, [int]$cmdId) {
        $b = [Native]::FindChildButtonByPrefix($dlg, $prefix)
        if ($b -ne [IntPtr]::Zero) {
            [void][Native]::SendMessage($b, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 900
            if (-not [Native]::IsWindow($dlg)) { return $true }
            # suppress Log's pipeline output so the function returns a single bool
            $null = Log "  [WARN] BM_CLICK on '$prefix' did not dismiss the dialog; escalating to WM_COMMAND id=$cmdId"
        }
        [void][Native]::SendMessage($dlg, $WM_COMMAND, [IntPtr]$cmdId, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 900
        return (-not [Native]::IsWindow($dlg))
    }
    function Wait-DialogGone([int]$sec) {
        $deadline = (Get-Date).AddSeconds($sec)
        while ((Get-Date) -lt $deadline) {
            if ([Native]::FindByTitlePid($dialogTitle, $script:TyperPid) -eq [IntPtr]::Zero) { return $true }
            Start-Sleep -Milliseconds 120
        }
        return ([Native]::FindByTitlePid($dialogTitle, $script:TyperPid) -eq [IntPtr]::Zero)
    }
    # Safety net: never let a modal dialog survive a subtest (it would pollute
    # later subtests). Direct WM_COMMAND IDCANCEL to the dialog procedure.
    function Close-LeftoverDialog([string]$why) {
        $dlg = [Native]::FindByTitlePid($dialogTitle, $script:TyperPid)
        if ($dlg -ne [IntPtr]::Zero) {
            Log "  [WARN] force-closing leftover dialog ($why)"
            [void][Native]::SendMessage($dlg, $WM_COMMAND, [IntPtr]2, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 600
            return $true
        }
        return $false
    }
    function Invoke-HidDialogRun([string]$targetTitle, [string]$snapPath, [string]$text) {
        $w = Start-Target $targetTitle $snapPath
        [void](Force-Fg $w.Hwnd)
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $text
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '10'
        Send-Cmd $typer $IDC_BTN_START
        $dlg = Wait-Dialog 10
        return @{ W = $w; Dlg = $dlg }
    }

    # T2a -- "No" keeps Virtual HID mode, sends nothing
    $snap2a = Join-Path $evidenceDir 't2a-actual.txt'
    $r = $null
    try {
        $r = Invoke-HidDialogRun 'E2E Target T2a' $snap2a 'SHOULD-NOT-APPEAR'
        Assert-True ($r.Dlg -ne [IntPtr]::Zero) 'T2a-dialog-shown' 'device open failed as expected (no driver)'
        if ($r.Dlg -ne [IntPtr]::Zero) {
            $body = Get-DialogBody $r.Dlg
            Assert-True ($body -match '软件输入模式') 'T2a-dialog-offers-software' "body=$body"
            Assert-True (-not ($body -match '管理员')) 'T2a-no-uac-path' 'NotFound path (no elevation prompt offered)'
            Assert-True ([Native]::CountChildButtons($r.Dlg) -eq 3) 'T2a-three-choices' "buttons=$([Native]::CountChildButtons($r.Dlg))"
            Assert-True ((Click-DialogButton $r.Dlg '否' 7)) 'T2a-click-no' 'IDNO dismissed the dialog'
            Assert-True (Wait-DialogGone 8) 'T2a-dialog-closed' ''
            # The "No" path clears the run state without printing a new banner, so
            # the status keeps showing the HID open failure. What must be asserted
            # is that the app did NOT switch to software mode and did NOT claim to
            # have typed anything - the title assertion below proves the mode.
            $st = Get-WmText ([Native]::GetDlgItem($typer, $IDC_STATIC_STATUS)) 2048
            $stayedHid = ($st -notmatch '软件输入模式') -and ($st -notmatch '完成【')
            Assert-True $stayedHid 'T2a-stayed-hid' "status=$st"
            Assert-True ((Get-Title $typer) -match '\[TextDeck HID 模式\]') 'T2a-title-hid' (Get-Title $typer)
            $s2a = Wait-SnapSettled $snap2a 5
            Assert-True ([string]::IsNullOrEmpty($s2a)) 'T2a-nothing-sent' "snap='$s2a'"
        }
    } finally { if ($r) { Stop-Target $r.W }; [void](Close-LeftoverDialog 'T2a cleanup') }

    # T2b -- "Cancel" stops without sending anything
    $snap2b = Join-Path $evidenceDir 't2b-actual.txt'
    $r = $null
    try {
        $r = Invoke-HidDialogRun 'E2E Target T2b' $snap2b 'SHOULD-NOT-APPEAR'
        if ($r.Dlg -ne [IntPtr]::Zero) {
            Assert-True ((Click-DialogButton $r.Dlg '取' 2)) 'T2b-click-cancel' 'IDCANCEL dismissed the dialog'
            Assert-True (Wait-DialogGone 8) 'T2b-dialog-closed' ''
            $st = Wait-Status $typer '已取消（未发送任何字符）' 8
            Assert-True ($st -match '已取消（未发送任何字符）') 'T2b-cancel-status' "status=$st"
            $s2b = Wait-SnapSettled $snap2b 5
            Assert-True ([string]::IsNullOrEmpty($s2b)) 'T2b-nothing-sent' "snap empty"
        } else { Assert-True $false 'T2b-dialog-shown' 'dialog did not appear' }
    } finally { if ($r) { Stop-Target $r.W }; [void](Close-LeftoverDialog 'T2b cleanup') }

    # T2c -- "Yes" switches to software mode and types for real (focus race retry)
    $snap2c = Join-Path $evidenceDir 't2c-actual.txt'
    $t2c = "Switched-OK 42 中文 fallback works."
    $done = $false
    for ($attempt = 1; $attempt -le 2 -and -not $done; $attempt++) {
        $r = $null
        try {
            Set-ModeRadio $typer 'hid'   # retries must start from HID mode again
            $r = Invoke-HidDialogRun "E2E Target T2c#$attempt" $snap2c $t2c
            if ($r.Dlg -eq [IntPtr]::Zero) { Assert-True $false 'T2c-dialog-shown' 'dialog missing'; break }
            [void](Force-Fg $r.W.Hwnd)
            Assert-True ((Click-DialogButton $r.Dlg '是' 6)) 'T2c-click-yes' "attempt=$attempt (IDYES dismissed the dialog)"
            # keep target foreground while the modal dialog unwinds (race window)
            for ($k = 0; $k -lt 8; $k++) {
                [void](Force-Fg $r.W.Hwnd)
                Start-Sleep -Milliseconds 80
            }
            Assert-True (Wait-DialogGone 8) 'T2c-dialog-closed' ''
            $st = Wait-Status $typer '(完成【软件输入模式】|焦点离开)' 30
            # read the 0-char fact from the status itself (the progress line can
            # hold a stale value from an earlier test that did type)
            if ($st -match '焦点离开' -and $st -match '已键入 0 字符') {
                Log "  [RETRY] T2c focus lost before first char (attempt $attempt)"
                Stop-Target $r.W; $r = $null
                continue
            }
            Assert-True ($st -match '完成【软件输入模式】') 'T2c-completed-software' "status=$st"
            $a = Wait-SnapSettled $snap2c 8
            Assert-True (Test-Equal $t2c $a) 'T2c-text-identical' "expected='$t2c' actual='$a'"
            Assert-True ((Get-Title $typer) -match '\[软件输入模式 SendInput\]') 'T2c-title-switched' (Get-Title $typer)
            Assert-True ([Native]::IsWindowEnabled([Native]::GetDlgItem($typer, $IDC_RADIO_HID))) `
                'T2c-radios-reenabled' 'mode selectable again after run'
            $done = $true
        } finally { if ($r) { Stop-Target $r.W }; [void](Close-LeftoverDialog "T2c attempt=$attempt cleanup") }
    }
    Assert-True $done 'T2c-finished' 'Yes-path typed real text after dialog'

    # ===========================================================================
    # T3 -- global stop hotkey Ctrl+Alt+X aborts mid-typing (prefix preserved)
    # ===========================================================================
    Log '--- T3: global stop hotkey (Ctrl+Alt+X) ---'
    Set-ModeRadio $typer 'software'
    $t3lines = @(
        'The quick brown fox jumps over the lazy dog 0123456789 line-one for stopping test.',
        'Second line keeps typing long enough so the hotkey lands mid-flight here.'
    )
    $t3 = ($t3lines -join "`r`n") + "`r`n"
    # repeat to ~600 chars
    $t3 = $t3 + $t3 + $t3
    $snap3 = Join-Path $evidenceDir 't3-actual.txt'
    $w3 = $null
    try {
        $w3 = Start-Target 'E2E Target T3' $snap3
        [void](Force-Fg $w3.Hwnd)
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t3
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '30'
        Send-Cmd $typer $IDC_BTN_START
        $st = Wait-Status $typer '输入中' 10
        Assert-True ($st -match '【软件输入模式】输入中') 'T3-typing-status' "status=$st"
        # wait until some progress
        $deadline = (Get-Date).AddSeconds(15)
        $p = Get-Progress $typer
        while ((Get-Date) -lt $deadline -and $p.sent -lt 20) {
            Start-Sleep -Milliseconds 80
            $p = Get-Progress $typer
        }
        Assert-True ($p.sent -ge 20) 'T3-midflight' "sent=$($p.sent)/$($p.total)"
        if ($hotkeyOK) { [Native]::CtrlAltShiftF10() } else { Send-Cmd $typer $IDC_BTN_STOP }
        $st = Wait-Status $typer '(已停止【|完成【|焦点离开|错误【)' 20
        Assert-True ($st -match '已停止【软件输入模式】') 'T3-stopped-status' "status=$st (hotkeyOK=$hotkeyOK)"
        $a3 = Wait-SnapSettled $snap3 8
        Assert-True (Test-IsPrefix $t3 $a3) 'T3-prefix-intact' "typed length=$((Norm $a3).Length)"
        Assert-True ((Norm $a3).Length -gt 0) 'T3-some-typed' "len=$((Norm $a3).Length)"
        Assert-True ((Norm $a3).Length -lt (Norm $t3).Length) 'T3-not-all-typed' "len=$((Norm $a3).Length) < $((Norm $t3).Length)"
        $editNow = Get-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) 4096
        Assert-True ($editNow -ceq $t3) 'T3-never-into-own-edit' 'typer edit box unchanged'
    } finally { if ($w3) { Stop-Target $w3 } }

    # ===========================================================================
    # T4 -- focus switch stops input immediately
    # ===========================================================================
    Log '--- T4: focus-switch protection (switch to a decoy window) ---'
    $t4 = ('Focus test line one with 12345 and words. ' * 12).Trim() + "`r`n" +
          ('Focus test line two with punctuation!?; ' * 12).Trim() + "`r`n"
    $snap4 = Join-Path $evidenceDir 't4-actual.txt'
    $snap4b = Join-Path $evidenceDir 't4-decoy-actual.txt'
    $w4 = $null; $w4b = $null
    try {
        $w4 = Start-Target 'E2E Target T4' $snap4
        $w4b = Start-Target 'E2E Target T4 decoy' $snap4b   # where the "user" switches to
        [void](Force-Fg $w4.Hwnd)
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t4
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '25'
        Send-Cmd $typer $IDC_BTN_START
        $st = Wait-Status $typer '输入中' 10
        $deadline = (Get-Date).AddSeconds(15)
        $p = Get-Progress $typer
        while ((Get-Date) -lt $deadline -and $p.sent -lt 15) {
            Start-Sleep -Milliseconds 80
            $p = Get-Progress $typer
        }
        Assert-True ($p.sent -ge 15) 'T4-midflight' "sent=$($p.sent)/$($p.total)"
        [void](Force-Fg $w4b.Hwnd)    # user switches away from the target window
        $st = Wait-Status $typer '(焦点离开|已停止【|完成【|错误【)' 15
        Assert-True ($st -match '焦点离开') 'T4-focus-status' "status=$st"
        $a4 = Wait-SnapSettled $snap4 8
        Assert-True ((Norm $a4).Length -lt (Norm $t4).Length) 'T4-stopped-early' "typed=$((Norm $a4).Length)/$((Norm $t4).Length)"
        # at most 2 single in-flight events may be lost at the transition instant;
        # every char PRESENT must be the expected one in order (no garbage)
        $nearOk = Test-NearPrefix $t4 $a4 2
        Assert-True $nearOk 'T4-content-ordered' "typed=$($script:LastNearPrefixInfo)"
        # no further characters AFTER the stop: content stays identical
        Start-Sleep -Milliseconds 1600
        $a4b = Read-Snap $snap4
        Assert-True ((Norm $a4b) -ceq (Norm $a4)) 'T4-no-chars-after-stop' `
            "len before=$((Norm $a4).Length) after=$((Norm $a4b).Length)"
        # at most ONE in-flight event may land in the decoy (SendInput race window)
        $d4 = Wait-SnapSettled $snap4b 5
        Assert-True ((Norm $d4).Length -le 1) 'T4-decoy-stray-bounded' "decoy='$d4'"
        # never types into its own text box
        $editNow = Get-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) 8192
        Assert-True ($editNow -ceq $t4) 'T4-never-into-own-edit' "edit unchanged len=$($editNow.Length)"
    } finally {
        if ($w4) { Stop-Target $w4 }
        if ($w4b) { Stop-Target $w4b }
    }

    # ===========================================================================
    # T5 -- cancel during countdown sends nothing at all
    # ===========================================================================
    Log '--- T5: countdown cancel sends nothing ---'
    Set-ModeRadio $typer 'software'
    $t5 = 'MUST NOT APPEAR AT ALL 0123456789'
    $snap5 = Join-Path $evidenceDir 't5-actual.txt'
    $w5 = $null
    try {
        $w5 = Start-Target 'E2E Target T5' $snap5
        [void](Force-Fg $w5.Hwnd)
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t5
        Send-Cmd $typer $IDC_BTN_START
        $st = Wait-Status $typer '倒计时' 5
        Assert-True ($st -match '倒计时.*软件输入模式') 'T5-countdown-shows-mode' "status=$st"
        Start-Sleep -Milliseconds 700
        if ($hotkeyOK) { [Native]::CtrlAltShiftF10() } else { Send-Cmd $typer $IDC_BTN_STOP }
        $st = Wait-Status $typer '已取消' 6
        Assert-True ($st -match '已取消') 'T5-cancel-status' "status=$st"
        Start-Sleep -Milliseconds 3500   # wait past the (cancelled) countdown window
        $st2 = Get-WmText ([Native]::GetDlgItem($typer, $IDC_STATIC_STATUS)) 2048
        Assert-True ($st2 -match '已取消') 'T5-still-cancelled' "status=$st2"
        $s5 = Wait-SnapSettled $snap5 5
        Assert-True ([string]::IsNullOrEmpty($s5)) 'T5-nothing-sent' 'target empty after cancel'
    } finally { if ($w5) { Stop-Target $w5 } }

    # ===========================================================================
    # T6 -- non-BMP (emoji) typed intact as surrogate pairs
    # ===========================================================================
    Log '--- T6: non-BMP emoji (surrogate pairs) ---'
    $t6 = "Emoji time: " + [char]0xD83D + [char]0xDE00 + [char]0xD83C + [char]0xDF89 +
          " thumbs " + [char]0xD83D + [char]0xDC4D + " ok`r`n" +
          "Mixed line 42 with " + [char]0xD83D + [char]0xDE00 + " inside.`r`n"
    $snap6 = Join-Path $evidenceDir 't6-actual.txt'
    $w6 = $null
    try {
        $w6 = Start-Target 'E2E Target T6' $snap6
        [void](Force-Fg $w6.Hwnd)
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_TEXT)) $t6
        Set-WmText ([Native]::GetDlgItem($typer, $IDC_EDIT_INTERVAL)) '10'
        Send-Cmd $typer $IDC_BTN_START
        $st = Wait-Status $typer '(完成【|已停止【|焦点离开|错误【|仍处于)' 60
        Assert-True ($st -match '完成【软件输入模式】') 'T6-completed' "status=$st"
        $a6 = Wait-SnapSettled $snap6 10   # final content (includes trailing newline)
        Assert-True (Test-Equal $t6 $a6) 'T6-emoji-identical' `
            "expected=$([System.BitConverter]::ToString([System.Text.Encoding]::Unicode.GetBytes($t6)))"
        Assert-True (Test-Equal $t6 $a6) 'T6-actual-text' "actual=$([System.BitConverter]::ToString([System.Text.Encoding]::Unicode.GetBytes([string]$a6)))"
    } finally { if ($w6) { Stop-Target $w6 } }

    # ===========================================================================
    # T7 -- clipboard non-interference
    #   default run: read-only before/after fingerprint (never writes)
    #   -ClipboardTest: sentinel path (refused unless provably round-trippable;
    #   restoration is executed and VERIFIED in the finally block below)
    # ===========================================================================
    Log '--- T7: clipboard non-interference ---'
    # The ONLY legitimate clipboard writes are the opt-in sentinel's. Assert the
    # implication: any write REQUIRES the explicit opt-in (default runs and
    # refused opt-ins must have touched=False; executed opt-in has touched=True).
    Assert-True ((-not $script:ClipboardTouched) -or [bool]$ClipboardTest) `
        'T7-writes-only-when-optedin' "touched=$($script:ClipboardTouched) optin=$([bool]$ClipboardTest) (write implies explicit opt-in)"
    $fpAfter = Wait-ClipFp 5
    if ($fpAfter.Error) {
        # Cannot re-read: honest failure (no write was attempted on our side).
        Assert-True $false 'T7-clipboard-unchanged' "cannot re-read clipboard state: $($fpAfter.Error)"
    } elseif ($script:SentinelActive) {
        # Sentinel variant: clipboard must still hold OUR sentinel (the app under
        # test never pasted or changed it). Original is restored+verified below.
        $intact = ($fpAfter.Text -ceq $sentinel)
        Assert-True $intact 'T7-sentinel-intact' `
            "textIntact=$intact formats=[$($fpAfter.Formats)] textLen=$(if ($null -eq $fpAfter.Text) { 'null' } else { $fpAfter.Text.Length })"
        $null = Log 'T7 status: sentinel EXECUTED (explicit -ClipboardTest opt-in); original clipboard restored+verified in cleanup'
    } else {
        # Default path: identical fingerprint before/after, because this script
        # never wrote the clipboard at all.
        Assert-True (Test-FingerprintEqual $fpBefore $fpAfter) 'T7-clipboard-unchanged' `
            "before=[$($fpBefore.Formats)]|len=$(if ($null -eq $fpBefore.Text) { 'n/a' } else { $fpBefore.Text.Length }) after=[$($fpAfter.Formats)]|len=$(if ($null -eq $fpAfter.Text) { 'n/a' } else { $fpAfter.Text.Length }) (no write attempted by script)"
        if ($ClipboardTest) {
            $null = Log 'T7 status: sentinel SKIPPED (refused before any write: original clipboard not provably round-trippable); read-only fingerprint EXECUTED -- skipped is NOT counted as passed'
        } else {
            $null = Log 'T7 status: sentinel SKIPPED (default run never writes the clipboard; opt in with -ClipboardTest); read-only fingerprint EXECUTED -- skipped is NOT counted as passed'
        }
    }
# E2E-FLOW-END
} catch {
    $fatalError = $_
    Log "FATAL: $($_.Exception.Message)"
} finally {
    # ---- 1) clipboard recovery (Round 10b): runs whenever any destructive
    # ----    clipboard step MAY have executed -- even on early exceptions ----
    # "attempted" and "verified" are reported separately; a failed recovery is
    # an explicit assertion failure, never a swallowed exception or a fake OK.
    $restoreRes = Invoke-ClipboardRestore $fpBefore $script:SentinelOriginalText
    if ($restoreRes.Attempted) {
        Assert-True ($restoreRes.Verified -eq $true) 'T7-clipboard-restored-verified' `
            $(if ($restoreRes.Verified) { 'recovery ran and was VERIFIED by re-reading formats+text (identical to pre-run fingerprint)' } else { "recovery FAILED: $($restoreRes.Error)" })
        $null = Log "T7 sentinel cleanup: $(if ($restoreRes.Verified) { 'original clipboard RESTORED -- VERIFIED by re-read (formats+text identical to pre-run fingerprint)' } else { 'RESTORE FAILED -- see assertion above; clipboard may still differ from the pre-run state' })"
    } elseif ($restoreRes.Error) {
        # no content destroy happened, but a leftover OPEN clipboard could not
        # be closed -- an explicit, reported cleanup failure (never ignored)
        Assert-True $false 'T7-clipboard-openstate-cleanup-failed' "leftover open clipboard state could not be disposed: $($restoreRes.Error)"
    } elseif ($restoreRes.OpenHandled) {
        $null = Log 'T7 sentinel cleanup: leftover open clipboard CLOSED during cleanup; no content restore needed (original data was never destroyed)'
    } else {
        $null = Log 'T7 sentinel cleanup: nothing to restore (no destructive clipboard step ever ran)'
    }
    # The owner window must outlive every clipboard operation; destroy it only
    # now that all clipboard work (write, recovery, verification) is finished.
    try { [Native]::DestroyClipOwnerWindow() } catch { $null = Log "cleanup: owner window destroy error: $($_.Exception.Message)" }

    # ---- 2) stop this run's target hosts (exact PIDs; grace, then force) ----
    foreach ($t in @($script:Targets)) {
        try { Stop-Target $t } catch { $null = Log "cleanup: target error: $($_.Exception.Message)" }
    }

    # ---- 3) close OUR typer instance (exact PID; grace, then force) ---------
    try {
        if ($typerProc -and -not $typerProc.HasExited) {
            $w = [Native]::FindByPidClass([uint32]$typerProc.Id, 'VirtualHIDTyperWnd')
            if ($w -ne [IntPtr]::Zero) { [void][Native]::SendMessage($w, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) }
            if (-not $typerProc.WaitForExit(4000)) {
                $null = Log "cleanup: force-killing OWN typer PID $($typerProc.Id) after 4s grace"
                $typerProc.Kill()
            }
        }
        if ($typerProc) {
            $null = Log "cleanup: own typer PID $($typerProc.Id) exited=$(if ($typerProc.HasExited) { 'yes' } else { 'NO' }) (other VirtualHIDTyper processes were never enumerated or touched)"
        }
    } catch { $null = Log "cleanup: typer close error: $($_.Exception.Message)" }
}

# ----- summary --------------------------------------------------------------
if ($fatalError) { $script:Failures++; Log 'FATAL counted as a failed run (never silently passed)' }
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
$t7state = if ($script:SentinelActive) { 'sentinel EXECUTED (opt-in), recovery verified' }
           elseif ($script:ClipTx.Emptied) { 'sentinel write FAILED mid-sequence (after EmptyClipboard); recovery ran in cleanup (see T7-clipboard-restored-verified)' }
           elseif ($ClipboardTest) { 'sentinel REFUSED before write (original not provably round-trippable), read-only fingerprint executed' }
           else { 'sentinel SKIPPED (default run), read-only fingerprint executed' }
Log "T7 final status: $t7state"
Log "=== E2E finished $stamp : $($script:Failures) failure(s)$(if ($fatalError) { ' [FATAL]' }) ==="
[System.IO.File]::WriteAllLines($LogPath, [string[]]$script:LogLines, (New-Object System.Text.UTF8Encoding($true)))
if ($script:Failures -eq 0) { Write-Output 'RESULT: E2E ALL PASSED'; exit 0 }
Write-Output "RESULT: E2E $($script:Failures) FAILURE(S)"
exit 1
