# ---------------------------------------------------------------------------
# install_driver.ps1  --  SAFE driver package installer (local patch, replaces
# the upstream install_driver.bat which ran `bcdedit /set testsigning on`).
#
# Flow (fail closed at every step):
#   1. verify package files (INF + SYS + CAT) exist in $PackageDir
#      (default = the real output of driver-tools\build_driver.ps1:
#       driver\TextDeckHidDriver\x64\Debug\TextDeckHidDriver)
#   2. verify signtool.exe exists -- if not, REFUSE (both install and
#      -CheckOnly mode)
#   3. read target policy (read-only): Secure Boot state + test-signing state.
#      If either query fails/returns unknown -> REFUSE.
#   4. catalog-based signature gate (documented SignTool forms):
#        - CAT self-signature status must be Valid (Get-AuthenticodeSignature
#          on the .cat). NotSigned / HashMismatch / NotTrusted / UnknownError
#          -> REFUSE, regardless of TestSigning/SecureBoot state.
#        - dev/test target (TestSigning=ON AND SecureBoot=OFF), per
#          "Verifying the Signature of a Test-Signed Catalog File":
#              signtool verify /v /pa          <CAT>        (CAT signature)
#              signtool verify /v /pa /c <CAT>  <INF|SYS>    (file covered by CAT)
#        - otherwise (production target), per "Verifying the Release-Signature":
#              signtool verify /kp /v          <CAT>
#              signtool verify /kp /v /c <CAT> <INF|SYS>
#        Every command's REAL exit code decides; any non-zero -> REFUSE.
#      The .sys embedded signature status is logged as INFORMATIONAL ONLY:
#      a package with a signed CAT covering an embedded-unsigned .sys is
#      valid and loadable (catalog-based signing is the normal case).
#      If the gate refuses: EXIT HERE -- nothing is staged, no device
#      instance is created.
#   5. read-only snapshot of existing Root\TextDeckHidDriver device instances
#      via a FAIL-CLOSED enumerator (SetupAPI), taken BEFORE any mutating
#      step: an enumeration error (SetupDiGetClassDevs failure, mid-
#      enumeration error, unreadable hardware ID) is reported as
#      "cannot determine" and REFUSES the install -- it is never treated as
#      "no devices". If ANY instance exists -> REFUSE and exit: nothing is
#      staged, no device is updated/created/deleted, no reboot.
#      (An install is never silently converted into an update path.)
#   6. stage the driver package with `pnputil /add-driver <INF>` -- WITHOUT
#      /install, because /install would also try to update already-matching
#      devices and would therefore bypass the step-5 snapshot. Staging alone
#      does not touch any device. Non-zero exit code -> log + EXIT
#      immediately; SetupAPI is never called.
#   7. re-confirm with the same fail-closed enumerator that no matching
#      device appeared (any enumeration error here also aborts BEFORE device
#      creation/binding), then create the root-enumerated device via SetupAPI
#      and install the staged package on THIS instance only:
#        SetupDiBuildDriverInfoList(SPDIT_COMPATDRIVER) -> EnumDriverInfo ->
#        GetDriverInfoDetail -> SetupGetInfDriverStoreLocation (published
#        oemNN.inf -> driver store INF; PATH resolution only, per docs the
#        API does not inspect contents) -> SetupGetInfPublishedName
#        round-trip cross-check -> CONTENT IDENTITY verification (SHA-256 of
#        the INF + every file declared via CatalogFile / SourceDisksFiles /
#        CopyFiles SOURCE names / ServiceBinary, resolved by the LIMITED,
#        documented INF inventory parser -- unsupported syntax, unresolved
#        tokens or any path escaping the package dir => refuse) against this
#        run's target package -> exactly ONE identity-verified store package
#        (zero, multiple distinct dirs, unreadable files or failed mapping =>
#        refuse) -> INSTALL-SECTION SEMANTICS: identity only proves one
#        package; every verified record for it must report the SAME non-empty
#        DDInstall SectionName, otherwise refuse as ambiguous (never a
#        first-entry pick) -> DiInstallDevice(set, our SP_DEVINFO_DATA, the
#        EXACTLY selected SP_DRVINFO_DATA, Flags=0).
#      Never the wide UpdateDriverForPlugAndPlayDevices(hwid, INSTALLFLAG_FORCE)
#      path, and never DiInstallDevice with DriverInfoData=NULL ("best match"
#      guessing). Hardware-ID comparisons are EXACT full identifiers
#      (case-insensitive), never substring tests.
#      The created instance ID is this run's OWNERSHIP TOKEN: on failure,
#      only that exact ID is rolled back (never a before/after snapshot
#      diff). If ownership cannot be confirmed, nothing is deleted and the
#      residual state is reported for manual handling.
#      After a SetupAPI failure the staged package MAY remain in the Driver
#      Store; this script never deletes driver packages (they may be shared).
#
# Use -CheckOnly to run steps 1-5 only (read-only; no staging, no device
# creation). Note: policy queries need elevation to succeed -- when they
# cannot be answered, the gate refuses as "policy unknown" (fail closed).
# Use -LibraryOnly to dot-source this script for unit tests (defines the
# functions, skips the main body).
#
# NEVER does: bcdedit / testsigning / Secure Boot / HVCI / BitLocker changes,
# downloads, certificate installation, or touching other devices.
# ---------------------------------------------------------------------------
param(
    [string]$PackageDir = "$PSScriptRoot\..\driver\TextDeckHidDriver\x64\Debug\TextDeckHidDriver",
    [switch]$CheckOnly,
    [switch]$LibraryOnly
)

$ErrorActionPreference = 'Stop'
function Log($m) { Write-Host ("[install] " + $m) }

# --- dependency seams (overridable by unit tests after `-LibraryOnly` dot-source)
$script:GateDeps = @{
    # Path to signtool.exe. Tests override this to simulate a missing tool.
    SigntoolPath       = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
    # Returns the Authenticode status of a file as a plain string
    # ('Valid' | 'NotSigned' | 'HashMismatch' | 'NotTrusted' | 'UnknownError' | ...).
    # Tests override this to feed canned statuses.
    GetSignatureStatus = { param([string]$Path) (Get-AuthenticodeSignature -LiteralPath $Path).Status.ToString() }
    # Runs signtool and returns its REAL exit code. stderr is left unredirected
    # (redirecting it with 2>&1 under $ErrorActionPreference='Stop' would turn
    # SignTool's error output into terminating ErrorRecords).
    # Tests override this to feed canned exit codes.
    RunSigntool        = {
        param([string]$ToolPath, [string[]]$Arguments)
        try {
            $out = & $ToolPath @Arguments
            $code = $LASTEXITCODE
        } catch {
            return -2   # tool could not be launched at all -> treat as failure
        }
        $out | ForEach-Object { Write-Host ("    " + $_) }
        return [int]$code
    }
}

# --- read-only target policy probe ------------------------------------------
# Returns @{ SecureBoot = $true|$false|$null; TestSigning = $true|$false|$null }
# ($null = query failed / unknown -> gate refuses, fail closed).
function Get-TargetPolicy {
    $secureBoot = $null
    try { $secureBoot = Confirm-SecureBootUEFI } catch { $secureBoot = $null }

    $testSign = $null
    # cmd-merge keeps bcdedit's stderr from becoming a terminating ErrorRecord
    # under $ErrorActionPreference='Stop'; a non-zero exit (e.g. access denied
    # when unelevated) means "unknown".
    $bd = cmd /c "bcdedit /enum {current} 2>&1" | Out-String
    if ($LASTEXITCODE -eq 0) {
        $testSign = [bool]($bd -match '(?im)^\s*testsigning\s+Yes')
    }
    return @{ SecureBoot = $secureBoot; TestSigning = $testSign }
}

# --- catalog-based signature gate (fail closed) ------------------------------
# Returns @{ Ok = $true|$false; Reason = '...'; Checks = @( @{Name;Pass;Detail} ... ) }
# - signtool.exe must exist
# - both policy values must be known
# - CAT Get-AuthenticodeSignature status must be exactly 'Valid'
# - every signtool verification exit code must be 0:
#     * dev/test target (TestSigning ON + SecureBoot OFF): /pa  (PnP policy,
#       per WDK "Verifying the Signature of a Test-Signed Catalog File")
#     * any other policy state:                          /kp  (kernel policy,
#       per WDK "Verifying the Release-Signature")
#     * CAT self-verify:            verify <pol> /v <CAT>
#     * INF/SYS covered-by-CAT:     verify <pol> /v /c <CAT> <file>
# The .sys embedded status is NOT part of the decision (informational only).
function Test-PackageGate {
    param(
        [Parameter(Mandatory = $true)][string]$InfPath,
        [Parameter(Mandatory = $true)][string]$SysPath,
        [Parameter(Mandatory = $true)][string]$CatPath,
        [Parameter(Mandatory = $true)][hashtable]$Policy
    )
    $checks = @()

    # 1. signtool.exe must exist (rule: missing tool -> refuse)
    $tool = $script:GateDeps.SigntoolPath
    if (-not (Test-Path -LiteralPath $tool)) {
        $checks += @{ Name = 'signtool.exe present'; Pass = $false; Detail = "not found: $tool" }
        return @{ Ok = $false; Reason = "signtool.exe not found at '$tool' -- cannot verify package"; Checks = $checks }
    }
    $checks += @{ Name = 'signtool.exe present'; Pass = $true; Detail = $tool }

    # 2. policy must be known (rule: unknown policy -> refuse)
    $sbKnown = ($Policy -ne $null -and $Policy.ContainsKey('SecureBoot') -and $null -ne $Policy.SecureBoot)
    $tsKnown = ($Policy -ne $null -and $Policy.ContainsKey('TestSigning') -and $null -ne $Policy.TestSigning)
    if (-not $sbKnown -or -not $tsKnown) {
        $detail = "SecureBoot=$(if ($sbKnown) { $Policy.SecureBoot } else { 'unknown' }) TestSigning=$(if ($tsKnown) { $Policy.TestSigning } else { 'unknown' })"
        $checks += @{ Name = 'target policy known'; Pass = $false; Detail = $detail }
        return @{ Ok = $false; Reason = "policy query failed/unknown ($detail) -- refusing (fail closed)"; Checks = $checks }
    }
    $checks += @{ Name = 'target policy known'; Pass = $true; Detail = "SecureBoot=$($Policy.SecureBoot) TestSigning=$($Policy.TestSigning)" }

    # 3. catalog self-signature status must be exactly Valid
    try {
        $catStatus = [string](& $script:GateDeps.GetSignatureStatus $CatPath)
    } catch {
        $catStatus = "error:$($_.Exception.Message)"
    }
    if ($catStatus -ne 'Valid') {
        $checks += @{ Name = 'CAT signature status (Authenticode)'; Pass = $false; Detail = $catStatus }
        return @{ Ok = $false; Reason = "catalog signature status is '$catStatus' (only 'Valid' accepted; HashMismatch/NotTrusted/UnknownError/NotSigned are refused regardless of TestSigning/SecureBoot)"; Checks = $checks }
    }
    $checks += @{ Name = 'CAT signature status (Authenticode)'; Pass = $true; Detail = $catStatus }

    # 4. verification parameters by target policy
    $devTarget = ($Policy.TestSigning -eq $true -and $Policy.SecureBoot -eq $false)
    if ($devTarget) {
        $pol = '/pa'   # PnP device installation signing requirements (test-signed catalog doc)
        $track = 'dev/test target (TestSigning=ON + SecureBoot=OFF) -> /pa'
    } else {
        $pol = '/kp'   # kernel-mode code signing policy + PnP requirements (release-signature doc)
        $track = "production target -> /kp (policy: TestSigning=$($Policy.TestSigning) SecureBoot=$($Policy.SecureBoot))"
    }

    # 4a. CAT self-verify: signtool verify <pol> /v <CAT>
    $code = [int](& $script:GateDeps.RunSigntool $tool @('verify', $pol, '/v', $CatPath))
    $checks += @{ Name = "CAT verify: signtool verify $pol /v <CAT>"; Pass = ($code -eq 0); Detail = "exit=$code | $track" }
    if ($code -ne 0) {
        return @{ Ok = $false; Reason = "signtool verify $pol /v <CAT> failed (exit=$code)"; Checks = $checks }
    }

    # 4b. INF must be covered by the CAT: signtool verify <pol> /v /c <CAT> <INF>
    $code = [int](& $script:GateDeps.RunSigntool $tool @('verify', $pol, '/v', '/c', $CatPath, $InfPath))
    $checks += @{ Name = "INF covered by CAT: signtool verify $pol /v /c <CAT> <INF>"; Pass = ($code -eq 0); Detail = "exit=$code" }
    if ($code -ne 0) {
        return @{ Ok = $false; Reason = "INF is not verified in the catalog (exit=$code)"; Checks = $checks }
    }

    # 4c. SYS must be covered by the CAT: signtool verify <pol> /v /c <CAT> <SYS>
    $code = [int](& $script:GateDeps.RunSigntool $tool @('verify', $pol, '/v', '/c', $CatPath, $SysPath))
    $checks += @{ Name = "SYS covered by CAT: signtool verify $pol /v /c <CAT> <SYS>"; Pass = ($code -eq 0); Detail = "exit=$code" }
    if ($code -ne 0) {
        return @{ Ok = $false; Reason = "SYS is not verified in the catalog (exit=$code)"; Checks = $checks }
    }

    return @{ Ok = $true; Reason = "package signed: CAT Valid + INF/SYS covered ($track)"; Checks = $checks }
}

# --- stage the driver package; returns pnputil's REAL exit code -------------
# NO /install flag on purpose: `pnputil /add-driver <INF> /install` would also
# try to update any already-matching device, which would mutate existing
# devices before our snapshot/rollback logic could protect them. Plain
# `/add-driver` only stages the package into the Driver Store (no device
# is touched); driver binding happens later via SetupAPI for the instance
# this script itself creates.
# stderr stays unredirected on purpose: merging it with 2>&1 under
# $ErrorActionPreference='Stop' would convert pnputil's error output into
# terminating ErrorRecords instead of letting us inspect $LASTEXITCODE.
function Invoke-PnputilStage {
    param([Parameter(Mandatory = $true)][string]$InfPath)
    & pnputil /add-driver $InfPath | ForEach-Object { Write-Host ("    " + $_) }
    return [int]$LASTEXITCODE
}

