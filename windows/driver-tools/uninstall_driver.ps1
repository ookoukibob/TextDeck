# ---------------------------------------------------------------------------
# uninstall_driver.ps1  --  SAFE removal of THIS driver package only.
#
# Removes ONLY:
#   * device instances whose hardware ID is exactly Root\TextDeckHidDriver
#     (and, via /subtree, the VHF/HID child nodes the driver itself created)
#   * driver packages whose Original Name is exactly textdeckhiddriver.inf
# It never deletes unrelated devices, never deletes a package that other
# devices still use (pnputil refuses and we report it), and never touches
# certificates, BCD, TESTSIGNING, HVCI or Secure Boot.
#
# FAILURE DISCIPLINE (Round 11): every pnputil call's REAL exit code is checked.
# An earlier revision piped pnputil straight into Write-Host and exited 0 even
# when the device could not be removed, so "uninstall succeeded" was reported
# while the device was still present. Removal failures are now fatal.
#
# WHY /subtree: a loaded VHF driver owns child nodes (VHF\HID_DEVICE_SYSTEM_VHF
# and HID\HID_DEVICE_SYSTEM_VHF). Removing only the root instance fails with
# "cannot disable this device" while those children are attached; /subtree
# removes the whole tree that this device owns.
#
# Run as Administrator.
# ---------------------------------------------------------------------------
param(
    [switch]$KeepDriverPackage
)

$ErrorActionPreference = 'Stop'
function Log($m) { Write-Host ("[uninstall] " + $m) }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must run elevated."
    exit 1
}

$failures = 0
$HWID = 'Root\TextDeckHidDriver'

# Preflight package enumeration before removing any device instance. If PnPUtil
# cannot provide a trustworthy inventory, stop before mutating device state.
if (-not $KeepDriverPackage) {
    $null = (& pnputil /enum-drivers 2>&1 | Out-String)
    $preflightExit = $LASTEXITCODE
    if ($preflightExit -ne 0) {
        Log "ERROR: preflight pnputil /enum-drivers failed with exit code $preflightExit; no devices were removed."
        exit 1
    }
}

