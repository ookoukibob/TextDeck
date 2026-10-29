# ---------------------------------------------------------------------------
# e2e_r13_hk_holder.ps1
#
# Simulates ANOTHER application occupying one global hotkey combination via
# the real Windows RegisterHotKey mechanism (thread-associated, NULL hwnd).
# The registration lives until this process exits, which lets the E2E harness
# deterministically provoke ERROR_HOTKEY_ALREADY_REGISTERED (1409) in the
# typer and verify the no-loss rollback behaviour.
#
# Usage: .\e2e_r13_hk_holder.ps1 -Mods 7 -Vk 0x7C -HoldSecs 300
#   Mods: MOD_ALT=1 MOD_CONTROL=2 MOD_SHIFT=4 MOD_WIN=8 (Ctrl+Alt+Shift = 7)
#   Vk:   virtual-key code (F13 = 0x7C)
# Prints HOLDER-REGISTERED and blocks; exits 0 on release.
# ---------------------------------------------------------------------------
param(
    [int]$Mods = 7,
    [int]$Vk = 0x7C,
    [int]$HoldSecs = 300,
    [string]$ReadyFile = ""   # written AFTER successful registration; the
                              # harness polls it instead of guessing timings
)

# PS5.1's -File argument chain (esp. via Start-Process) delivers the value
# SPACE-wrapped (" path " -> first char 32); a leading space then makes
# Set-Content -Path fail (drive ' C') and -Encoding misreport as
# NamedParameterNotFound - the ready file never appeared while the chord WAS
# registered (run9/run10's 40s-timeout WARNs). Trim BEFORE any use.
$ReadyFile = $ReadyFile.Trim().Trim('"').Trim()

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class HKHolder {
    // SetLastError=true is required for GetLastWin32Error() to see 1409.
    [DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);
    [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr hWnd, int id);
}
'@

$ok = [HKHolder]::RegisterHotKey([IntPtr]::Zero, 4242, [uint32]$Mods, [uint32]$Vk)
if (-not $ok) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Output "HOLDER-REGISTER-FAILED err=$err"
    exit 1
}
if ($ReadyFile) { Set-Content -Path $ReadyFile -Value 'ok' -Encoding ASCII }
Write-Output "HOLDER-REGISTERED mods=$Mods vk=0x$($Vk.ToString('X2'))"
# The registration is bound to this thread's lifetime; sleeping keeps it held
# without needing a message loop (we never need the WM_HOTKEY delivery).
Start-Sleep -Seconds $HoldSecs
[void][HKHolder]::UnregisterHotKey([IntPtr]::Zero, 4242)
exit 0