# --- fail-closed install sequence (ordering invariant; mock-testable) -------
# Order (each step aborts the next on failure):
#   1. pre-install snapshot via the fail-closed enumerator: Success=false
#      (enum API failure / mid-enumeration error / unreadable property) =>
#      abort BEFORE staging -- "cannot determine" is never treated as "no
#      devices". Any existing instance => refuse (install never becomes an
#      update of existing devices).
#   2. stage the package with `pnputil /add-driver <INF>` (NO /install:
#      staging never touches devices); non-zero exit -> abort
#   3. post-staging re-check with the same fail-closed enumerator:
#      Success=false => abort before device creation/binding; any instance
#      present => abort and leave it untouched (not ours).
#   4. create the instance and bind the staged package with the DEVICE-SCOPED
#      DiInstallDevice on this run's own SP_DEVINFO_DATA (never the wide
#      hardware-ID force update). The created instance ID is this run's
#      ownership token; on failure ONLY that exact ID is rolled back. If
#      ownership cannot be confirmed, nothing is deleted and the residual
#      state is reported for manual handling.
# Returns @{Phase='ok'|'ok-reboot'|'snapshot-failed'|'refused-existing'|
#           'stage-failed'|'recheck-failed'|'race-existing'|'setupapi-failed';
#           Ok; Before; StageCode; RegisterCode; CreatedInstanceId; ErrorCode;
#           ErrorMessage; ReCheck; Rollback}
function Invoke-InstallSequence {
    param(
        [Parameter(Mandatory = $true)][string]$InfPath,
        [Parameter(Mandatory = $true)][string]$SysGuidText,
        [Parameter(Mandatory = $true)][string]$HwidText,
        # Injection seams for unit tests; defaults call the real
        # implementations (SetupAPI helper / pnputil). The listing seam must
        # return @{Success;Instances;ErrorCode;ErrorMessage}.
        [scriptblock]$ListInstances = { param($g, $h) [PnpSetup]::ListInstances([Guid]$g, $h) },
        [scriptblock]$Stage = { param($p) Invoke-PnputilStage -InfPath $p },
        [scriptblock]$RegisterAndInstall = { param($g, $h, $p) [PnpSetup]::RegisterAndInstall([Guid]$g, $h, $p) },
        [scriptblock]$RemoveInstance = { param($g, $h, $i) [PnpSetup]::RemoveInstance([Guid]$g, $h, $i) }
    )
    # 1. pre-install snapshot -- fail closed on enumeration errors
    $snap1 = & $ListInstances $SysGuidText $HwidText
    if (-not $snap1.Success) {
        Log "ERROR: pre-install device enumeration failed (code $($snap1.ErrorCode): $($snap1.ErrorMessage)) - aborting BEFORE staging (fail closed, not treated as 'no devices')."
        return @{ Phase = 'snapshot-failed'; Ok = $false; ErrorCode = $snap1.ErrorCode; ErrorMessage = "$($snap1.ErrorMessage)" }
    }
    $before = @($snap1.Instances | Where-Object { $_ })
    if ($before.Count -gt 0) {
        return @{ Phase = 'refused-existing'; Ok = $false; Before = $before }
    }
    Log "pre-existing device instances: (none)"

    # 2. stage (staging only; no /install flag => no device is touched)
    $stageCode = [int](& $Stage $InfPath)
    if ($stageCode -ne 0) {
        Log "ERROR: pnputil /add-driver failed with exit code $stageCode."
        Log "ABORTING: no device instance is created or updated."
        return @{ Phase = 'stage-failed'; Ok = $false; Before = $before; StageCode = $stageCode }
    }
    Log "driver package staged (pnputil /add-driver WITHOUT /install: staging only; no device is touched)"

    # 3. post-staging re-check -- fail closed on enumeration errors
    $snap2 = & $ListInstances $SysGuidText $HwidText
    if (-not $snap2.Success) {
        Log "ERROR: post-staging device enumeration failed (code $($snap2.ErrorCode): $($snap2.ErrorMessage)) - aborting before device creation/binding (fail closed)."
        return @{ Phase = 'recheck-failed'; Ok = $false; Before = $before; StageCode = $stageCode; ErrorCode = $snap2.ErrorCode; ErrorMessage = "$($snap2.ErrorMessage)" }
    }
    $reCheck = @($snap2.Instances | Where-Object { $_ })
    if ($reCheck.Count -gt 0) {
        Log "REFUSING to continue: matching device instance(s) present after staging: $($reCheck -join ', ')"
        Log "They were not created by this script and are left untouched."
        return @{ Phase = 'race-existing'; Ok = $false; Before = $before; StageCode = $stageCode; ReCheck = $reCheck }
    }

    # 4. create + device-scoped bind; the returned instance ID is ours
    $reg = & $RegisterAndInstall $SysGuidText $HwidText $InfPath
    $r = [int]$reg.Code
    $ownedId = $null
    if ($null -ne $reg.InstanceId -and "$($reg.InstanceId)" -ne '') { $ownedId = "$($reg.InstanceId)" }
    Log "SetupAPI create + select-INF + DiInstallDevice (device-scoped, flags=0) result: $r (0=OK, 999=reboot required, other=error); created instance: $(if ($ownedId) { $ownedId } else { 'unconfirmed' })$(if ($reg.ErrorMessage) { '; detail: ' + $reg.ErrorMessage } else { '' })"
    if ($r -eq 999) {
        return @{ Phase = 'ok-reboot'; Ok = $true; Before = $before; StageCode = $stageCode; RegisterCode = $r; CreatedInstanceId = $ownedId }
    }
    if ($r -ne 0) {
        if ($r -eq 5) {
            Log "ERROR_ACCESS_DENIED: rejected by driver-signature/CI policy (expected on this machine: Secure Boot ON, test signing OFF)."
        }
        # Rollback by OWNERSHIP, never by before/after diff: remove exactly the
        # instance ID this run created -- or nothing if ownership is unknown.
        $rollback = @()
        if ($null -eq $ownedId) {
            Log "OWNERSHIP NOT CONFIRMED after error ${r}: this run's instance ID is unknown - deleting NOTHING (fail-safe)."
            Log "MANUAL ACTION REQUIRED: inspect Root\TextDeckHidDriver device instances manually before retrying."
            # Informational only (never used for a safety decision here):
            $post = & $ListInstances $SysGuidText $HwidText
            if ($post.Success) {
                Log "informational: matching instances now present: $(@($post.Instances | Where-Object { $_ }) -join ', ')"
            } else {
                Log "informational: post-failure enumeration also failed (code $($post.ErrorCode)) - inspect manually."
            }
        } else {
            Log "rolling back ONLY the instance created by this run: $ownedId"
            $rm = [int](& $RemoveInstance $SysGuidText $HwidText $ownedId)
            $rollback += @{ InstanceId = $ownedId; Result = $rm; OwnedByThisRun = $true }
            if ($rm -eq 0) {
                Log "removed instance $ownedId (created by this run)."
            } else {
                Log "ROLLBACK of $ownedId failed (code $rm) - MANUAL ACTION REQUIRED: remove that instance manually."
            }
        }
        Log "staged package may remain in the Driver Store; this script never deletes driver packages (they may be shared)."
        return @{ Phase = 'setupapi-failed'; Ok = $false; Before = $before; StageCode = $stageCode; RegisterCode = $r; CreatedInstanceId = $ownedId; Rollback = $rollback }
    }
    return @{ Phase = 'ok'; Ok = $true; Before = $before; StageCode = $stageCode; RegisterCode = 0; CreatedInstanceId = $ownedId }
}

# =============================== main =======================================