# --- 1. remove OUR device instances (exact hardware-ID match only) ----------
# Enumerate via the device REGISTRY (fast) and confirm the hardware ID from the
# registry value, instead of calling Get-PnpDeviceProperty once per device:
# on this machine that per-device WMI round-trip across ~280 devices made the
# uninstall appear to hang for minutes. Only devices whose Enum key actually
# contains our exact hardware ID are ever touched.
function Get-OurInstanceIds {
    $found = @()
    $enumRoot = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT'
    if (-not (Test-Path $enumRoot -ErrorAction Stop)) {
        throw "Required device-enumeration registry key is missing: $enumRoot. Refusing to assume no TextDeck device exists."
    }

    $deviceKeys = @()
    try {
        $deviceKeys = @(Get-ChildItem $enumRoot -ErrorAction Stop)
    } catch {
        throw "Cannot enumerate $enumRoot; refusing to assume no TextDeck device exists. $($_.Exception.Message)"
    }

    foreach ($devKey in $deviceKeys) {
        if ($devKey.PSChildName -notlike 'TEXTDECKHIDDRIVER*') { continue }
        $instances = @()
        try {
            $instances = @(Get-ChildItem $devKey.PSPath -ErrorAction Stop)
        } catch {
            throw "Cannot enumerate potential TextDeck device key $($devKey.PSPath); refusing to continue. $($_.Exception.Message)"
        }
        foreach ($inst in $instances) {
            $props = $null
            try {
                $props = Get-ItemProperty -LiteralPath $inst.PSPath -ErrorAction Stop
            } catch {
                throw "Cannot read potential TextDeck device instance $($inst.PSPath); refusing to assume it is unrelated. $($_.Exception.Message)"
            }
            if (-not $props.HardwareID) {
                throw "HardwareID is missing from potential TextDeck device instance $($inst.PSPath); refusing to assume it is unrelated."
            }
            $hw = @($props.HardwareID)
            if ($hw -contains $HWID) {
                $found += ('ROOT\' + $devKey.PSChildName + '\' + $inst.PSChildName)
            }
        }
    }
    return $found
}

$targets = @(Get-OurInstanceIds)

if ($targets.Count -eq 0) {
    Log "no device instance with hardware ID '$HWID' found (nothing to remove)"
} else {
    foreach ($instId in $targets) {
        Log ("removing device " + $instId + " (with /subtree: the VHF/HID children it owns)")
        $out = & pnputil /remove-device $instId /subtree 2>&1
        $code = $LASTEXITCODE
        $out | ForEach-Object { Write-Host ("    " + $_) }
        if ($code -ne 0) {
            $failures++
            Log ("ERROR: pnputil /remove-device returned $code for " + $instId)
        }
    }
}

# --- 2. delete ONLY our staged package(s) -----------------------------------
if ($KeepDriverPackage) {
    Log "-KeepDriverPackage: leaving the driver package in the Driver Store"
} else {
    $published = (& pnputil /enum-drivers 2>&1 | Out-String)
    $enumExit = $LASTEXITCODE
    if ($enumExit -ne 0) {
        Log "ERROR: pnputil /enum-drivers failed with exit code $enumExit; no package deletion will be attempted."
        exit 1
    }
    $blocks = [regex]::Split($published, '\r?\n\r?\n')
    foreach ($b in $blocks) {
        # pnputil output is LOCALIZED: on a zh-CN system the fields are
        # "发布名称:" / "原始名称:" instead of "Published Name:" / "Original Name:".
        # Matching the English labels silently found nothing and the package was
        # never deleted while the script still reported success. Both the
        # original INF name and the oemNN.inf published name are therefore
        # matched LANGUAGE-INDEPENDENTLY: the INF file name is a locale-neutral
        # token, and the oemNN.inf token is identified by its exact shape.
        if ($b -notmatch 'textdeckhiddriver\.inf') { continue }
        if ($b -match '(oem\d+\.inf)') {
            $oem = $Matches[1]
        } else { continue }
        Log ("deleting driver package $oem (original name textdeckhiddriver.inf)")
        $out = & pnputil /delete-driver $oem /uninstall 2>&1
        $code = $LASTEXITCODE
        $out | ForEach-Object { Write-Host ("    " + $_) }
        if ($code -ne 0) {
            # A package still bound to another device is REFUSED here on purpose:
            # deleting it could break unrelated hardware. Report, do not force.
            $failures++
            Log ("ERROR: pnputil /delete-driver returned $code for $oem - it may still be in use by a device; NOT forcing.")
        }
    }
}

# --- 3. verify the outcome instead of assuming it ---------------------------
Start-Sleep -Milliseconds 500
$still = @(Get-OurInstanceIds)
if ($still.Count -gt 0) {
    Log ("VERIFY FAILED: device instance(s) still present: " + ($still -join ', '))
    $failures++
} else {
    Log "verified: no device instance with hardware ID '$HWID' remains"
}

if (-not $KeepDriverPackage) {
    $left = (& pnputil /enum-drivers 2>&1 | Out-String)
    $enumExit = $LASTEXITCODE
    if ($enumExit -ne 0) {
        Log "VERIFY FAILED: pnputil /enum-drivers exited $enumExit; package state is unknown"
        $failures++
    } elseif ($left -match 'textdeckhiddriver\.inf') {
        Log "VERIFY FAILED: a textdeckhiddriver.inf package is still in the Driver Store"
        $failures++
    } else {
        Log "verified: no textdeckhiddriver.inf package remains in the Driver Store"
    }
}

if ($failures -gt 0) {
    Log "FAILED with $failures error(s). A reboot may be required if the driver was loaded."
    exit 1
}
Log "done. A reboot may be required if the driver was loaded."
exit 0