# --- 1. SetupAPI helper (compiled in memory; NO system change) --------------
# Compiled BEFORE the -LibraryOnly return below, so unit tests can exercise
# the pure logic (target-driver selection, hardware-ID exact comparison,
# probe-result classification, x64 struct layout) after dot-sourcing. In-memory
# compilation itself changes nothing on the system.
$cs = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
public static class PnpSetup {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA {
        public int cbSize;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }
    // Fail-closed enumeration result: Success=false means "could NOT determine"
    // and must be treated as fatal by callers -- never as "no matching devices".
    public class EnumResult {
        public bool Success;
        public List<string> Instances;
        public int ErrorCode;     // 0 on success
        public string ErrorMessage;
    }
    // Result of creating + device-specific installing. InstanceId is the device
    // instance THIS call created and registered (null = ownership unknown, so
    // callers must delete nothing).
    public class InstallResult {
        public int Code;          // 0=OK, 999=reboot required, other=negative=script-level, positive=Win32 error
        public string InstanceId;
        public string ErrorMessage;
    }
    // SP_DRVINFO_DATA -- MODERN V2 layout per setupapi.h (the V1 form has no
    // DriverDate/Version; SP_DRVINFO_DATA resolves to V2 unless
    // USE_SP_DRVINFO_DATA_V1 is defined). SetupDi*Xxx verifies
    // cbSize == sizeof(SP_DRVINFO_DATA) (else ERROR_INVALID_PARAMETER), so the
    // managed layout must match the x64 native ABI: 1568 bytes.
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SP_DRVINFO_DATA {
        public uint cbSize;
        public uint DriverType;                  // SPDIT_CLASSDRIVER / SPDIT_COMPATDRIVER
        public IntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Description;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string MfgName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string ProviderName;
        public System.Runtime.InteropServices.ComTypes.FILETIME DriverDate;
        public ulong Version;
    }
    // SP_DRVINFO_DETAIL_DATA per setupapi.h (_W form): SectionName[LINE_LEN=256],
    // InfFileName[MAX_PATH=260], DrvDescription[LINE_LEN=256], then the
    // variable-length HardwareID buffer (ANYSIZE_ARRAY). SizeConst=1 reproduces
    // the native sizeof (1584 on x64) for cbSize; the actual ID list is read
    // from the raw unmanaged buffer, parsed per CompatIDsOffset.
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SP_DRVINFO_DETAIL_DATA {
        public uint cbSize;
        public System.Runtime.InteropServices.ComTypes.FILETIME InfDate;
        public uint CompatIDsOffset;   // >1 => first string in HardwareID buffer is the hardware ID
        public uint CompatIDsLength;
        public IntPtr Reserved;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string SectionName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string InfFileName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string DrvDescription;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 1)] public string HardwareID;
    }
    // One driver-list entry's VERIFIED identity (from SP_DRVINFO_DETAIL_DATA).
    public class DrvDetail {
        public string InfFileName;   // full-qualified INF name as reported by SetupAPI
        public string HardwareId;    // primary hardware ID (null when the entry has none)
        public string SectionName;   // DDInstall section reported for this entry
        public string Description;   // driver description reported in the DETAIL struct
        public string ProviderName;
    }
    // Pure target-driver selection outcome.
    public class SelectResult {
        public int Code;    // 0=selected, 2=target INF not found, 3=ambiguous
        public int Index;   // selected index into the record list, or -1
        public string Message;
    }
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr hwndParent);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, string enumerator, IntPtr hwndParent, uint flags);
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiEnumDeviceInfo(IntPtr devInfoSet, uint memberIndex, ref SP_DEVINFO_DATA devInfoData);
    // Signature MUST match SetupAPI.h verbatim:
    //   BOOL SetupDiCreateDeviceInfoW(HDEVINFO DeviceInfoSet,
    //                                 PCWSTR   DeviceName,          <-- instance name
    //                                 const GUID *ClassGuid,
    //                                 PCWSTR   DeviceDescription,
    //                                 HWND     hwndParent,
    //                                 DWORD    CreationFlags,
    //                                 PSP_DEVINFO_DATA DeviceInfoData);  <-- OUT
    // An earlier revision passed SP_DEVINFO_DATA as argument #2 (the instance
    // name slot) and 0 as CreationFlags. The OS then read the struct as a wide
    // string and rejected it with 0xE0000205 (ERROR_INVALID_DEVINST_NAME), so
    // device creation could never succeed.
    // DeviceName must be a NON-NULL, backslash-free device name: on this build
    // NULL is rejected with the same 0xE0000205, and so is any name containing
    // '\' (verified by direct probe: "TextDeckHidDriver" => ok,
    // "Root\TextDeckHidDriver" => 0xE0000205, NULL => 0xE0000205). The ROOT
    // enumerator plus DICD_GENERATE_ID turns this name into the instance ID
    // ROOT\TextDeckHidDriver\000N; the hardware ID is set separately below.
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiCreateDeviceInfo(IntPtr devInfoSet,
        string deviceName, ref Guid classGuid, string deviceDescription,
        IntPtr hwndParent, uint creationFlags, ref SP_DEVINFO_DATA devInfoData);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiSetDeviceRegistryProperty(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, uint property, string multiSzBuffer);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceRegistryProperty(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, uint property, out uint propertyRegDataType,
        byte[] propertyBuffer, uint propertyBufferSize, out uint requiredSize);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiGetDeviceInstanceId(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, StringBuilder deviceInstanceId,
        uint deviceInstanceIdSize, out uint requiredSize);
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiCallClassInstaller(uint installFunction,
        IntPtr devInfoSet, ref SP_DEVINFO_DATA devInfoData);
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr devInfoSet);
    // Driver-list APIs (setupapi.dll): the documented way to pick a SPECIFIC
    // driver for DiInstallDevice -- SetupDiBuildDriverInfoList ->
    // SetupDiEnumDriverInfo -> SetupDiGetDriverInfoDetail (newdev.h remarks).
    // SPDIT_COMPATDRIVER lists the drivers compatible with this device's
    // hardware IDs, staged packages included.
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiBuildDriverInfoList(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, uint driverType);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiEnumDriverInfo(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, uint driverType, uint memberIndex,
        ref SP_DRVINFO_DATA driverInfoData);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupDiGetDriverInfoDetail(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, ref SP_DRVINFO_DATA driverInfoData,
        IntPtr driverInfoDetailData, uint driverInfoDetailDataSize, out uint requiredSize);
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiDestroyDriverInfoList(IntPtr devInfoSet,
        ref SP_DEVINFO_DATA devInfoData, uint driverType);
    // Device-specific driver installation for EXACTLY the supplied device
    // instance (newdev.dll). Per the DiInstallDevice documentation the 4th
    // parameter is a PSP_DRVINFO_DATA -- the driver to install, taken from the
    // device's built driver list -- NOT an INF path string. Passing NULL would
    // make Windows install whatever it deems the best match, which we refuse
    // to assume; instead we verify driver details and pass the exact selected
    // entry. Flags must be zero or DIIDFLAG_* (DI_QUIETINSTALL is NOT valid
    // for this API); we pass 0. A non-null NeedReboot also suppresses the
    // restart dialog. Deliberately replaces the wide-scope
    // UpdateDriverForPlugAndPlayDevices(hwid, INSTALLFLAG_FORCE), which scans
    // ALL devices matching the hardware ID and may force-update newer drivers.
    [DllImport("newdev.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool DiInstallDevice(IntPtr hwndParent, IntPtr deviceInfoSet,
        ref SP_DEVINFO_DATA deviceInfoData, ref SP_DRVINFO_DATA driverInfoData,
        uint flags, out bool needReboot);
    public const uint DIF_REGISTERDEVICE = 0x00000019;
    public const uint DIF_REMOVE = 0x00000002;
    public const uint SPDRP_HARDWAREID = 0x00000001;
    // setupapi.h: DICD_GENERATE_ID 0x00000001 -- "the system generates a
    // device instance ID for the new device" (used with DeviceName = NULL).
    public const uint DICD_GENERATE_ID = 0x00000001;
    // Device name handed to SetupDiCreateDeviceInfo. MUST equal the hardware-ID
    // leaf so the instance ID becomes ROOT\TextDeckHidDriver\000N, which is what
    // the installer's own enumeration/ownership checks look for. Non-NULL and
    // backslash-free are both required (see the note on the P/Invoke).
    public const string RootDeviceName = "TextDeckHidDriver";
    public const uint DIGCF_ALLCLASSES = 0x00000002;
    public const uint SPDIT_CLASSDRIVER = 0x00000001;
    public const uint SPDIT_COMPATDRIVER = 0x00000002;
    // Error codes per the SetupDiGetDeviceRegistryProperty documentation:
    // ERROR_INVALID_DATA (13) is returned BOTH when the requested property
    // does not exist AND when the property data is invalid -- it can therefore
    // NEVER prove "the device has no hardware IDs" (ambiguous => fail closed).
    // ERROR_INSUFFICIENT_BUFFER (122) is the documented outcome of the NULL/0
    // size probe. ERROR_NO_MORE_ITEMS (259) is documented for the Enum* end-
    // of-list, NOT for this property getter; it must not be read as absence.
    private const int ERROR_NO_MORE_ITEMS = 259;        // Enum* end-of-list only
    private const int ERROR_INSUFFICIENT_BUFFER = 122;  // expected size probe
    private const int ERROR_INVALID_DATA = 13;          // absent OR invalid (ambiguous)

    // ---- pure, unit-testable decision logic (no native calls) --------------

    // Classifies the SPDRP_HARDWAREID size probe per the documentation.
    // Returns 1 = proceed to the real buffer read; 0 = FAIL CLOSED (nothing
    // reliable can be determined). Only the documented buffer-size path may
    // proceed: a FALSE call reporting ERROR_INSUFFICIENT_BUFFER with a
    // non-zero size, or a TRUE call that reports a size. Every other probe
    // outcome -- including ERROR_INVALID_DATA, which the docs say means
    // "the property does not exist OR the property data is not valid" -- is
    // indeterminate and must never become a "no match" conclusion.
    public static int ClassifyHwidProbe(bool callSucceeded, int win32Error, uint reportedSize) {
        if (callSucceeded) return reportedSize > 0 ? 1 : 0;
        if (win32Error == ERROR_INSUFFICIENT_BUFFER && reportedSize > 0) return 1;
        return 0;
    }

    // Exact, case-insensitive comparison of two full identifiers.
    // Deliberately NOT a substring test: 'Root\TextDeckHidDriver\0000' or
    // 'Root\TextDeckHidDriver2' must never equal 'Root\TextDeckHidDriver'.
    public static bool HwidEquals(string a, string b) {
        return string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    }

    // True iff any complete NUL-separated identifier in a REG_MULTI_SZ blob
    // equals `needle` exactly (case-insensitively).
    public static bool MultiSzContainsExact(string multiSz, string needle) {
        if (multiSz == null || needle == null) return false;
        string[] parts = multiSz.Split(new char[] { '\0' }, StringSplitOptions.RemoveEmptyEntries);
        for (int i = 0; i < parts.Length; i++) {
            if (HwidEquals(parts[i], needle)) return true;
        }
        return false;
    }

    // ---- driver package identity + unique target selection ------------------
    // Per the SetupGetInfDriverStoreLocation documentation, device install
    // stores TWO copies of a package INF: an OEMnnn.inf published copy in the
    // system INF directory (C:\Windows\INF) and a copy under its ORIGINAL file
    // name in the driver store (DriverStore\FileRepository). Driver-list
    // details report the published INF, so the leaf name alone can NEVER
    // identify our target package. The documented resolution flow is
    // SetupDiGetDriverInfoDetail -> SetupGetInfDriverStoreLocation (PATH
    // mapping only -- the doc explicitly states it does not process file
    // contents) -> the driver store INF whose DIRECTORY holds the package
    // files. Because mapping proves no identity, every candidate store package
    // is then verified by CONTENT: SHA-256 of the INF plus every file the INF
    // itself declares (CatalogFile -> CAT, SourceDisksFiles/ServiceBinary ->
    // SYS and any other binaries), compared against this run's target package.

    // Result of published-INF -> driver store INF path mapping.
    public class MapResult {
        public bool Ok;
        public string StoreInfPath;
        public int Err;
    }
    // Verifiable identity of one driver package on disk (or in a test FS).
    public class PackageIdentity {
        public string Dir;             // package directory (driver store dir or build dir)
        public string InfPath;         // full path of the INF identity was built from
        public string InfHash;         // SHA-256 (hex) of the INF content
        public List<string> KeyFileNames;   // files the INF declares (CAT/SYS/...)
        public List<string> KeyFileHashes;  // parallel SHA-256 (hex) list
        public string Error;           // non-null => identity could NOT be established
    }
    // Per-record audit trail of the selection decision.
    public class CandidateOutcome {
        public int Index;
        public string ReportedInf;   // SP_DRVINFO_DETAIL_DATA.InfFileName as reported
        public string StoreInf;      // resolved driver store INF (null if unresolved)
        public string PackageDir;    // identity-verified package dir (null if rejected)
        public string SectionName;   // DDInstall section reported for this record
        public string Description;   // driver/model description reported for this record
        public bool HwidMatch;
        public bool IdentityMatch;
        public string Reason;
    }
    public class SelectionOutcome {
        public SelectResult Result;
        public List<CandidateOutcome> Outcomes;
    }

    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupGetInfDriverStoreLocation(string fileName,
        IntPtr alternatePlatformInfo, string localeName, StringBuilder returnBuffer,
        uint returnBufferSize, out uint requiredSize);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern bool SetupGetInfPublishedName(string driverStoreLocation,
        StringBuilder returnBuffer, uint returnBufferSize, out uint requiredSize);

    // ---- injectable seams (unit tests override; production uses natives) ----
    public delegate MapResult InfLocationMapper(string candidateInfPath);
    public static InfLocationMapper MapPublishedToStore = NativeMapPublishedToStore;
    public delegate string PublishedNameLookup(string storeInfPath);
    public static PublishedNameLookup LookupPublishedName = NativeLookupPublishedName;
    public delegate bool FileExistsCheck(string path);
    public static FileExistsCheck FileExists = DefaultFileExists;
    public delegate string FileHasher(string path);      // null = missing/unreadable
    public static FileHasher HashFile = DefaultHashFile;
    public delegate string InfTextReader(string path);   // null = missing/unreadable
    public static InfTextReader ReadInfText = DefaultReadInfText;
    public static void ResetSeams() {
        MapPublishedToStore = NativeMapPublishedToStore;
        LookupPublishedName = NativeLookupPublishedName;
        FileExists = DefaultFileExists;
        HashFile = DefaultHashFile;
        ReadInfText = DefaultReadInfText;
    }
    public static bool DefaultFileExists(string path) {
        try { return !string.IsNullOrEmpty(path) && File.Exists(path); } catch { return false; }
    }
    public static string DefaultHashFile(string path) {
        try {
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
            using (FileStream fs = File.OpenRead(path)) {
                using (SHA256 sha = SHA256.Create()) {
                    byte[] hash = sha.ComputeHash(fs);
                    StringBuilder sb = new StringBuilder(hash.Length * 2);
                    for (int i = 0; i < hash.Length; i++) sb.Append(hash[i].ToString("x2"));
                    return sb.ToString();
                }
            }
        } catch { return null; }
    }
    public static string DefaultReadInfText(string path) {
        try {
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
            return File.ReadAllText(path, Encoding.UTF8);
        } catch { return null; }
    }
    // Doc-conformant mapping: FileName = published INF name or full path (or
    // an already-store INF path); AlternatePlatformInfo/LocaleName are
    // reserved => NULL; probe with ReturnBuffer=NULL/size=0 + RequiredSize.
    public static MapResult NativeMapPublishedToStore(string candidateInfPath) {
        MapResult m = new MapResult();
        m.Ok = false; m.StoreInfPath = null; m.Err = 0;
        if (string.IsNullOrEmpty(candidateInfPath)) { m.Err = -1; return m; }
        uint need = 0;
        SetupGetInfDriverStoreLocation(candidateInfPath, IntPtr.Zero, null, null, 0, out need);
        int probeErr = Marshal.GetLastWin32Error();
        if (need == 0) { m.Err = probeErr != 0 ? probeErr : -1; return m; }
        StringBuilder sb = new StringBuilder((int)need);
        if (!SetupGetInfDriverStoreLocation(candidateInfPath, IntPtr.Zero, null, sb, need, out need)) {
            m.Err = Marshal.GetLastWin32Error();
            return m;
        }
        string p = sb.ToString();
        if (string.IsNullOrEmpty(p)) { m.Err = -1; return m; }
        m.Ok = true; m.StoreInfPath = p;
        return m;
    }
    // Reverse mapping (driver store INF -> published INF), same probe pattern.
    public static string NativeLookupPublishedName(string storeInfPath) {
        try {
            if (string.IsNullOrEmpty(storeInfPath)) return null;
            uint need = 0;
            SetupGetInfPublishedName(storeInfPath, null, 0, out need);
            if (need == 0) return null;
            StringBuilder sb = new StringBuilder((int)need);
            if (!SetupGetInfPublishedName(storeInfPath, sb, need, out need)) return null;
            string p = sb.ToString();
            return string.IsNullOrEmpty(p) ? null : p;
        } catch { return null; }
    }

    // ---- INF package inventory parser (deliberately LIMITED, fail-closed) --
    // This is NOT a complete Windows INF parser. It supports exactly the
    // file-reference syntax documented for the constructs below and REJECTS
    // everything outside that subset with a reason -- package identity is
    // never built from a partial read.
    // Supported (WDK INF docs):
    //   * general syntax: case-insensitive sections/entries, merged duplicate
    //     section names, ';' comments outside quotes, "" quote escaping,
    //     '%%' percent escaping, comma fields with omitted optional values,
    //     recursive (bounded) %token% expansion from [Strings];
    //   * [SourceDisksNames[.arch]]  diskid=desc[,tag[,[unused][,path]]]
    //     (path is relative to the installation root, '\dir1\dir2' form);
    //   * [SourceDisksFiles[.arch]]  filename=diskid[,[subdir][,size]]
    //     (subdir is relative to that disk's path; file NAMES must be
    //      literal per docs -- a %token% there is invalid and rejected);
    //   * CopyFiles=@filename and CopyFiles=file-list-section, with
    //     file-list entries destination[,source[,unused[,flag]]] -- the
    //     SOURCE name (2nd field when present) is what the package carries
    //     and therefore what identity covers; DestinationDirs is an
    //     install TARGET and never a source location;
    //   * CatalogFile[.arch] (token expansion supported) and
    //     ServiceBinary=%dirid%\leaf (dirid macro not expanded; the leaf
    //     after the last '\' must be a literal file name).
    // Architecture decorations: '.x86/.ia64/.amd64/.arm/.arm64' only (NOT
    // '.ntamd64' etc. per docs). This 64-bit target USES the generic +
    // '.amd64' sections (amd64 overrides same-name generic entries, per the
    // documented lookup order) and EXCLUDES the other architectures'.
    // Rejected outright (identity insufficient, with reason):
    //   Include/Needs (external INF sections cannot be inventoried),
    //   CopyINF (recursively imports the COMPLETE external driver package(s)
    //   referenced by those INFs into the driver store - the set of files
    //   those packages install is not enumerable from this INF alone),
    //   [Version] LayoutFile, .cab cabinets (files inside are not loose
    //   package content), unknown decorations, drive-absolute/UNC source
    //   paths, '..' traversal, and any unresolvable %token%.
    // CopyFiles follows the documented syntax exactly:
    //   CopyFiles = @filename | file-list-section[,...]  -- @filename and
    //   file-list section names are MUTUALLY EXCLUSIVE alternatives (a lone
    //   @filename is one file); mixed lists, multiple @filename entries and
    //   empty list items are refused; file-list section names are never
    //   platform-decorated (.nt/.ntamd64/...), per the CopyFiles directive
    //   documentation. An EMPTY CopyFiles value carries no file references
    //   and is ignored (same convention as an empty Include/Needs).
    public class InfFileDecl {
        public string LeafName;      // exact file name (the package-root lookup key)
        public string SourceRelPath; // validated media-relative path ('' = root)
        public string Via;           // CatalogFile | SourceDisksFiles | CopyFiles | ServiceBinary
    }
    public class InfSections {
        public Dictionary<string, List<KeyValuePair<string, string>>> ByName;
        public List<string> Order;
        public string Error;
    }
    public class InfModelEntry {
        public string ModelsSection;  // e.g. Standard.NTamd64
        public string SectionBase;    // DDInstall section base name
        public string HardwareId;     // the hardware ID that matched exactly
    }

    // Strips a trailing ';'-comment that is OUTSIDE double quotes.
    public static string StripInfComment(string line) {
        if (line == null) return "";
        bool inQ = false;
        for (int i = 0; i < line.Length; i++) {
            char c = line[i];
            if (c == '"') inQ = !inQ;
            else if (c == ';' && !inQ) return line.Substring(0, i);
        }
        return line;
    }
    // Comma-separated field split honoring double quotes ("\"" escape).
    public static List<string> SplitInfFields(string value) {
        List<string> fields = new List<string>();
        if (value == null) return fields;
        StringBuilder cur = new StringBuilder();
        bool inQ = false;
        for (int i = 0; i < value.Length; i++) {
            char c = value[i];
            if (inQ) {
                if (c == '"') {
                    if (i + 1 < value.Length && value[i + 1] == '"') { cur.Append('"'); i++; }
                    else inQ = false;
                } else cur.Append(c);
            } else {
                if (c == '"') inQ = true;
                else if (c == ',') { fields.Add(cur.ToString().Trim()); cur.Length = 0; }
                else cur.Append(c);
            }
        }
        fields.Add(cur.ToString().Trim());
        return fields;
    }
    // Strips surrounding quotes ONLY when the ENTIRE value is one quoted
    // string ("..." with "" escapes). Mixed/fielded values (e.g.
    // "Disk, One",tag,,"") are returned unchanged -- never guessed at.
    public static string StripWholeQuoted(string v) {
        if (string.IsNullOrEmpty(v)) return v;
        string t = v.Trim();
        if (t.Length < 2 || t[0] != '"') return v;
        int close = -1;
        for (int i = 1; i < t.Length; i++) {
            if (t[i] == '"') {
                if (i + 1 < t.Length && t[i + 1] == '"') { i++; continue; }
                close = i;
                break;
            }
        }
        if (close < 0) return v;                                  // unterminated
        if (t.Substring(close + 1).Trim().Length > 0) return v;    // more fields follow
        return t.Substring(1, close - 1).Replace("\"\"", "\"");
    }
    // Tokenizes INF text into merged sections. Any structural violation
    // (entry before the first section, empty section name/key) is a hard
    // parse error -- the caller must fail closed.
    public static InfSections TokenizeInf(string infText) {
        InfSections t = new InfSections();
        t.ByName = new Dictionary<string, List<KeyValuePair<string, string>>>(StringComparer.OrdinalIgnoreCase);
        t.Order = new List<string>();
        t.Error = null;
        if (infText == null) { t.Error = "INF text is unavailable"; return t; }
        string[] lines = infText.Split(new char[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
        string section = null;
        for (int i = 0; i < lines.Length; i++) {
            string line = StripInfComment(lines[i]).Trim();
            if (line.Length == 0) continue;
            if (line.StartsWith("[") && line.EndsWith("]")) {
                section = line.Substring(1, line.Length - 2).Trim();
                if (section.Length == 0) { t.Error = "empty section name at line " + (i + 1).ToString(); return t; }
                if (!t.ByName.ContainsKey(section)) { t.ByName[section] = new List<KeyValuePair<string, string>>(); t.Order.Add(section); }
                continue;
            }
            if (section == null) { t.Error = "entry outside any section at line " + (i + 1).ToString() + ": '" + line + "'"; return t; }
            int eq = line.IndexOf('=');
            string key = (eq >= 0 ? line.Substring(0, eq) : line).Trim();
            string val = (eq >= 0 ? line.Substring(eq + 1) : "").Trim();
            if (key.Length == 0) { t.Error = "empty entry key at line " + (i + 1).ToString(); return t; }
            if (key.Length >= 2 && key.StartsWith("\"") && key.EndsWith("\"")) key = key.Substring(1, key.Length - 2).Trim();
            t.ByName[section].Add(new KeyValuePair<string, string>(key, val));
        }
        return t;
    }
    // Fails closed when the INF pulls sections from external INFs
    // (Include/Needs), copies other INFs via CopyINF, or uses the
    // system-supplied LayoutFile layout: the identity file set would not be
    // complete. Runs on the TOKENIZED (comment-stripped, quote-normalized,
    // case-insensitive) entries, so section scope, comments, casing and
    // whitespace cannot bypass it.
    public static string RejectExternalInfRefs(InfSections t) {
        foreach (KeyValuePair<string, List<KeyValuePair<string, string>>> kv in t.ByName) {
            for (int i = 0; i < kv.Value.Count; i++) {
                string k = kv.Value[i].Key;
                if ((string.Equals(k, "Include", StringComparison.OrdinalIgnoreCase) ||
                     string.Equals(k, "Needs", StringComparison.OrdinalIgnoreCase)) &&
                    !string.IsNullOrEmpty(kv.Value[i].Value.Trim())) {
                    return "[" + kv.Key + "] uses " + k + "=" + kv.Value[i].Value.Trim() +
                           " - sections from external INFs cannot be inventoried; package identity would be incomplete";
                }
                if (string.Equals(k, "CopyINF", StringComparison.OrdinalIgnoreCase) &&
                    !string.IsNullOrEmpty(kv.Value[i].Value.Trim())) {
                    // CopyINF (docs: INF CopyINF directive) recursively
                    // imports the COMPLETE driver package(s) referenced by
                    // the listed INFs into the driver store. Recursive
                    // package enumeration + path containment + full content
                    // hashing + unique identity over those referenced INFs
                    // is NOT implemented, so a non-empty CopyINF leaves the
                    // identity set provably incomplete -> refuse.
                    return "[" + kv.Key + "] uses CopyINF=" + kv.Value[i].Value.Trim() +
                           " - CopyINF recursively imports the complete external driver package(s) referenced by those INFs" +
                           " (per the INF CopyINF directive); those packages cannot be inventoried, path-constrained or" +
                           " content-hashed from this INF, so package identity would be incomplete";
                }
                if (string.Equals(kv.Key, "Version", StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(k, "LayoutFile", StringComparison.OrdinalIgnoreCase)) {
                    return "[Version] uses LayoutFile - system-supplied INF layout is unsupported for package identity";
                }
            }
        }
        return null;
    }
    public static Dictionary<string, string> CollectInfStrings(InfSections t) {
        Dictionary<string, string> strVars = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        List<KeyValuePair<string, string>> ents;
        if (t.ByName.TryGetValue("Strings", out ents)) {
            for (int i = 0; i < ents.Count; i++) {
                string k = ents[i].Key;
                string v = StripWholeQuoted(ents[i].Value);
                if (k.Length > 0) strVars[k] = v;
            }
        }
        return strVars;
    }
    // '<base>[.<ext>]' applicability for THIS 64-bit target: undecorated
    // always, '.amd64' yes, the other documented architectures no; anything
    // else is unsupported (ok=false + err).
    public static bool SectionApplies(string name, string baseName, out bool ok, out string err) {
        ok = true; err = null;
        if (string.Equals(name, baseName, StringComparison.OrdinalIgnoreCase)) return true;
        string prefix = baseName + ".";
        if (!name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return false;
        string ext = name.Substring(prefix.Length);
        if (string.Equals(ext, "amd64", StringComparison.OrdinalIgnoreCase)) return true;
        if (string.Equals(ext, "x86", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(ext, "ia64", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(ext, "arm", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(ext, "arm64", StringComparison.OrdinalIgnoreCase)) return false;
        ok = false;
        err = "unsupported decoration '." + ext + "' on [" + name + "] (supported here: undecorated and .amd64; other-architecture sections are excluded)";
        return false;
    }
    // Per-disk-id lookup across the two APPLICABLE SourceDisksNames layers:
    // the architecture-specific definition wins over the generic one
    // (documented SetupAPI lookup order); a miss in both means the disk id is
    // not defined for this target. The layers are stored separately so a
    // legal '.amd64' override never collides with a generic entry, and
    // non-target-architecture sections never contribute (they are excluded
    // upstream by SectionApplies).
    public static bool TryLookupDiskPath(Dictionary<string, string> genericPaths, Dictionary<string, string> archPaths, string diskid, out string path, out bool usedArch) {
        path = null; usedArch = false;
        if (archPaths != null && archPaths.TryGetValue(diskid, out path)) { usedArch = true; return true; }
        if (genericPaths != null && genericPaths.TryGetValue(diskid, out path)) return true;
        return false;
    }
    // The CopyFiles directive does NOT support decorating a file-list-section
    // name with a system-defined platform extension (.nt/.ntx86/.ntia64/
    // .ntamd64/.ntarm/.ntarm64) - exact suffix match on a leading-dot token so
    // ordinary section names ending in letters can never false-positive.
    public static bool HasSectionDecoration(string name) {
        if (string.IsNullOrEmpty(name)) return false;
        string[] decorations = new string[] { ".nt", ".ntx86", ".ntia64", ".ntamd64", ".ntarm", ".ntarm64" };
        for (int i = 0; i < decorations.Length; i++) {
            if (name.EndsWith(decorations[i], StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }
    // A file NAME per the INF docs is a literal leaf name: no %token% (the
    // docs forbid tokens in file names), no path characters, never '.'/'..'.
    public static bool ValidateLeafName(string s, out string err) {
        err = null;
        if (string.IsNullOrEmpty(s)) { err = "empty file name"; return false; }
        if (s.IndexOf('%') >= 0) { err = "'" + s + "' contains a % token (file names must be literal per INF docs)"; return false; }
        if (s.IndexOfAny(new char[] { '\\', '/', ':' }) >= 0) { err = "'" + s + "' is not a plain file name (path characters present)"; return false; }
        if (s == "." || s == "..") { err = "'" + s + "' is not a file name"; return false; }
        if (s.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) { err = "'" + s + "' contains invalid file name characters"; return false; }
        return true;
    }
    // Validates a media-relative source path ('\dir1\dir2' per docs). Rejects
    // traversal, drive-absolute and UNC forms: such a path could point
    // outside the package and is never trusted for identity.
    public static bool ValidateSourceRelPath(string p, out string err) {
        err = null;
        if (string.IsNullOrEmpty(p)) return true;
        string t = p.Trim();
        if (t.Length == 0) return true;
        int lead = 0;
        while (lead < t.Length && t[lead] == '\\') lead++;
        if (lead >= 2) { err = "UNC source path rejected: '" + p + "'"; return false; }
        string body = t.Substring(lead);
        if (body.Length == 0) return true;
        if (body.Length >= 2 && body[1] == ':') { err = "drive-absolute source path rejected: '" + p + "'"; return false; }
        if (body.IndexOf(':') >= 0) { err = "source path contains ':': '" + p + "'"; return false; }
        string[] segs = body.Split(new char[] { '\\' }, StringSplitOptions.RemoveEmptyEntries);
        for (int i = 0; i < segs.Length; i++) {
            string s = segs[i];
            if (s == ".") { err = "source path contains '.' segment: '" + p + "'"; return false; }
            if (s == "..") { err = "source path traversal ('..') rejected: '" + p + "'"; return false; }
            if (s.IndexOfAny(new char[] { '*', '?', '<', '>', '|', '"' }) >= 0) { err = "source path contains invalid characters: '" + p + "'"; return false; }
        }
        return true;
    }
    public static string NormalizeRel(string p) {
        if (string.IsNullOrEmpty(p)) return "";
        return p.Trim().Trim('\\');
    }
    public static string CombineRel(string a, string b) {
        string x = NormalizeRel(a); string y = NormalizeRel(b);
        if (x.Length == 0) return y;
        if (y.Length == 0) return x;
        return x + "\\" + y;
    }
    // Expands %token% references using [Strings] values (bounded depth).
    // '%%' is an escaped literal '%' per the general syntax rules; any other
    // unresolved % stays literal and callers must reject leftovers.
    public static string ExpandInfVar(string v, Dictionary<string, string> vars) {
        if (v == null) return null;
        string cur = v.Replace("%%", "\u0001");
        for (int n = 0; n < 8 && cur.IndexOf('%') >= 0; n++) {
            int a = cur.IndexOf('%');
            int b = cur.IndexOf('%', a + 1);
            if (b < 0) break;
            string name = cur.Substring(a + 1, b - a - 1);
            string rep;
            if (name.Length > 0 && vars.TryGetValue(name, out rep) && rep != null) {
                cur = cur.Substring(0, a) + rep + cur.Substring(b + 1);
            } else {
                break;   // unresolved token stays literal (caller fail-closes)
            }
        }
        return cur.Replace("\u0001", "%");
    }

    // Builds the declared package-file inventory (see the parser contract
    // above). parseError != null => the inventory could NOT be established.
    public static List<InfFileDecl> ParseInfInventory(string infText, out string parseError) {
        parseError = null;
        List<InfFileDecl> files = new List<InfFileDecl>();
        InfSections tok = TokenizeInf(infText);
        if (tok.Error != null) { parseError = tok.Error; return files; }
        string ext = RejectExternalInfRefs(tok);
        if (ext != null) { parseError = ext; return files; }
        Dictionary<string, string> strVars = CollectInfStrings(tok);
        Dictionary<string, InfFileDecl> byLeaf = new Dictionary<string, InfFileDecl>(StringComparer.OrdinalIgnoreCase);
        Dictionary<string, string> leafSection = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        // -- [SourceDisksNames[.arch]] : diskid -> media-relative path ------
        string secErr; bool secOk;
        List<string> sdnSecs = new List<string>();
        List<string> sdfSecs = new List<string>();
        foreach (string s in tok.Order) {
            bool applies = SectionApplies(s, "SourceDisksNames", out secOk, out secErr);
            if (!secOk) { parseError = secErr; return files; }
            if (applies) sdnSecs.Add(s);
            applies = SectionApplies(s, "SourceDisksFiles", out secOk, out secErr);
            if (!secOk) { parseError = secErr; return files; }
            if (applies) sdfSecs.Add(s);
        }
        // documented SetupAPI order: look for a disk id in the architecture-
        // specific section BEFORE the generic section, PER DISK ID -- a disk
        // id defined in '.amd64' uses that definition (legal override, not a
        // conflict); a disk id only defined in the generic section falls back
        // to it; a disk id defined ONLY in a non-target-architecture section
        // (e.g. '.x86') is not defined for this target. Conflicting duplicates
        // WITHIN one applicable section are ambiguous -> refused.
        List<string> orderedSdn = new List<string>();
        foreach (string s in sdnSecs) if (string.Equals(s, "SourceDisksNames", StringComparison.OrdinalIgnoreCase)) orderedSdn.Add(s);
        foreach (string s in sdnSecs) if (!string.Equals(s, "SourceDisksNames", StringComparison.OrdinalIgnoreCase)) orderedSdn.Add(s);
        Dictionary<string, string> genericPaths = new Dictionary<string, string>(StringComparer.Ordinal);
        Dictionary<string, string> archPaths = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (string s in orderedSdn) {
            bool isArchSection = !string.Equals(s, "SourceDisksNames", StringComparison.OrdinalIgnoreCase);
            Dictionary<string, string> layer = isArchSection ? archPaths : genericPaths;
            List<KeyValuePair<string, string>> ents = tok.ByName[s];
            for (int i = 0; i < ents.Count; i++) {
                string diskid = ents[i].Key.Trim();
                int didNum;
                if (!int.TryParse(diskid, out didNum) || didNum < 0) { parseError = "[" + s + "] entry key '" + ents[i].Key + "' is not a valid disk id"; return files; }
                // split FIRST, expand per field AFTER: a [Strings] value may
                // itself contain commas and must not shift field positions
                List<string> f = SplitInfFields(ents[i].Value.Trim());
                // diskid = desc[,tag[,[unused][,path]]]  -> path = 4th field
                string cab = f.Count > 1 ? ExpandInfVar(f[1].Trim(), strVars).Trim() : "";
                if (cab.EndsWith(".cab", StringComparison.OrdinalIgnoreCase)) {
                    parseError = "[" + s + "] disk " + diskid + " specifies cabinet '" + cab + "' - files inside cabinets cannot be identity-verified";
                    return files;
                }
                string pathVal = f.Count > 3 ? ExpandInfVar(f[3].Trim(), strVars).Trim() : "";
                if (pathVal.IndexOf('%') >= 0) { parseError = "[" + s + "] disk " + diskid + " path contains an unresolvable token: '" + pathVal + "'"; return files; }
                string perr2;
                if (!ValidateSourceRelPath(pathVal, out perr2)) { parseError = "[" + s + "] disk " + diskid + ": " + perr2; return files; }
                string norm = NormalizeRel(pathVal);
                string prev;
                if (layer.TryGetValue(diskid, out prev) && !HwidEquals(prev, norm)) {
                    parseError = "[" + s + "] disk " + diskid + " defined twice in the same section with different paths ('" + prev + "' vs '" + norm + "')";
                    return files;
                }
                layer[diskid] = norm;
            }
        }
        // -- [SourceDisksFiles[.arch]] : filename=diskid[,[subdir][,size]] ---
        List<string> orderedSdf = new List<string>();
        foreach (string s in sdfSecs) if (string.Equals(s, "SourceDisksFiles", StringComparison.OrdinalIgnoreCase)) orderedSdf.Add(s);
        foreach (string s in sdfSecs) if (!string.Equals(s, "SourceDisksFiles", StringComparison.OrdinalIgnoreCase)) orderedSdf.Add(s);
        foreach (string s in orderedSdf) {
            List<KeyValuePair<string, string>> ents = tok.ByName[s];
            for (int i = 0; i < ents.Count; i++) {
                string leaf = ents[i].Key.Trim();
                string lerr;
                if (!ValidateLeafName(leaf, out lerr)) { parseError = "[" + s + "]: " + lerr; return files; }
                List<string> f = SplitInfFields(ents[i].Value.Trim());
                string diskid = f.Count > 0 ? f[0].Trim() : "";
                string subdir = f.Count > 1 ? ExpandInfVar(f[1].Trim(), strVars).Trim() : "";
                if (diskid.Length == 0) { parseError = "[" + s + "] '" + leaf + "' has no disk id"; return files; }
                if (subdir.IndexOf('%') >= 0) { parseError = "[" + s + "] '" + leaf + "' subdir contains an unresolvable token: '" + subdir + "'"; return files; }
                string sperr;
                if (!ValidateSourceRelPath(subdir, out sperr)) { parseError = "[" + s + "] '" + leaf + "': " + sperr; return files; }
                string dpath; bool diskFromArch;
                if (!TryLookupDiskPath(genericPaths, archPaths, diskid, out dpath, out diskFromArch)) {
                    parseError = "[" + s + "] '" + leaf + "' references disk id " + diskid + " which is not defined in [SourceDisksNames]";
                    return files;
                }
                string rel = CombineRel(dpath, subdir);
                InfFileDecl prev;
                string prevSec;
                if (byLeaf.TryGetValue(leaf, out prev)) {
                    leafSection.TryGetValue(leaf, out prevSec);
                    bool sameSection = string.Equals(prevSec, s, StringComparison.OrdinalIgnoreCase);
                    if (sameSection && !HwidEquals(prev.SourceRelPath, rel)) {
                        parseError = "[" + s + "] '" + leaf + "' defined twice with different source paths ('" + prev.SourceRelPath + "' vs '" + rel + "')";
                        return files;
                    }
                    // documented precedence: the architecture-specific entry
                    // replaces the generic one for the same file name
                    prev.SourceRelPath = rel;
                    prev.Via = "SourceDisksFiles";
                    leafSection[leaf] = s;
                    continue;
                }
                InfFileDecl dcl = new InfFileDecl();
                dcl.LeafName = leaf; dcl.SourceRelPath = rel; dcl.Via = "SourceDisksFiles";
                byLeaf[leaf] = dcl; leafSection[leaf] = s; files.Add(dcl);
            }
        }
        // -- CopyFiles directives (in any section) ---------------------------
        foreach (string s in tok.Order) {
            List<KeyValuePair<string, string>> ents = tok.ByName[s];
            for (int i = 0; i < ents.Count; i++) {
                if (!string.Equals(ents[i].Key, "CopyFiles", StringComparison.OrdinalIgnoreCase)) continue;
                string cv = ExpandInfVar(ents[i].Value.Trim(), strVars);
                if (cv.IndexOf('%') >= 0) { parseError = "[" + s + "] CopyFiles contains an unresolvable token: '" + cv + "'"; return files; }
                // documented syntax (docs: INF CopyFiles directive):
                //   CopyFiles = @filename | file-list-section[,...]
                // @filename and file-list section names are MUTUALLY EXCLUSIVE
                // alternatives, and @filename names ONE file. An EMPTY value
                // carries no references and is ignored (same convention as an
                // empty Include/Needs); otherwise an EMPTY list item, a MIXED
                // @/section list, multiple @filename entries or a platform-
                // decorated section name is refused as invalid syntax.
                if (cv.Trim().Length == 0) continue;
                List<string> refs = SplitInfFields(cv);
                int atCount = 0; int secCount = 0;
                for (int r = 0; r < refs.Count; r++) {
                    string rf = refs[r].Trim();
                    if (rf.Length == 0) { parseError = "[" + s + "] CopyFiles list '" + cv + "' contains an empty item - not valid CopyFiles syntax"; return files; }
                    if (rf.StartsWith("@")) atCount++; else secCount++;
                }
                if (atCount > 0 && secCount > 0) {
                    parseError = "[" + s + "] CopyFiles value '" + cv + "' mixes @filename with file-list section names - not valid CopyFiles syntax (documented form: @filename | file-list-section[,...])";
                    return files;
                }
                if (atCount > 1) {
                    parseError = "[" + s + "] CopyFiles value '" + cv + "' lists multiple @filename entries - not valid CopyFiles syntax (only a single @filename is documented)";
                    return files;
                }
                for (int r = 0; r < refs.Count; r++) {
                    string rf = refs[r].Trim();
                    if (rf.StartsWith("@")) {
                        string leaf = rf.Substring(1).Trim();
                        string lerr;
                        if (!ValidateLeafName(leaf, out lerr)) { parseError = "[" + s + "] CopyFiles: " + lerr; return files; }
                        if (!byLeaf.ContainsKey(leaf)) {
                            InfFileDecl dcl = new InfFileDecl();
                            dcl.LeafName = leaf; dcl.SourceRelPath = ""; dcl.Via = "CopyFiles";
                            byLeaf[leaf] = dcl; leafSection[leaf] = s; files.Add(dcl);
                        }
                    } else {
                        if (HasSectionDecoration(rf)) {
                            parseError = "[" + s + "] CopyFiles references file-list section '" + rf + "' with a platform extension - the CopyFiles directive does not support decorating a file-list-section name";
                            return files;
                        }
                        List<KeyValuePair<string, string>> fl;
                        if (!tok.ByName.TryGetValue(rf, out fl)) { parseError = "[" + s + "] CopyFiles references missing file-list section '" + rf + "'"; return files; }
                        for (int j = 0; j < fl.Count; j++) {
                            if (fl[j].Value.Trim().Length > 0) { parseError = "[" + rf + "] entry '" + fl[j].Key + "' uses '=' which is not valid in a file-list section"; return files; }
                            List<string> ef = SplitInfFields(fl[j].Key);
                            string dest = ef[0].Trim();
                            string lerr;
                            if (!ValidateLeafName(dest, out lerr)) { parseError = "[" + rf + "]: " + lerr; return files; }
                            // identity covers the SOURCE file the package
                            // carries; destination name may legitimately differ
                            string src = (ef.Count > 1 && ef[1].Trim().Length > 0) ? ef[1].Trim() : dest;
                            if (!ValidateLeafName(src, out lerr)) { parseError = "[" + rf + "] source name for '" + dest + "': " + lerr; return files; }
                            if (!byLeaf.ContainsKey(src)) {
                                InfFileDecl dcl = new InfFileDecl();
                                dcl.LeafName = src; dcl.SourceRelPath = ""; dcl.Via = "CopyFiles";
                                byLeaf[src] = dcl; leafSection[src] = rf; files.Add(dcl);
                            }
                        }
                    }
                }
            }
        }
        // -- CatalogFile[.arch] + ServiceBinary ------------------------------
        bool sawCatalog = false;
        foreach (string s in tok.Order) {
            List<KeyValuePair<string, string>> ents = tok.ByName[s];
            for (int i = 0; i < ents.Count; i++) {
                string k = ents[i].Key;
                if (k.StartsWith("CatalogFile", StringComparison.OrdinalIgnoreCase)) {
                    string restOfKey = k.Substring("CatalogFile".Length);
                    if (restOfKey.Length > 0 && !restOfKey.StartsWith(".")) continue;  // not a catalog directive
                    sawCatalog = true;
                    string cval = ExpandInfVar(StripWholeQuoted(ents[i].Value), strVars);
                    if (cval.IndexOf('%') >= 0) { parseError = "CatalogFile name contains an unresolvable token: '" + ents[i].Value.Trim() + "'"; return files; }
                    string lerr;
                    if (!ValidateLeafName(cval, out lerr)) { parseError = "CatalogFile: " + lerr; return files; }
                    if (!byLeaf.ContainsKey(cval)) {
                        InfFileDecl dcl = new InfFileDecl();
                        dcl.LeafName = cval; dcl.SourceRelPath = ""; dcl.Via = "CatalogFile";
                        byLeaf[cval] = dcl; leafSection[cval] = s; files.Add(dcl);
                    }
                } else if (string.Equals(k, "ServiceBinary", StringComparison.OrdinalIgnoreCase)) {
                    string sbv = ExpandInfVar(StripWholeQuoted(ents[i].Value), strVars);
                    int bs = sbv.LastIndexOf('\\');
                    string leaf = (bs >= 0) ? sbv.Substring(bs + 1).Trim() : sbv.Trim();
                    string lerr;
                    if (!ValidateLeafName(leaf, out lerr)) { parseError = "ServiceBinary: " + lerr; return files; }
                    if (!byLeaf.ContainsKey(leaf)) {
                        InfFileDecl dcl = new InfFileDecl();
                        dcl.LeafName = leaf; dcl.SourceRelPath = ""; dcl.Via = "ServiceBinary";
                        byLeaf[leaf] = dcl; leafSection[leaf] = s; files.Add(dcl);
                    }
                }
            }
        }
        if (!sawCatalog) { parseError = "INF declares no CatalogFile directive - cannot bind the CAT into the package identity"; return files; }
        if (files.Count == 0) { parseError = "INF declares no package files (CatalogFile/SourceDisksFiles/CopyFiles/ServiceBinary)"; return files; }
        return files;
    }

    // Audit helper: THIS INF's [Manufacturer]+models entries that target
    // `hwid` exactly, with their DDInstall section base names. Supported
    // subset: %mfg%=<section-declaration>[,<decoration>] with decorations
    // none/NT/NTamd64 (most specific wins), models entries
    // %desc%=<SectionBase>,<hwid>[,<hwid>...]. Anything else (external refs,
    // other decorations, missing sections/IDs, unresolvable tokens) is a
    // parse error -- the audit must fail closed rather than guess.
    public static List<InfModelEntry> ParseInfModelsForHwid(string infText, string hwid, out string parseError) {
        parseError = null;
        List<InfModelEntry> models = new List<InfModelEntry>();
        InfSections tok = TokenizeInf(infText);
        if (tok.Error != null) { parseError = tok.Error; return models; }
        string ext = RejectExternalInfRefs(tok);
        if (ext != null) { parseError = ext; return models; }
        Dictionary<string, string> strVars = CollectInfStrings(tok);
        List<KeyValuePair<string, string>> mfg;
        if (!tok.ByName.TryGetValue("Manufacturer", out mfg) || mfg.Count == 0) { parseError = "[Manufacturer] section is missing or empty"; return models; }
        List<string> cand = new List<string>();   // section names, best rank last
        int bestRank = -1;
        for (int i = 0; i < mfg.Count; i++) {
            List<string> f = SplitInfFields(mfg[i].Value.Trim());   // split, then expand per field
            if (f.Count == 0 || f[0].Trim().Length == 0) { parseError = "[Manufacturer] entry '" + mfg[i].Key + "' has no models section name"; return models; }
            string baseSec = ExpandInfVar(f[0].Trim(), strVars).Trim();
            if (baseSec.IndexOf('%') >= 0) { parseError = "[Manufacturer] entry '" + mfg[i].Key + "' models section name contains an unresolvable token: '" + baseSec + "'"; return models; }
            int rank = 0;                         // 0 = undecorated (all arches)
            string chosen = baseSec;
            for (int j = 1; j < f.Count; j++) {
                string dec = ExpandInfVar(f[j].Trim(), strVars).Trim();
                if (dec.Length == 0) continue;
                if (string.Equals(dec, "NTamd64", StringComparison.OrdinalIgnoreCase)) { if (1 > rank) { rank = 1; chosen = baseSec + "." + dec; } }
                else if (string.Equals(dec, "NT", StringComparison.OrdinalIgnoreCase)) { if (0 > rank) { rank = 0; chosen = baseSec + "." + dec; } }
                else { parseError = "[Manufacturer] unsupported decoration '" + dec + "' (supported here: none, NT, NTamd64)"; return models; }
            }
            if (rank > bestRank) { bestRank = rank; cand.Clear(); cand.Add(chosen); }
            else if (rank == bestRank) cand.Add(chosen);
        }
        for (int c = 0; c < cand.Count; c++) {
            string ms = cand[c];
            List<KeyValuePair<string, string>> ents;
            if (!tok.ByName.TryGetValue(ms, out ents)) { parseError = "[Manufacturer] references missing models section '" + ms + "'"; return models; }
            for (int i = 0; i < ents.Count; i++) {
                List<string> f = SplitInfFields(ents[i].Value.Trim());   // split, then expand per field
                string baseName = f.Count > 0 ? ExpandInfVar(f[0].Trim(), strVars).Trim() : "";
                if (baseName.Length == 0) { parseError = "[" + ms + "] entry '" + ents[i].Key + "' has no install section name"; return models; }
                if (baseName.IndexOf('%') >= 0) { parseError = "[" + ms + "] entry '" + ents[i].Key + "' install section name contains an unresolvable token: '" + baseName + "'"; return models; }
                if (f.Count < 2) { parseError = "[" + ms + "] entry '" + ents[i].Key + "' has no hardware ID"; return models; }
                for (int j = 1; j < f.Count; j++) {
                    string id = ExpandInfVar(f[j].Trim(), strVars).Trim();
                    if (id.Length == 0) continue;
                    if (id.IndexOf('%') >= 0) { parseError = "[" + ms + "] entry '" + ents[i].Key + "' hardware ID contains an unresolvable token: '" + id + "'"; return models; }
                    if (HwidEquals(id, hwid)) {
                        InfModelEntry me = new InfModelEntry();
                        me.ModelsSection = ms; me.SectionBase = baseName; me.HardwareId = id;
                        models.Add(me);
                    }
                }
            }
        }
        return models;
    }
    private static int IndexOfIgnoreCase(List<string> list, string v) {
        for (int i = 0; i < list.Count; i++) if (HwidEquals(list[i], v)) return i;
        return -1;
    }
    private static string ShortHash(string h) {
        if (string.IsNullOrEmpty(h)) return "(none)";
        return h.Length <= 12 ? h : h.Substring(0, 12);
    }

    // Locates one declared file INSIDE one package directory, without ever
    // leaving it. The INF's SourceDisksNames/SourceDisksFiles values describe
    // the original DISTRIBUTION-MEDIA layout, which need not equal either the
    // build-output layout or the Driver Store FileRepository layout (both are
    // flat in practice). Lookup order is therefore: the flat package root
    // first (Driver Store / build-output layout), then the composed
    // media-relative path under that root (original-layout packages). Every
    // candidate is normalized and proven to stay under the root first.
    private static bool TryResolvePackageFile(string root, InfFileDecl d, out string path, out string tried) {
        path = null;
        StringBuilder tr = new StringBuilder();
        List<string> cands = new List<string>();
        cands.Add(Path.Combine(root, d.LeafName));
        string rel = NormalizeRel(d.SourceRelPath);
        if (rel.Length > 0 && !HwidEquals(rel, d.LeafName)) cands.Add(Path.Combine(root, rel, d.LeafName));
        for (int i = 0; i < cands.Count; i++) {
            string full;
            if (!TryGetFullPathInside(root, cands[i], out full)) {
                if (tr.Length > 0) tr.Append(", ");
                tr.Append(cands[i]).Append(" [escapes package root]");
                continue;
            }
            if (tr.Length > 0) tr.Append(", ");
            tr.Append(full);
            if (FileExists(full)) { path = full; tried = tr.ToString(); return true; }
        }
        tried = tr.ToString();
        if (tried.Length == 0) tried = "(no candidate path)";
        return false;
    }
    // Normalizes candidate + root and verifies the candidate is strictly
    // INSIDE the root. Any invalid path or escape attempt => false.
    private static bool TryGetFullPathInside(string root, string p, out string full) {
        full = null;
        try {
            string r = Path.GetFullPath(root).TrimEnd('\\', '/');
            string f = Path.GetFullPath(p);
            if (string.Equals(f, r, StringComparison.OrdinalIgnoreCase)) return false;
            if (!f.StartsWith(r + "\\", StringComparison.OrdinalIgnoreCase)) return false;
            full = f;
            return true;
        } catch { return false; }
    }

    // Builds the content identity of the package whose INF is at infPath.
    // Identity = SHA-256(INF) + SHA-256 of every file the INF declares via
    // CatalogFile / SourceDisksFiles / CopyFiles (source names) /
    // ServiceBinary (inventory contract: ParseInfInventory). The declared set
    // is sufficient to separate packages because a driver package installs
    // exactly what its INF declares: same INF text but a different SYS/CAT
    // yields a different hash set, and different INF text yields a different
    // INF hash. Any parse error, missing/unreadable file, or unresolved path
    // => Error set (identity insufficient; never a partial hash set).
    public static PackageIdentity BuildIdentity(string infPath) {
        PackageIdentity id = new PackageIdentity();
        id.Error = null;
        id.KeyFileNames = new List<string>();
        id.KeyFileHashes = new List<string>();
        id.InfPath = infPath;
        try {
            if (string.IsNullOrEmpty(infPath)) { id.Error = "INF path is empty"; return id; }
            if (!FileExists(infPath)) { id.Error = "INF not found: " + infPath; return id; }
            string dir = Path.GetDirectoryName(infPath);
            if (string.IsNullOrEmpty(dir)) { id.Error = "INF path has no directory: " + infPath; return id; }
            id.Dir = dir;
            string text = ReadInfText(infPath);
            if (text == null) { id.Error = "INF could not be read: " + infPath; return id; }
            id.InfHash = HashFile(infPath);
            if (string.IsNullOrEmpty(id.InfHash)) { id.Error = "INF hash could not be computed: " + infPath; return id; }
            string perr;
            List<InfFileDecl> decls = ParseInfInventory(text, out perr);
            if (perr != null) { id.Error = perr; return id; }
            if (decls.Count == 0) { id.Error = "INF declares no package files (CatalogFile/SourceDisksFiles/CopyFiles) - identity insufficient"; return id; }
            for (int i = 0; i < decls.Count; i++) {
                InfFileDecl d = decls[i];
                string fp, tried;
                if (!TryResolvePackageFile(dir, d, out fp, out tried)) {
                    id.Error = "declared package file missing: " + d.LeafName + " (looked at " + tried + ")";
                    return id;
                }
                string h = HashFile(fp);
                if (string.IsNullOrEmpty(h)) { id.Error = "declared package file hash failed: " + fp; return id; }
                id.KeyFileNames.Add(d.LeafName);
                id.KeyFileHashes.Add(h);
            }
        } catch (Exception ex) {
            id.Error = "identity build exception: " + ex.Message;
        }
        return id;
    }

    // Pure content-identity comparison: both identities must be complete, the
    // INF hashes must be equal, and every file the TARGET declares must exist
    // in the candidate with an equal hash. (The candidate's own declaration
    // set is derived from its INF, whose hash already had to match, so the
    // declared sets are necessarily the same.)
    public static bool IdentityMatches(PackageIdentity target, PackageIdentity cand, out string reason) {
        reason = null;
        if (target == null || target.Error != null) { reason = "target identity unavailable: " + (target == null ? "null" : target.Error); return false; }
        if (cand == null || cand.Error != null) { reason = "candidate identity unavailable: " + (cand == null ? "null" : cand.Error); return false; }
        if (!HwidEquals(target.InfHash, cand.InfHash)) {
            reason = "INF content hash differs (candidate " + ShortHash(cand.InfHash) + " vs target " + ShortHash(target.InfHash) + ")";
            return false;
        }
        for (int i = 0; i < target.KeyFileNames.Count; i++) {
            int j = IndexOfIgnoreCase(cand.KeyFileNames, target.KeyFileNames[i]);
            if (j < 0) { reason = "candidate package missing declared file: " + target.KeyFileNames[i]; return false; }
            if (!HwidEquals(target.KeyFileHashes[i], cand.KeyFileHashes[j])) {
                reason = "content hash differs for declared file: " + target.KeyFileNames[i];
                return false;
            }
        }
        return true;
    }

    // Unique target-driver selection, identity-verified. Per record:
    //   (1) hardware ID must equal ours EXACTLY (full identifier, case-insens.),
    //   (2) published INF -> driver store INF via SetupGetInfDriverStoreLocation
    //       (path resolution ONLY; a failure or unresolvable path rejects it),
    //   (3) published-name round-trip cross-check via SetupGetInfPublishedName
    //       when the detail reported a published oemNN.inf name,
    //   (4) driver store package content identity vs the target package,
    //   (5) install-section semantics: identity only proves that records point
    //       at ONE package -- it does NOT prove they install the same way.
    //       Records are interchangeable only when EVERY verified record for
    //       that package reports the same NON-EMPTY SectionName (the DDInstall
    //       section SetupAPI reports is the documented carrier of install
    //       semantics). Multiple verified entries with DIFFERENT sections, or
    //       with a missing section name => Code=3 ambiguity (refuse; no
    //       first-entry preference is ever invented).
    // Distinct matching package directories => ambiguous. Zero verified
    // packages -> Code=2 (never fall back to "best match" / NULL record).
    public static SelectionOutcome SelectTargetDriver(List<DrvDetail> records, PackageIdentity target, string hwid) {
        SelectionOutcome so = new SelectionOutcome();
        so.Outcomes = new List<CandidateOutcome>();
        SelectResult r = new SelectResult();
        r.Code = 2; r.Index = -1; r.Message = "";
        so.Result = r;
        if (records == null) records = new List<DrvDetail>();
        List<string> matchDirKeys = new List<string>();
        List<int> matchIdx = new List<int>();
        List<string> matchDirs = new List<string>();
        List<int> allMatchIdx = new List<int>();   // every identity-verified record
        List<string> allMatchDirs = new List<string>();
        for (int i = 0; i < records.Count; i++) {
            DrvDetail d = records[i];
            CandidateOutcome o = new CandidateOutcome();
            o.Index = i;
            o.ReportedInf = (d == null) ? null : d.InfFileName;
            so.Outcomes.Add(o);
            if (d == null) { o.Reason = "driver record is null"; continue; }
            o.SectionName = d.SectionName;
            o.Description = d.Description;
            o.HwidMatch = HwidEquals(d.HardwareId, hwid);
            if (!o.HwidMatch) { o.Reason = "hardware ID differs from the target device"; continue; }
            if (string.IsNullOrEmpty(d.InfFileName)) { o.Reason = "driver detail reported no INF name"; continue; }
            MapResult m = MapPublishedToStore(d.InfFileName);
            if (m == null || !m.Ok || string.IsNullOrEmpty(m.StoreInfPath)) {
                int e = (m == null) ? -1 : m.Err;
                o.Reason = "SetupGetInfDriverStoreLocation failed for '" + d.InfFileName + "' (error " + e.ToString() + ")";
                continue;
            }
            o.StoreInf = m.StoreInfPath;
            // Published-name round trip (oemNN.inf form only; the "oem*.inf"
            // test routes this cross-check, it is not the identity proof).
            string leaf = Path.GetFileName(d.InfFileName);
            if (leaf != null && leaf.StartsWith("oem", StringComparison.OrdinalIgnoreCase) &&
                leaf.EndsWith(".inf", StringComparison.OrdinalIgnoreCase)) {
                string pub = LookupPublishedName(m.StoreInfPath);
                if (string.IsNullOrEmpty(pub)) {
                    o.Reason = "SetupGetInfPublishedName reverse lookup failed for '" + m.StoreInfPath + "'";
                    continue;
                }
                if (!HwidEquals(Path.GetFileName(pub), leaf)) {
                    o.Reason = "published-name round-trip mismatch: store INF maps back to '" + pub + "', not '" + d.InfFileName + "'";
                    continue;
                }
            }
            PackageIdentity cand = BuildIdentity(m.StoreInfPath);
            string why;
            if (!IdentityMatches(target, cand, out why)) {
                o.Reason = "package identity rejected: " + why;
                continue;
            }
            o.IdentityMatch = true;
            o.PackageDir = cand.Dir;
            allMatchIdx.Add(i);
            allMatchDirs.Add(cand.Dir);
            string dirKey = cand.Dir.ToLowerInvariant();
            bool seen = false;
            for (int k = 0; k < matchDirKeys.Count; k++) {
                if (string.Equals(matchDirKeys[k], dirKey, StringComparison.Ordinal)) { seen = true; break; }
            }
            if (!seen) { matchDirKeys.Add(dirKey); matchIdx.Add(i); matchDirs.Add(cand.Dir); }
        }
        if (allMatchIdx.Count == 0) {
            r.Code = 2;
            StringBuilder sb = new StringBuilder();
            sb.Append("no compatible driver entry matches the target package by content identity (");
            sb.Append(records.Count.ToString()).Append(" entries): ");
            for (int i = 0; i < so.Outcomes.Count; i++) {
                if (i > 0) sb.Append("; ");
                sb.Append("[").Append(i.ToString()).Append("] ")
                 .Append(so.Outcomes[i].Reason == null ? "no reason" : so.Outcomes[i].Reason);
            }
            sb.Append(" - refusing to install an unverified driver");
            r.Message = sb.ToString();
        } else if (matchIdx.Count > 1) {
            r.Code = 3;
            r.Message = "ambiguous: " + matchIdx.Count.ToString() +
                        " distinct driver store packages match the target identity - refusing to guess";
        } else {
            // Exactly ONE identity-verified package, but it may be referenced
            // by MULTIPLE driver records. Same package + same hardware ID does
            // NOT imply the same install semantics: separate model entries in
            // one INF can name DIFFERENT DDInstall sections (different
            // services/registry/coinstallers => different install effects).
            // Records may be de-duplicated ONLY when EVERY verified record
            // reports the same non-empty install section (the documented
            // carrier of install semantics). Divergent or missing section
            // names => ambiguity; refuse -- never pick the first entry.
            string chosenDir = matchDirs[0];
            List<string> distinct = new List<string>();
            List<List<int>> members = new List<List<int>>();
            List<int> noSection = new List<int>();
            for (int n = 0; n < allMatchIdx.Count; n++) {
                int idx = allMatchIdx[n];
                DrvDetail d = records[idx];
                string sec = (d == null || d.SectionName == null) ? "" : d.SectionName.Trim();
                if (sec.Length == 0) { noSection.Add(idx); continue; }
                int at = -1;
                for (int q = 0; q < distinct.Count; q++) {
                    if (string.Equals(distinct[q], sec, StringComparison.OrdinalIgnoreCase)) { at = q; break; }
                }
                if (at < 0) {
                    distinct.Add(sec);
                    List<int> l = new List<int>();
                    l.Add(idx);
                    members.Add(l);
                } else {
                    members[at].Add(idx);
                }
            }
            if (noSection.Count > 0) {
                r.Code = 3;
                StringBuilder sb = new StringBuilder();
                sb.Append("ambiguous: ").Append(allMatchIdx.Count.ToString())
                  .Append(" driver entries verified against the same package ").Append(chosenDir)
                  .Append(" but entry ").Append(DescribeRecords(noSection, records))
                  .Append(" report no install section name - install semantics cannot be proven; refusing to select");
                r.Message = sb.ToString();
            } else if (distinct.Count > 1) {
                r.Code = 3;
                StringBuilder sb = new StringBuilder();
                sb.Append("ambiguous: ").Append(allMatchIdx.Count.ToString())
                  .Append(" driver entries verified against the same package ").Append(chosenDir)
                  .Append(" report DIFFERENT install sections: ");
                for (int q = 0; q < distinct.Count; q++) {
                    if (q > 0) sb.Append(", ");
                    sb.Append("'").Append(distinct[q]).Append("' in ")
                      .Append(DescribeRecords(members[q], records));
                }
                sb.Append(" - install semantics differ; refusing to choose arbitrarily");
                r.Message = sb.ToString();
            } else {
                r.Code = 0;
                r.Index = members[0][0];
                r.Message = "selected driver entry " + r.Index.ToString() +
                            " - package identity verified in driver store dir " + chosenDir +
                            "; install section '" + distinct[0] + "' unique";
                if (allMatchIdx.Count > 1) {
                    r.Message += " (" + allMatchIdx.Count.ToString() +
                                 " equivalent entries sharing this package+section de-duplicated)";
                }
            }
        }
        return so;
    }

    // "([i] 'desc' section='s', ...)" -- identifies which candidate records
    // differ (or are incomplete) in an ambiguity error.
    private static string DescribeRecords(List<int> idxs, List<DrvDetail> records) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < idxs.Count; i++) {
            if (i > 0) sb.Append(", ");
            int idx = idxs[i];
            sb.Append("[").Append(idx.ToString()).Append("]");
            DrvDetail d = (records != null && idx >= 0 && idx < records.Count) ? records[idx] : null;
            if (d != null) {
                if (!string.IsNullOrEmpty(d.Description)) sb.Append(" '").Append(d.Description).Append("'");
                if (!string.IsNullOrEmpty(d.SectionName)) sb.Append(" section='").Append(d.SectionName).Append("'");
            }
        }
        return sb.ToString();
    }

    // Reads SPDRP_HARDWAREID and decides whether it matches `needle` by FULL
    // IDENTIFIER (exact, case-insensitive). Fail closed: any probe or read
    // outcome outside the documented buffer-size path yields an error and no
    // match verdict -- indeterminate is never reported as "does not match".
    private static bool TryHwidMatches(IntPtr set, ref SP_DEVINFO_DATA data, string needle, out bool matches, out int err) {
        matches = false;
        err = 0;
        uint type = 0, size = 0;
        bool probe = SetupDiGetDeviceRegistryProperty(set, ref data, SPDRP_HARDWAREID, out type, null, 0, out size);
        int probeErr = probe ? 0 : Marshal.GetLastWin32Error();
        if (ClassifyHwidProbe(probe, probeErr, size) == 0) {
            err = probeErr != 0 ? probeErr : -1;   // -1: probe succeeded but reported no size
            return false;
        }
        byte[] buf = new byte[size];
        if (!SetupDiGetDeviceRegistryProperty(set, ref data, SPDRP_HARDWAREID, out type, buf, size, out size)) {
            err = Marshal.GetLastWin32Error();
            return false;
        }
        string multi = Encoding.Unicode.GetString(buf, 0, (int)size);
        matches = MultiSzContainsExact(multi, needle);
        return true;
    }

    private static bool TryGetInstanceId(IntPtr set, ref SP_DEVINFO_DATA data, out string id, out int err) {
        id = null;
        err = 0;
        StringBuilder sb = new StringBuilder(1024);
        uint need;
        if (SetupDiGetDeviceInstanceId(set, ref data, sb, (uint)sb.Capacity, out need)) { id = sb.ToString(); return true; }
        err = Marshal.GetLastWin32Error();
        return false;
    }

    // Instances of `hwid` (case-insensitive) in `classGuid`.
    // NEVER collapses failure into an empty list: SetupDiGetClassDevs failure,
    // mid-enumeration errors (anything but ERROR_NO_MORE_ITEMS), unreadable
    // hardware IDs or unreadable instance IDs all yield Success=false with
    // ErrorCode/ErrorMessage. Only a genuinely exhausted enumeration returns
    // Success=true.
    public static EnumResult ListInstances(Guid classGuid, string hwid) {
        EnumResult res = new EnumResult();
        res.Success = false;
        res.Instances = new List<string>();
        res.ErrorCode = 0;
        res.ErrorMessage = "";
        Guid g = classGuid;
        string needle = hwid.ToUpperInvariant();
        IntPtr set = SetupDiGetClassDevs(ref g, null, IntPtr.Zero, DIGCF_ALLCLASSES);
        if (set == IntPtr.Zero || set == new IntPtr(-1)) {
            res.ErrorCode = Marshal.GetLastWin32Error();
            res.ErrorMessage = "SetupDiGetClassDevs failed";
            return res;
        }
        SP_DEVINFO_DATA data = new SP_DEVINFO_DATA();
        data.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        uint idx = 0;
        while (true) {
            if (!SetupDiEnumDeviceInfo(set, idx++, ref data)) {
                // Captured immediately: with SetLastError=true the marshaler slot
                // then holds THIS call's error, before anything else runs.
                int enumErr = Marshal.GetLastWin32Error();
                if (enumErr != ERROR_NO_MORE_ITEMS) {
                    res.ErrorCode = enumErr;
                    res.ErrorMessage = "SetupDiEnumDeviceInfo failed at index " + (idx - 1).ToString() + " (mid-enumeration)";
                    SetupDiDestroyDeviceInfoList(set);
                    return res;
                }
                break;   // normal, complete enumeration
            }
            bool matches;
            int err;
            if (!TryHwidMatches(set, ref data, needle, out matches, out err)) {
                res.ErrorCode = err;
                res.ErrorMessage = "SPDRP_HARDWAREID unreadable for device index " + (idx - 1).ToString() + " - refusing to assume non-match";
                SetupDiDestroyDeviceInfoList(set);
                return res;
            }
            if (!matches) continue;
            string id;
            if (!TryGetInstanceId(set, ref data, out id, out err) || id == null) {
                res.ErrorCode = (err != 0 ? err : -1);
                res.ErrorMessage = "matched device's instance ID unreadable - cannot enumerate safely";
                SetupDiDestroyDeviceInfoList(set);
                return res;
            }
            if (!res.Instances.Contains(id)) res.Instances.Add(id);
        }
        SetupDiDestroyDeviceInfoList(set);
        res.Success = true;
        return res;
    }

    // Retrieves + parses one driver-list entry's SP_DRVINFO_DETAIL_DATA.
    // Fails (err set) on ANY read/parse problem: without a verified detail we
    // must not decide which INF an entry represents. cbSize is set to
    // sizeof(SP_DRVINFO_DETAIL_DATA) -- NOT the buffer size (doc requirement).
    private static bool TryGetDriverDetail(IntPtr set, ref SP_DEVINFO_DATA data, ref SP_DRVINFO_DATA drv, out DrvDetail detail, out int err) {
        detail = null;
        err = 0;
        uint need = 0;
        SetupDiGetDriverInfoDetail(set, ref data, ref drv, IntPtr.Zero, 0, out need);
        if (need == 0) { err = Marshal.GetLastWin32Error(); return false; }
        IntPtr buf = Marshal.AllocHGlobal((int)need);
        try {
            Marshal.WriteInt32(buf, Marshal.SizeOf(typeof(SP_DRVINFO_DETAIL_DATA)));
            if (!SetupDiGetDriverInfoDetail(set, ref data, ref drv, buf, need, out need)) {
                err = Marshal.GetLastWin32Error();
                return false;
            }
            SP_DRVINFO_DETAIL_DATA d =
                (SP_DRVINFO_DETAIL_DATA)Marshal.PtrToStructure(buf, typeof(SP_DRVINFO_DETAIL_DATA));
            // Hardware ID = first NUL-terminated string of the HardwareID
            // buffer, but only when CompatIDsOffset > 1 (doc requirement).
            string hwidFirst = null;
            int hwidByteOff = Marshal.OffsetOf(typeof(SP_DRVINFO_DETAIL_DATA), "HardwareID").ToInt32();
            if (d.CompatIDsOffset > 1 && need > (uint)hwidByteOff) {
                int maxChars = ((int)need - hwidByteOff) / 2;
                string raw = Marshal.PtrToStringUni((IntPtr)(buf.ToInt64() + hwidByteOff), maxChars);
                if (raw != null) {
                    int z = raw.IndexOf('\0');
                    hwidFirst = (z >= 0) ? raw.Substring(0, z) : raw;
                    if (hwidFirst.Length == 0) hwidFirst = null;
                }
            }
            detail = new DrvDetail();
            detail.InfFileName = d.InfFileName;
            detail.HardwareId = hwidFirst;
            detail.SectionName = d.SectionName;
            detail.Description = d.DrvDescription;
            detail.ProviderName = drv.ProviderName;   // provider comes from SP_DRVINFO_DATA, not the DETAIL struct
            return true;
        } finally {
            Marshal.FreeHGlobal(buf);
        }
    }

    // Creates + registers a NEW root-enumerated instance, records ITS device
    // instance ID as this run's ownership token, then installs the staged
    // package on EXACTLY that instance via DiInstallDevice with an EXPLICITLY
    // SELECTED SP_DRVINFO_DATA (documented flow: BuildDriverInfoList ->
    // EnumDriverInfo -> GetDriverInfoDetail -> map published INF to the
    // driver store package -> verify package CONTENT IDENTITY -> select).
    // Selection only accepts the entry whose resolved driver store package
    // is content-identical (INF + declared files, SHA-256) to THIS run's
    // target package and whose hardware ID equals ours exactly -- never
    // "best match" guessing, never the wide hardware-ID force update. The
    // target identity is built BEFORE any device is created (Code=-4 on
    // failure: nothing created, nothing to roll back). InstanceId stays null
    // when registration did not complete or the created ID could not be read
    // -> callers must then delete nothing (ownership unknown).
    public static InstallResult RegisterAndInstall(Guid classGuid, string hwid, string infPath) {
        InstallResult res = new InstallResult();
        res.Code = -1;
        res.InstanceId = null;
        res.ErrorMessage = "";
        // Target package identity MUST be established BEFORE any device is
        // created: SHA-256 of the target INF plus every file the INF declares
        // (CatalogFile CAT, SourceDisksFiles/ServiceBinary binaries). Failure
        // => abort here: no device instance exists, nothing to roll back.
        PackageIdentity targetId = BuildIdentity(infPath);
        if (targetId.Error != null) {
            res.Code = -4;   // script-level: target identity insufficient
            res.ErrorMessage = "target package identity could not be established: " + targetId.Error +
                               " - refusing before creating any device";
            return res;
        }
        IntPtr set = SetupDiCreateDeviceInfoList(ref classGuid, IntPtr.Zero);
        if (set == IntPtr.Zero || set == new IntPtr(-1)) {
            res.Code = Marshal.GetLastWin32Error();
            res.ErrorMessage = "SetupDiCreateDeviceInfoList failed";
            return res;
        }
        SP_DEVINFO_DATA data = new SP_DEVINFO_DATA();
        data.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        bool listBuilt = false;
        // DeviceName must be non-NULL and must not contain '\'; the ROOT
        // enumerator + DICD_GENERATE_ID yield instance ID ROOT\TextDeckHidDriver\000N.
        // SP_DEVINFO_DATA is filled in as an OUT parameter.
        if (!SetupDiCreateDeviceInfo(set, RootDeviceName, ref classGuid,
                "Virtual HID Driver Device", IntPtr.Zero, DICD_GENERATE_ID, ref data)) {
            res.Code = Marshal.GetLastWin32Error();
            res.ErrorMessage = "SetupDiCreateDeviceInfo failed";
        } else if (!SetupDiSetDeviceRegistryProperty(set, ref data, SPDRP_HARDWAREID, hwid + "\0")) {
            res.Code = Marshal.GetLastWin32Error();
            res.ErrorMessage = "SetupDiSetDeviceRegistryProperty failed";
        } else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref data)) {
            res.Code = Marshal.GetLastWin32Error();
            res.ErrorMessage = "DIF_REGISTERDEVICE failed";
        } else {
            // The instance now EXISTS and was created by this call: own it.
            string id; int idErr;
            if (TryGetInstanceId(set, ref data, out id, out idErr) && id != null) {
                res.InstanceId = id;
            } else {
                res.InstanceId = null;   // ownership unknown => caller must not delete
            }
            if (!SetupDiBuildDriverInfoList(set, ref data, SPDIT_COMPATDRIVER)) {
                res.Code = Marshal.GetLastWin32Error();
                res.ErrorMessage = "SetupDiBuildDriverInfoList(SPDIT_COMPATDRIVER) failed";
            } else {
                listBuilt = true;
                List<SP_DRVINFO_DATA> drvInfos = new List<SP_DRVINFO_DATA>();
                List<DrvDetail> details = new List<DrvDetail>();
                string enumError = null;
                uint idx = 0;
                while (enumError == null) {
                    SP_DRVINFO_DATA di = new SP_DRVINFO_DATA();
                    di.cbSize = (uint)Marshal.SizeOf(typeof(SP_DRVINFO_DATA));
                    if (!SetupDiEnumDriverInfo(set, ref data, SPDIT_COMPATDRIVER, idx, ref di)) {
                        int e = Marshal.GetLastWin32Error();
                        if (e != ERROR_NO_MORE_ITEMS) {   // documented end-of-list
                            enumError = "SetupDiEnumDriverInfo failed at index " + idx.ToString() +
                                        " (error " + e.ToString() + ")";
                        }
                        break;
                    }
                    idx++;
                    DrvDetail det; int derr;
                    if (!TryGetDriverDetail(set, ref data, ref di, out det, out derr)) {
                        enumError = "SetupDiGetDriverInfoDetail failed at index " + (idx - 1).ToString() +
                                    " (error " + derr.ToString() + ") - cannot verify which INF an entry is";
                        break;
                    }
                    drvInfos.Add(di);
                    details.Add(det);
                }
                if (enumError != null) {
                    res.Code = -2;   // script-level: driver list could not be fully verified
                    res.ErrorMessage = enumError + " - refusing";
                } else {
                    SelectionOutcome sel = SelectTargetDriver(details, targetId, hwid);
                    if (sel.Result.Code != 0) {
                        res.Code = -3;   // script-level: no single identity-verified package
                        res.ErrorMessage = sel.Result.Message;
                    } else {
                        bool reboot;
                        SP_DRVINFO_DATA chosen = drvInfos[sel.Result.Index];
                        if (!DiInstallDevice(IntPtr.Zero, set, ref data, ref chosen, 0, out reboot)) {
                            res.Code = Marshal.GetLastWin32Error();
                            res.ErrorMessage = "DiInstallDevice failed (explicitly selected driver, device-scoped)";
                        } else {
                            res.Code = reboot ? 999 : 0;
                            res.ErrorMessage = sel.Result.Message;
                        }
                    }
                }
            }
        }
        if (listBuilt) SetupDiDestroyDriverInfoList(set, ref data, SPDIT_COMPATDRIVER);
        SetupDiDestroyDeviceInfoList(set);
        return res;
    }

    // Removes exactly ONE instance: requires hwid match AND exact instance-ID
    // match (callers pass only IDs they own). Devices whose hardware ID or
    // instance ID cannot be read are skipped, never deleted (fail-safe).
    // Returns 0 = removed, 1 = not found, other = Win32 error.
    public static int RemoveInstance(Guid classGuid, string hwid, string instanceId) {
        Guid g = classGuid;
        string needle = hwid.ToUpperInvariant();
        IntPtr set = SetupDiGetClassDevs(ref g, null, IntPtr.Zero, DIGCF_ALLCLASSES);
        if (set == IntPtr.Zero || set == new IntPtr(-1)) return Marshal.GetLastWin32Error();
        SP_DEVINFO_DATA data = new SP_DEVINFO_DATA();
        data.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
        int result = 1;   // not found
        uint idx = 0;
        while (SetupDiEnumDeviceInfo(set, idx++, ref data)) {
            bool matches; int err;
            if (!TryHwidMatches(set, ref data, needle, out matches, out err)) continue;  // unreadable -> never delete
            if (!matches) continue;
            string id;
            if (!TryGetInstanceId(set, ref data, out id, out err) || id == null) continue;
            if (!string.Equals(id, instanceId, StringComparison.OrdinalIgnoreCase)) continue;
            result = SetupDiCallClassInstaller(DIF_REMOVE, set, ref data) ? 0 : Marshal.GetLastWin32Error();
            break;
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }
}
'@
Add-Type -TypeDefinition $cs -ErrorAction Stop
# Device class GUID comes from the package INF, not a hard-coded constant:
# SetupDiCreateDeviceInfoList must be given the SAME class the INF declares,
# otherwise SetupDiBuildDriverInfoList(SPDIT_COMPATDRIVER) finds no match.
$infForClassPath = Join-Path $PackageDir 'TextDeckHidDriver.inf'
if (-not (Test-Path -LiteralPath $infForClassPath)) { Write-Error "INF not found: $infForClassPath"; exit 1 }
$infClassText = Get-Content -LiteralPath $infForClassPath -Raw
$classGuidMatch = [regex]::Match($infClassText, '(?im)^\s*ClassGuid\s*=\s*(\{[0-9a-fA-F\-]{36}\})')
if (-not $classGuidMatch.Success) { Write-Error "INF has no parsable ClassGuid"; exit 1 }
$sysGuid = $classGuidMatch.Groups[1].Value
$classNameMatch = [regex]::Match($infClassText, '(?im)^\s*Class\s*=\s*(\S[^\r\n]*)')
$className = if ($classNameMatch.Success) { $classNameMatch.Groups[1].Value.Trim() } else { '<unknown>' }
Write-Host ("[install] device class from INF: " + $className + " " + $sysGuid)
$hwid = 'Root\TextDeckHidDriver'

if ($LibraryOnly) { return }   # unit-test dot-source: functions + compiled helper only

# --- 2. package files (INF + SYS + CAT) ------------------------------------
$inf = Join-Path $PackageDir 'TextDeckHidDriver.inf'
$sys = Join-Path $PackageDir 'TextDeckHidDriver.sys'
$cat = Join-Path $PackageDir 'TextDeckHidDriver.cat'
foreach ($f in @($inf, $sys, $cat)) {
    if (-not (Test-Path $f)) { Write-Error "Package file not found: $f"; exit 1 }
}
Log "package files present (INF+SYS+CAT) in: $PackageDir"

# Informational only: the .sys embedded signature is NOT the package decision.
# A valid package can be "CAT signed, SYS embedded-unsigned, SYS covered by CAT".
try {
    $sysEmbedded = [string](& $script:GateDeps.GetSignatureStatus $sys)
} catch {
    $sysEmbedded = "error:$($_.Exception.Message)"
}
Log "sys embedded signature status: $sysEmbedded (informational only; the package decision is catalog-based)"

# --- 3. existing-device pre-check (read-only; runs BEFORE any system change) -
# The fail-closed enumeration MUST precede every mutating step (staging /
# device creation), so pre-existing Root\TextDeckHidDriver devices can never be
# updated or deleted by this script; finding any => refuse the install outright.
$preSnap = [PnpSetup]::ListInstances([Guid]$sysGuid, $hwid)
if (-not $preSnap.Success) {
    Log "REFUSING to install: device enumeration failed (code $($preSnap.ErrorCode): $($preSnap.ErrorMessage)) - cannot determine whether pre-existing devices exist (fail closed, not treated as 'no devices')."
    Log "No driver package staged, no device instance created."
    exit 1
}
$preExisting = @($preSnap.Instances | Where-Object { $_ })
if ($preExisting.Count -gt 0) {
    Log "REFUSING to install: pre-existing Root\TextDeckHidDriver device instance(s): $($preExisting -join ', ')"
    Log "This script performs first-time installs only: it will NOT stage, update, delete or reboot any existing device."
    Log "No driver package staged, no device instance created."
    exit 1
}
Log "pre-existing device instances: (none) - read-only check passed"

# --- 3. target policy (read-only queries) ----------------------------------
$policy = Get-TargetPolicy
$sbTxt = if ($null -eq $policy.SecureBoot)  { 'unknown' } else { "$($policy.SecureBoot)" }
$tsTxt = if ($null -eq $policy.TestSigning) { 'unknown' } else { "$($policy.TestSigning)" }
Log "target policy: SecureBoot=$sbTxt TestSigning=$tsTxt"

# --- 4. catalog-based signature gate (fail closed) --------------------------
$gate = Test-PackageGate -InfPath $inf -SysPath $sys -CatPath $cat -Policy $policy
foreach ($chk in $gate.Checks) {
    Log ("check [{0}] {1}: {2}" -f $(if ($chk.Pass) { 'PASS' } else { 'FAIL' }), $chk.Name, $chk.Detail)
}
if (-not $gate.Ok) {
    Log "REFUSING to install: $($gate.Reason)"
    Log "No driver package staged, no device instance created."
    exit 1
}
Log "signature gate passed: $($gate.Reason)"

if ($CheckOnly) {
    Log "CheckOnly: pre-flight checks passed; stopping before staging/device creation (nothing was installed)."
    exit 0
}

# --- mutating steps below: administrator required --------------------------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "This script must run elevated (Run as administrator)."
    exit 1
}

# --- staging + device creation: fail-closed sequence (mock-testable) --------
$seq = Invoke-InstallSequence -InfPath $inf -SysGuidText $sysGuid -HwidText $hwid
switch ($seq.Phase) {
    'snapshot-failed' {
        Log "ERROR: pre-install device enumeration failed (code $($seq.ErrorCode)); aborted BEFORE staging (nothing was staged, created or deleted)."
        exit 1
    }
    'refused-existing' {
        Log "REFUSING to install: pre-existing device instance(s) detected: $($seq.Before -join ', ')"
        Log "Existing devices are never updated, deleted or rebooted; nothing further was staged or created."
        exit 1
    }
    'stage-failed' {
        Log "ERROR: staging failed (pnputil exit code $($seq.StageCode)); no device instance was created or updated."
        exit 1
    }
    'recheck-failed' {
        Log "ERROR: post-staging device enumeration failed (code $($seq.ErrorCode)); aborted before device creation/binding."
        Log "Driver Store note: the staged package may remain there; this script never deletes driver packages."
        exit 1
    }
    'race-existing' {
        Log "ABORTED: matching device instance(s) present after staging: $($seq.ReCheck -join ', ')"
        Log "They were not created by this script and are left untouched; device creation/binding was not attempted."
        Log "Driver Store note: the staged package may remain there; this script never deletes driver packages."
        exit 1
    }
    'setupapi-failed' {
        Log "ERROR: SetupAPI device creation/binding failed (code $($seq.RegisterCode))."
        if ($seq.CreatedInstanceId) {
            Log "Rollback was ownership-based: only the instance ID confirmed as created by this run was eligible for removal ($($seq.CreatedInstanceId)); external instances were never touched."
        } elseif ($seq.Rollback.Count -eq 0) {
            Log "Ownership of any created instance could not be confirmed, so NOTHING was deleted (see sequence log; manual inspection may be required)."
        }
        Log "Driver Store note: the staged package may remain there (it may be shared); this script never deletes driver packages - use uninstall_driver.ps1 manually if desired."
        exit 1
    }
    'ok-reboot' {
        Log "device created; Windows reports a reboot would be required; NOT rebooting (project policy: no unsolicited reboots)."
    }
    'ok' { }
}

# --- result (read-only) -----------------------------------------------------
$dev = Get-PnpDevice -InstanceId '*ROOT\TEXTDECKHIDDRIVER*' -ErrorAction SilentlyContinue
if ($dev) { $dev | Format-List InstanceId, Status, Class, FriendlyName }
else { Log "No Root\TextDeckHidDriver device instance found (see messages above)." }

# --- installed-stack verification ------------------------------------------
# SetupAPI returning 0 means "the request was accepted", not "the device is
# right". The two can disagree: a device created before the INF's LowerFilters
# line existed keeps the hardware key it was born with, and the device then
# sits at Code 31 with VhfCreate failing, while every install call succeeded.
# The package-level contract test cannot see that, so assert the installed
# result here and make it part of the exit status.
$stackTest = Join-Path $PSScriptRoot '..\driver-tests\verify_installed_stack.ps1'
if (Test-Path $stackTest) {
    Log ""
    Log "verifying the installed device stack (not just the package)..."
    $prevEa = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    # -Strict: a freshly installed device has just re-created the volatile
    # diagnostic key, so the VhfCreate observation MUST be present. Accepting
    # "PASS WITH SKIPS" here would let a fresh install be called verified while
    # the driver's own result was never read -- exactly the unearned green this
    # gate exists to prevent.
    try { & $stackTest -Strict 2>&1 | ForEach-Object { Write-Host $_ } }
    finally { $ErrorActionPreference = $prevEa }
    $stackExit = $LASTEXITCODE
    if ($stackExit -ne 0) {
        Log "INSTALLED-STACK VERIFICATION FAILED (exit $stackExit)."
        Log "The device may exist but not have vhf.sys below it, not be started, or"
        Log "a required check did not run (exit 4 means: executed checks passed but"
        Log "at least one was skipped; here -Strict turns that into exit 1)."
        Log "Do NOT treat this install as successful. Inspect with:"
        Log "  pnputil /enum-devices /instanceid ROOT\TEXTDECKHIDDRIVER\000N /stack"
        exit 1
    }
    Log "installed-stack verification passed (all checks executed, none skipped)."
} else {
    Log "NOTE: $stackTest not found - installed-stack verification was NOT run."
    Log "A successful install was NOT confirmed beyond SetupAPI's return code."
}
