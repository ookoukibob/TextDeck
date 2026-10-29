# ---------------------------------------------------------------------------
# test_install_gate.ps1  -- unit/mock tests for driver-tools\install_driver.ps1
#
# Scope (safe by construction):
#   * dot-sources install_driver.ps1 with -LibraryOnly (main body never runs)
#   * feeds the gate canned Authenticode statuses / signtool exit codes /
#     policy states via the $script:GateDeps seams -> no signing, no certs
#   * exercises Invoke-PnputilStage against a PATH-shimmed pnputil.cmd stub
#     (returns a fixed exit code) -> no driver staging
#   * mocked Invoke-InstallSequence runs (injected listing/stage/register/
#     remove seams) proving pre-existing devices abort BEFORE any pnputil
#     mutation, staging never carries /install, enumeration failures abort
#     (fail-closed, never mistaken for "no devices"), and rollback removes
#     ONLY the instance ID this run created (ownership token, never a
#     before/after snapshot diff) -> no real devices are ever touched
#   * static control-flow assertions on install_driver.ps1 source text
#   * real read-only checks: signtool command-form smoke against whatever
#     package is currently built (signed OR unsigned -- the expectations are
#     derived from the MEASURED Authenticode status, never assumed), and
#     -CheckOnly preflight runs with before/after snapshots proving zero
#     side effects
#
# Never: installs drivers, creates/deletes device instances, imports
# certificates, touches Secure Boot/HVCI/Defender/BCD/BitLocker, reboots.
#
# Exit code: 0 = all checks passed, 1 = at least one failure.
# ---------------------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$installer = Join-Path (Split-Path -Parent $here) 'install_driver.ps1'
$pkgDir    = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) 'driver\TextDeckHidDriver\x64\Debug\TextDeckHidDriver'

# dot-source: defines Log/Get-TargetPolicy/Test-PackageGate/Invoke-PnputilStage
# and $script:GateDeps, then returns before the main body (-LibraryOnly).
. $installer -LibraryOnly

$script:Failures = @()
function Check([string]$Name, [bool]$Cond, [string]$Detail) {
    $state = if ($Cond) { 'OK  ' } else { 'FAIL' }
    Write-Host ("  [{0}] {1} :: {2}" -f $state, $Name, $Detail)
    if (-not $Cond) { $script:Failures += "$Name :: $Detail" }
}

# capture Log output so scenarios can assert on the sequence's messages
# (e.g. "deleting NOTHING" / "MANUAL ACTION REQUIRED") while still echoing.
$script:SeqLog = New-Object System.Collections.ArrayList
function Log($m) { Write-Host ("[install] " + $m); [void]$script:SeqLog.Add([string]$m) }

# preserve the production seams before overriding them
$origTool   = $script:GateDeps.SigntoolPath
$origStatus = $script:GateDeps.GetSignatureStatus
$origRun    = $script:GateDeps.RunSigntool

$fakeInf = 'D:\fake\TextDeckHidDriver.inf'
$fakeSys = 'D:\fake\TextDeckHidDriver.sys'
$fakeCat = 'D:\fake\TextDeckHidDriver.cat'

# ---------------------------------------------------------------------------
# 1. Gate decision matrix (canned statuses / exit codes / policy)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== [1] Gate decision matrix (mocked deps) ==='

function Invoke-GateScenario {
    param(
        [string]$Name,
        [string]$CatStatus,
        [string]$SysEmbedded,
        [hashtable]$Policy,
        [string]$SigntoolPath,
        [int]$CatVerifyExit = 0,
        [int]$InfVerifyExit = 0,
        [int]$SysVerifyExit = 0
    )
    # NB: captured via a free variable (ArrayList) -- GetNewClosure modules do
    # not share $script:-scoped variables with the test script.
    $list = New-Object System.Collections.ArrayList
    $script:GateDeps.SigntoolPath = $SigntoolPath

    $st = $CatStatus; $se = $SysEmbedded
    $script:GateDeps.GetSignatureStatus = {
        param([string]$Path)
        if ($Path -like '*.cat') { return $st }
        return $se
    }.GetNewClosure()

    $ce1 = $CatVerifyExit; $ce2 = $InfVerifyExit; $ce3 = $SysVerifyExit
    $script:GateDeps.RunSigntool = {
        param([string]$ToolPath, [string[]]$Arguments)
        [void]$list.Add($Arguments)
        if ($Arguments -contains '/c') {
            $last = $Arguments[$Arguments.Length - 1]
            if ($last -like '*.inf') { return [int]$ce2 }
            if ($last -like '*.sys') { return [int]$ce3 }
        }
        return [int]$ce1
    }.GetNewClosure()

    $gate = Test-PackageGate -InfPath $fakeInf -SysPath $fakeSys -CatPath $fakeCat -Policy $Policy
    return @{ Name = $Name; Gate = $gate; Calls = @($list) }
}

$devPolicy = @{ SecureBoot = $false; TestSigning = $true }   # dev/test target
$prodPolicy = @{ SecureBoot = $false; TestSigning = $false } # production target
$sbOnTsOnPolicy = @{ SecureBoot = $true; TestSigning = $true } # -> still production track

$cases = @(
    @{ Name = 'Valid + dev/test policy + all verifications 0 (+SYS embedded NotSigned = CAT-covered package) -> ALLOW'
       Want = $true; ReasonLike = $null; Pol = '/pa'
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + production policy + /kp verifications 0 -> ALLOW'
       Want = $true; ReasonLike = $null; Pol = '/kp'
       CatStatus = 'Valid'; SysEmbedded = 'Valid'; Policy = $prodPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + production + CAT /kp verify fails (exit 1) -> REFUSE'
       Want = $false; ReasonLike = '*<CAT> failed*'; Pol = '/kp'
       CatStatus = 'Valid'; SysEmbedded = 'Valid'; Policy = $prodPolicy; Tool = $origTool; Ex = @(1, 0, 0) },
    @{ Name = 'NotSigned + dev/test policy (current package) -> REFUSE'
       Want = $false; ReasonLike = '*NotSigned*'; Pol = $null
       CatStatus = 'NotSigned'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'HashMismatch + dev/test policy -> REFUSE (never allowed by policy alone)'
       Want = $false; ReasonLike = '*HashMismatch*'; Pol = $null
       CatStatus = 'HashMismatch'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'NotTrusted + dev/test policy -> REFUSE (never allowed by policy alone)'
       Want = $false; ReasonLike = '*NotTrusted*'; Pol = $null
       CatStatus = 'NotTrusted'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'UnknownError + dev/test policy -> REFUSE (never allowed by policy alone)'
       Want = $false; ReasonLike = '*UnknownError*'; Pol = $null
       CatStatus = 'UnknownError'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + SecureBoot query failed (unknown) -> REFUSE (fail closed)'
       Want = $false; ReasonLike = '*policy*'; Pol = $null
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = @{ SecureBoot = $null; TestSigning = $true }; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + TestSigning query failed (unknown) -> REFUSE (fail closed)'
       Want = $false; ReasonLike = '*policy*'; Pol = $null
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = @{ SecureBoot = $true; TestSigning = $null }; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + signtool.exe missing -> REFUSE'
       Want = $false; ReasonLike = '*signtool.exe not found*'; Pol = $null
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = 'C:\nonexistent\signtool.exe'; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + dev/test + INF not in CAT (verify /c exit 1) -> REFUSE'
       Want = $false; ReasonLike = '*INF*'; Pol = '/pa'
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 1, 0) },
    @{ Name = 'Valid + dev/test + SYS not in CAT (verify /c exit 1) -> REFUSE'
       Want = $false; ReasonLike = '*SYS*'; Pol = '/pa'
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = $devPolicy; Tool = $origTool; Ex = @(0, 0, 1) },
    @{ Name = 'Valid + SecureBoot=ON & TestSigning=ON -> routed to production /kp, all 0 -> ALLOW'
       Want = $true; ReasonLike = $null; Pol = '/kp'
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = $sbOnTsOnPolicy; Tool = $origTool; Ex = @(0, 0, 0) },
    @{ Name = 'Valid + SecureBoot=ON & TestSigning=ON + /kp fails -> REFUSE'
       Want = $false; ReasonLike = '*<CAT> failed*'; Pol = '/kp'
       CatStatus = 'Valid'; SysEmbedded = 'NotSigned'; Policy = $sbOnTsOnPolicy; Tool = $origTool; Ex = @(1, 0, 0) }
)

Write-Host ('  {0,-92} {1,-8} {2,-8} {3}' -f 'scenario', 'expect', 'actual', 'OK?')
$scenarioFail = 0
foreach ($c in $cases) {
    $r = Invoke-GateScenario -Name $c.Name -CatStatus $c.CatStatus -SysEmbedded $c.SysEmbedded `
        -Policy $c.Policy -SigntoolPath $c.Tool `
        -CatVerifyExit $c.Ex[0] -InfVerifyExit $c.Ex[1] -SysVerifyExit $c.Ex[2]
    $wantTxt = if ($c.Want) { 'ALLOW' } else { 'REFUSE' }
    $actual = if ($r.Gate.Ok) { 'ALLOW' } else { 'REFUSE' }
    $ok = ($actual -eq $wantTxt)
    if ($ok -and $c.ReasonLike) { $ok = ($r.Gate.Reason -like $c.ReasonLike) }
    # documented command form: every signtool call must use the expected policy
    if ($ok -and $c.Pol) {
        foreach ($a in $r.Calls) {
            $formOk = ($a[0] -eq 'verify') -and ($a[1] -eq $c.Pol) -and ($a[2] -eq '/v') -and
                      (($a.Length -eq 4) -or ($a.Length -eq 6 -and $a[3] -eq '/c' -and $a[4] -eq $fakeCat))
            if (-not $formOk) { $ok = $false; break }
        }
        if ($r.Calls.Count -ne 3 -and $wantTxt -eq 'ALLOW') { $ok = $false }
    }
    $detail = if ($ok) { $r.Gate.Reason } else { "reason='$($r.Gate.Reason)'" }
    Write-Host ('  {0,-92} {1,-8} {2,-8} {3}  {4}' -f $c.Name.Substring(0, [Math]::Min(92, $c.Name.Length)), $wantTxt, $actual, $(if ($ok) { 'OK' } else { '**' }), $detail)
    if (-not $ok) { $scenarioFail++; $script:Failures += "matrix: $($c.Name) (expected $wantTxt)" }
}
Check 'gate decision matrix' ($scenarioFail -eq 0) "$($cases.Count) scenarios, $scenarioFail failed"

# restore production seams
$script:GateDeps.SigntoolPath = $origTool
$script:GateDeps.GetSignatureStatus = $origStatus
$script:GateDeps.RunSigntool = $origRun

# ---------------------------------------------------------------------------
# 2. Real package: gate + documented signtool command forms (read-only)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== [2] Real package: CAT/INF/SYS verification decision table ==='
$realInf = Join-Path $pkgDir 'TextDeckHidDriver.inf'
$realSys = Join-Path $pkgDir 'TextDeckHidDriver.sys'
$realCat = Join-Path $pkgDir 'TextDeckHidDriver.cat'
$filesOk = (Test-Path $realInf) -and (Test-Path $realSys) -and (Test-Path $realCat)
Check 'package files present' $filesOk "INF+SYS+CAT under $pkgDir"
if ($filesOk) {
    $catStatus = (Get-AuthenticodeSignature -LiteralPath $realCat).Status.ToString()
    $sysStatus = (Get-AuthenticodeSignature -LiteralPath $realSys).Status.ToString()
    # Policy is MEASURED, not assumed. The expectation for every check below is
    # derived from the real Authenticode status of the current package, so this
    # section stays correct whether the package is unsigned (pre-signing state)
    # or test-signed (post signing).
    $policyReal = Get-TargetPolicy
    $policyKnown = ($null -ne $policyReal.SecureBoot) -and ($null -ne $policyReal.TestSigning)

    $gateReal = Test-PackageGate -InfPath $realInf -SysPath $realSys -CatPath $realCat -Policy $policyReal
    Write-Host '  gate checks (fail closed, stops at first failure):'
    foreach ($chk in $gateReal.Checks) {
        Write-Host ('    [{0}] {1}: {2}' -f $(if ($chk.Pass) { 'PASS' } else { 'FAIL' }), $chk.Name, $chk.Detail)
    }
    Write-Host ('  measured: CAT={0} SYS={1} policy known={2} (SecureBoot={3} TestSigning={4})' -f $catStatus, $sysStatus, $policyKnown, $policyReal.SecureBoot, $policyReal.TestSigning)

    if (-not $policyKnown) {
        # Fail closed: an unanswerable policy must refuse regardless of signing.
        Check 'gate refuses when policy unknown (fail closed)' (-not $gateReal.Ok) "reason: $($gateReal.Reason)"
    } elseif ($catStatus -eq 'Valid') {
        # Signed package: the gate must ACCEPT it (the signed state).
        Check 'signed package accepted by gate' ($gateReal.Ok) "reason: $($gateReal.Reason)"
        Check 'acceptance is catalog-based' ($gateReal.Checks | Where-Object { $_.Name -like 'CAT signature status*' -and $_.Pass }) 'CAT Authenticode status Valid'
    } else {
        # Unsigned package: the gate must REFUSE, and say why.
        Check 'unsigned package refused by gate' (-not $gateReal.Ok) "reason: $($gateReal.Reason)"
        Check 'refusal mentions NotSigned catalog' ($gateReal.Reason -like '*NotSigned*') $gateReal.Reason
    }

    # documented SignTool forms, run directly (verification only, read-only)
    $formRows = @(
        @{ What = 'CAT (dev/test form) signtool verify /pa /v <CAT>';      Args = @('verify', '/pa', '/v', $realCat) },
        @{ What = 'CAT (production form) signtool verify /kp /v <CAT>';    Args = @('verify', '/kp', '/v', $realCat) },
        @{ What = 'SYS in CAT signtool verify /kp /v /c <CAT> <SYS>';      Args = @('verify', '/kp', '/v', '/c', $realCat, $realSys) },
        @{ What = 'INF in CAT signtool verify /kp /v /c <CAT> <INF>';      Args = @('verify', '/kp', '/v', '/c', $realCat, $realInf) }
    )
    # Expected exit per documented form, derived from the measured package state:
    #   unsigned package -> every form must fail (nothing to verify)
    #   signed package   -> the /pa (PnP) forms must pass; the /kp (kernel
    #                       production) forms must STILL fail, because a
    #                       self-signed test certificate does not chain to a
    #                       Microsoft root. That asymmetry is exactly why the
    #                       installer selects /pa for a dev/test target.
    Write-Host '  direct signtool invocations (expected exit derived from measured state):'
    foreach ($row in $formRows) {
        & $origTool @($row.Args) | ForEach-Object { Write-Host ('      ' + $_) }
        $code = $LASTEXITCODE
        $isPa = ($row.Args -contains '/pa')
        if ($catStatus -ne 'Valid') {
            Check $row.What ($code -ne 0) "exit=$code (non-zero = correctly reports unsigned/absent signature)"
        } elseif ($isPa) {
            Check $row.What ($code -eq 0) "exit=$code (signed package: /pa form must verify)"
        } else {
            Check $row.What ($code -ne 0) "exit=$code (test cert does not chain to a Microsoft root -> /kp must fail)"
        }
    }
    Write-Host ('  decision table (real package): CAT status={0} | SYS embedded={1} (informational) | policy: SecureBoot={2} TestSigning={3}' -f $catStatus, $sysStatus, $policyReal.SecureBoot, $policyReal.TestSigning)
}

# ---------------------------------------------------------------------------
# 3. pnputil failure handling (PATH shim; no real staging)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== [3] pnputil failure must abort before SetupAPI ==='
$shimDir = Join-Path $env:TEMP 'vhid_pnputil_shim'
New-Item -ItemType Directory -Force -Path $shimDir | Out-Null
Set-Content -Path (Join-Path $shimDir 'pnputil.cmd') -Value "@echo off`r`nexit /b 7" -Encoding ASCII
$oldPath = $env:PATH
$env:PATH = "$shimDir;$oldPath"
$stageCode = $null
try {
    $stageCode = Invoke-PnputilStage -InfPath (Join-Path $shimDir 'fake.inf')
} finally {
    $env:PATH = $oldPath
}
Check 'Invoke-PnputilStage returns stub exit code' ($stageCode -eq 7) "returned=$stageCode (stub=7); no real pnputil ran"

$src = Get-Content -Raw -LiteralPath $installer
$srcLines = Get-Content -LiteralPath $installer
# executable (non-comment) text: PS '#' comments and the C# '//' comments
# inside the here-string are excluded, so identifiers may be named freely in
# documentation while their ABSENCE from code is asserted strictly.
$codeLines = @($srcLines | Where-Object { $_ -notmatch '^\s*(#|//)' })
$code = $codeLines -join "`n"

# main ordering: the fail-closed read-only pre-check must precede any mutation
$idxPreChk = $src.IndexOf('$preSnap = [PnpSetup]::ListInstances')
$idxPreOk  = $src.IndexOf('-not $preSnap.Success')
$idxSeq    = $src.IndexOf('Invoke-InstallSequence -InfPath $inf')
Check 'main: read-only device snapshot before any mutating step' ($idxPreChk -gt 0 -and $idxSeq -gt $idxPreChk) "precheck@$idxPreChk < sequence call@$idxSeq"
Check 'main: pre-check fail-closed (enum failure refuses, not "no devices")' ($idxPreOk -gt $idxPreChk) "Success guard@$idxPreOk"

# sequence internal invariant: fail-closed snapshot -> refuse-or-stage ->
# fail-closed recheck -> device-scoped SetupAPI
$idxList1  = $src.IndexOf('$snap1 = & $ListInstances')
$idxRefuse = $src.IndexOf("Phase = 'refused-existing'")
$idxStageF = $src.IndexOf('$stageCode = [int](& $Stage')
$idxReChk  = $src.IndexOf('$snap2 = & $ListInstances')
$idxReg    = $src.IndexOf('$reg = & $RegisterAndInstall')
Check 'sequence: existing-device refusal precedes staging' ($idxRefuse -gt 0 -and $idxRefuse -lt $idxStageF) "refuse@$idxRefuse < stage@$idxStageF"
Check 'sequence: snapshot -> staging -> recheck -> SetupAPI order' ($idxList1 -ge 0 -and $idxList1 -lt $idxStageF -and $idxStageF -lt $idxReChk -and $idxReChk -lt $idxReg) "list@$idxList1 < stage@$idxStageF < recheck@$idxReChk < register@$idxReg"
Check 'sequence: both snapshots check Success before use' (($src -match '\$snap1\.Success') -and ($src -match '\$snap2\.Success')) 'snap1.Success + snap2.Success guards present'
Check 'sequence: enumeration failures have dedicated phases' (($src.IndexOf("Phase = 'snapshot-failed'") -gt 0) -and ($src.IndexOf("Phase = 'recheck-failed'") -gt 0)) 'snapshot-failed / recheck-failed phases present'

# staging must never touch devices: the single pnputil invocation has no /install
$pnputilInvoke = @($srcLines | Where-Object { $_ -match '&\s+pnputil' })
Check 'single pnputil invocation, without /install' (($pnputilInvoke.Count -eq 1) -and ($pnputilInvoke[0] -notmatch '/install')) ($pnputilInvoke -join ' | ')

# rollback is OWNERSHIP-based: the before/after snapshot diff is gone, and
# removal is keyed to the exact instance ID this run created
Check 'rollback: no diff-based ownership inference' (-not ($code -match '\$before -notcontains')) 'no `$before -notcontains` anywhere in executable code'
Check 'rollback: removal keyed to owned instance ID' ($code.IndexOf('$RemoveInstance $SysGuidText $HwidText $ownedId') -gt 0) 'RemoveInstance called only with $ownedId'
Check 'result carries CreatedInstanceId (ownership token)' ($code.IndexOf('CreatedInstanceId = $ownedId') -gt 0) 'CreatedInstanceId present in returns'

# device-specific binding: DiInstallDevice receives an EXPLICITLY SELECTED
# ref SP_DRVINFO_DATA (never the INF path string, never NULL "best match");
# the wide hardware-ID force update must be absent from executable code entirely
Check 'binding passes selected ref SP_DRVINFO_DATA + Flags=0 to DiInstallDevice' ($code.IndexOf('DiInstallDevice(IntPtr.Zero, set, ref data, ref chosen, 0, out reboot)') -gt 0) 'call site wires the verified driver entry, Flags=0'
Check 'INF path is never passed as DriverInfoData' (-not ($code -match 'DiInstallDevice\([^)]*infPath')) 'no DiInstallDevice call takes infPath'
Check 'DI_QUIETINSTALL never used with DiInstallDevice' (-not ($code -match 'DI_QUIETINSTALL')) 'flag absent from executable code (invalid for this API)'
Check 'driver list lifecycle build->enum->detail->destroy present' (($code.IndexOf('SetupDiBuildDriverInfoList(set, ref data, SPDIT_COMPATDRIVER)') -gt 0) -and ($code.IndexOf('SetupDiEnumDriverInfo(set, ref data, SPDIT_COMPATDRIVER, idx, ref di)') -gt 0) -and ($code.IndexOf('SetupDiGetDriverInfoDetail(set, ref data, ref drv, buf, need, out need)') -gt 0) -and ($code.IndexOf('SetupDiDestroyDriverInfoList(set, ref data, SPDIT_COMPATDRIVER)') -gt 0)) 'full documented selection flow'
Check 'no wide UpdateDriverForPlugAndPlayDevices/INSTALLFLAG_FORCE in code' (($code -notmatch 'UpdateDriverForPlugAndPlayDevices') -and ($code -notmatch 'INSTALLFLAG_FORCE')) 'identifiers appear only in comments'

# enumerator fail-closed markers (C#): ERROR_NO_MORE_ITEMS is the ONLY normal
# end of the Enum* loops; the HWID probe never infers absence from an error
Check 'enum: only ERROR_NO_MORE_ITEMS ends enumeration normally' ($code.IndexOf('if (enumErr != ERROR_NO_MORE_ITEMS)') -gt 0) 'enum loop checks the captured last error'
Check 'hwid probe: 122 correctly named ERROR_INSUFFICIENT_BUFFER' (($code.IndexOf('ERROR_INSUFFICIENT_BUFFER = 122') -gt 0) -and (-not ($code -match 'ERROR_MORE_DATA'))) 'constant renamed per docs; old wrong name gone'
Check 'hwid probe: ERROR_INVALID_DATA (13) kept as ambiguous' ($code.IndexOf('ERROR_INVALID_DATA = 13') -gt 0) 'docs: absent OR invalid data => never a proof of absence'
Check 'hwid probe: no error treated as proof of absence' (($code.IndexOf('if (e == ERROR_NO_MORE_ITEMS) return true;') -lt 0) -and ($code.IndexOf('ClassifyHwidProbe(probe, probeErr, size)') -gt 0)) 'probe routed through the fail-closed classifier'
Check 'hwid comparison is exact full-identifier (no Contains)' ($code.IndexOf('MultiSzContainsExact(multi, needle)') -gt 0) 'MULTI_SZ member equality, not substring'

# package identity: published oemNN.inf -> driver store mapping + content
# verification (the INF leaf name alone can never identify the target package)
Check 'store mapping uses documented NULL/0 probe call' ($code.IndexOf('SetupGetInfDriverStoreLocation(candidateInfPath, IntPtr.Zero, null, null, 0, out need)') -gt 0) 'buffer probe per SetupGetInfDriverStoreLocation docs'
Check 'published-name round-trip lookup present' ($code.IndexOf('LookupPublishedName(m.StoreInfPath)') -gt 0) 'SetupGetInfPublishedName cross-check'
Check 'selection is content-identity based, not INF-leaf compare' (($code.IndexOf('Path.GetFileName(d.InfFileName), targetLeaf') -lt 0) -and ($code.IndexOf('IdentityMatches(target, cand, out why)') -gt 0)) 'leaf-only selection removed; identity comparison in use'
Check 'SHA-256 package identity hashing implemented' ($code.IndexOf('SHA256.Create()') -gt 0) 'INF + declared files hashed'
Check 'target identity built before any device creation' (($code.IndexOf('BuildIdentity(infPath)') -gt 0) -and ($code.IndexOf('BuildIdentity(infPath)') -lt $code.IndexOf('SetupDiCreateDeviceInfo(set, RootDeviceName, ref classGuid'))) 'identity gate precedes SetupDiCreateDeviceInfo'
# The create call must use the real SetupAPI.h argument order: the SP_DEVINFO_DATA
# is the OUT (last) parameter, and the instance name is NULL with
# DICD_GENERATE_ID so the system generates the ID.
Check 'SetupDiCreateDeviceInfo uses documented arg order + DICD_GENERATE_ID' (($code.IndexOf('SetupDiCreateDeviceInfo(set, RootDeviceName, ref classGuid') -gt 0) -and ($code.IndexOf('DICD_GENERATE_ID, ref data') -gt 0)) 'DeviceName=RootDeviceName, flags=DICD_GENERATE_ID, SP_DEVINFO_DATA passed by ref as the OUT parameter'
# DeviceName must be non-NULL and backslash-free; NULL and "Root\X" both fail
# with 0xE0000205 (ERROR_INVALID_DEVINST_NAME) - verified by direct probe.
Check 'device name is non-NULL and backslash-free' (($code.IndexOf('RootDeviceName = "TextDeckHidDriver"') -gt 0) -and ($code.IndexOf('SetupDiCreateDeviceInfo(set, null,') -lt 0)) 'DeviceName = "TextDeckHidDriver" (no backslash, not NULL)'

# doc-driven INF source-path inventory + install-section semantics.
# The declared file set must come from the audited parser (never a bare
# path.Combine over raw keys), unsupported syntax must fail closed with a
# reason, and one verified package may only yield a selection when every
# record proves the SAME install section.
Check 'identity uses the limited inventory parser (not leaf+dir combine)' ($code.IndexOf('ParseInfInventory(text, out perr)') -gt 0) 'declared file set comes from the audited parser'
Check 'parser refuses external INF refs and system LayoutFile' (($code.IndexOf('cannot be inventoried') -gt 0) -and ($code.IndexOf('system-supplied INF layout is unsupported') -gt 0)) 'Include/Needs/LayoutFile => identity insufficient'
Check 'parser refuses traversal/drive/UNC source paths' (($code.IndexOf("source path traversal ('..') rejected") -gt 0) -and ($code.IndexOf('drive-absolute source path rejected') -gt 0) -and ($code.IndexOf('UNC source path rejected') -gt 0)) 'media paths can never escape the package root'
Check 'declared files resolved only inside the package root (both layouts)' (($code.IndexOf('TryResolvePackageFile(dir, d, out fp, out tried)') -gt 0) -and ($code.IndexOf('private static bool TryGetFullPathInside') -gt 0)) 'flat-store + media-relative lookup, root containment enforced'
Check 'no first-record preference when one package has several entries' ($code.IndexOf('r.Index = matchIdx[0]') -lt 0) 'selection index comes from the proven-unique section, not list order'
Check 'install-section uniqueness enforced before any selection' (($code.IndexOf('report DIFFERENT install sections') -gt 0) -and ($code.IndexOf('report no install section name') -gt 0)) 'divergent or missing SectionName => ambiguity refusal'

# CopyINF refused, SourceDisksNames arch priority, strict CopyFiles syntax
Check 'CopyINF refused with explicit reason (not silently skipped)' (($code.IndexOf('CopyINF recursively imports the complete external driver package') -gt 0) -and ($code.IndexOf('string.Equals(k, "CopyINF", StringComparison.OrdinalIgnoreCase)') -gt 0)) 'non-empty CopyINF => identity incomplete; case-insensitive key match in tokenized scan'
Check 'SourceDisksNames arch priority implemented per disk id' (($code.IndexOf('TryLookupDiskPath(genericPaths, archPaths, diskid, out dpath, out diskFromArch)') -gt 0) -and ($code.IndexOf('defined twice in the same section with different paths') -gt 0)) 'arch layer wins per disk id; only same-section duplicates conflict'
Check 'CopyFiles strict syntax: mixing/multi-@/empty-item/decorated-section refused' (($code.IndexOf('mixes @filename with file-list section names') -gt 0) -and ($code.IndexOf('lists multiple @filename entries') -gt 0) -and ($code.IndexOf('CopyFiles list ') -gt 0) -and ($code.IndexOf('does not support decorating a file-list-section name') -gt 0)) 'documented form @filename | file-list-section[,...] enforced'
Check 'empty CopyFiles value ignored, no references added' ($code.IndexOf('if (cv.Trim().Length == 0) continue;') -gt 0) 'empty directive carries no file references (Include/Needs convention)'

# ---------------------------------------------------------------------------
# 4. Existing-device protection (mocked Invoke-InstallSequence; no real devices)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== [4] Existing-device protection (mocked sequence, no real devices) ==='

function Invoke-SeqScenario {
    param(
        [string]$Name,
        [object[]]$ListResults,  # successive EnumResult hashtables for the listing seam
        [int]$StageExit = 0,
        [int]$RegisterExit = 0,
        # Instance ID the register seam claims THIS run created; $null = the
        # created instance could not be identified (ownership unknown).
        [object]$CreatedId = 'ROOT\TextDeckHidDriver\0000',
        [int]$RemoveExit = 0
    )
    # NB: captured via free variables (ArrayList/hashtable) -- GetNewClosure
    # modules do not share $script:-scoped variables with the test script, and
    # plain `$n++` counters would be lost (scriptblocks run in a child scope:
    # reads traverse up, assignments stay local), so the list index lives in a
    # captured hashtable whose property updates persist by reference.
    $order = New-Object System.Collections.ArrayList
    $stageCalls = New-Object System.Collections.ArrayList
    $regCalls = New-Object System.Collections.ArrayList
    $remCalls = New-Object System.Collections.ArrayList
    $state = @{ Idx = 0 }
    $lists = $ListResults
    $listFallback = @{ Success = $false; Instances = @(); ErrorCode = 31; ErrorMessage = 'stub: no further canned enumeration results' }
    $script:SeqLog.Clear()

    $listStub = {
        param($g, $h)
        [void]$order.Add('list')
        $v = $listFallback
        if ($state.Idx -lt $lists.Count) { $v = $lists[$state.Idx] }
        $state.Idx++
        return $v
    }.GetNewClosure()
    $stageStub = {
        param($p)
        [void]$order.Add('stage'); [void]$stageCalls.Add($p)
        return $StageExit
    }.GetNewClosure()
    $regStub = {
        param($g, $h, $p)
        [void]$order.Add('register'); [void]$regCalls.Add($p)
        return @{ Code = $RegisterExit; InstanceId = $CreatedId; ErrorMessage = 'stubbed register result' }
    }.GetNewClosure()
    $remStub = {
        param($g, $h, $i)
        [void]$order.Add('remove'); [void]$remCalls.Add($i)
        return $RemoveExit
    }.GetNewClosure()

    $seq = Invoke-InstallSequence -InfPath 'D:\fake\pkg\TextDeckHidDriver.inf' `
        -SysGuidText '{4d36e97d-e325-11ce-bfc1-08002be10318}' -HwidText 'Root\TextDeckHidDriver' `
        -ListInstances $listStub -Stage $stageStub -RegisterAndInstall $regStub -RemoveInstance $remStub
    return @{ Name = $Name; Seq = $seq; Order = @($order); StageCalls = @($stageCalls); RegCalls = @($regCalls); RemCalls = @($remCalls) }
}

# EnumResult builders for the injected listing seam (never real SetupAPI)
function EnumOk([string[]]$Ids) {
    return @{ Success = $true; Instances = @($Ids); ErrorCode = 0; ErrorMessage = '' }
}
function EnumFail([int]$Code, [string]$Message) {
    return @{ Success = $false; Instances = @(); ErrorCode = $Code; ErrorMessage = $Message }
}

function SeqCheck([string]$Scenario, [string]$Name, [bool]$Cond, [string]$Detail) {
    $state = if ($Cond) { 'OK  ' } else { 'FAIL' }
    Write-Host ("  [{0}] {1} :: {2} :: {3}" -f $state, $Scenario, $Name, $Detail)
    if (-not $Cond) { $script:Failures += "seq[$Scenario] $Name :: $Detail" }
}

# S1: pre-existing device exists -> refuse BEFORE any pnputil/staging mutation
$r = Invoke-SeqScenario -Name 'S1-existing-device' -ListResults (,@(EnumOk @('ROOT\TextDeckHidDriver\0000')))
SeqCheck $r.Name 'phase=refused-existing'       ($r.Seq.Phase -eq 'refused-existing' -and -not $r.Seq.Ok) "phase=$($r.Seq.Phase)"
SeqCheck $r.Name 'staging NEVER executed'       ($r.StageCalls.Count -eq 0 -and ($r.Order -notcontains 'stage')) "order=$($r.Order -join ',')"
SeqCheck $r.Name 'no create/update/remove ran'  ($r.RegCalls.Count -eq 0 -and $r.RemCalls.Count -eq 0) "reg=$($r.RegCalls.Count) remove=$($r.RemCalls.Count)"
SeqCheck $r.Name 'reported in Before'           ($r.Seq.Before -contains 'ROOT\TextDeckHidDriver\0000') ($r.Seq.Before -join ',')

# S2: no existing devices -> staging allowed, then device creation
$r = Invoke-SeqScenario -Name 'S2-no-existing' -ListResults (,@(EnumOk @()) + ,@(EnumOk @()))
SeqCheck $r.Name 'phase=ok'                     ($r.Seq.Phase -eq 'ok' -and $r.Seq.Ok) "phase=$($r.Seq.Phase)"
SeqCheck $r.Name 'order=list,stage,list,register' (($r.Order -join ',') -eq 'list,stage,list,register') "order=$($r.Order -join ',')"
SeqCheck $r.Name 'staged exactly once'          ($r.StageCalls.Count -eq 1) "stage calls=$($r.StageCalls.Count)"

# S3: staging fails -> abort; SetupAPI never called
$r = Invoke-SeqScenario -Name 'S3-stage-fail' -ListResults (,@(EnumOk @())) -StageExit 7
SeqCheck $r.Name 'phase=stage-failed (code 7)'  ($r.Seq.Phase -eq 'stage-failed' -and $r.Seq.StageCode -eq 7) "phase=$($r.Seq.Phase) code=$($r.Seq.StageCode)"
SeqCheck $r.Name 'SetupAPI never called'        ($r.RegCalls.Count -eq 0) "reg=$($r.RegCalls.Count)"

# S4: a matching device appears after staging (race) -> abort, leave it untouched
$r = Invoke-SeqScenario -Name 'S4-race-existing' -ListResults (,@(EnumOk @()) + ,@(EnumOk @('ROOT\TextDeckHidDriver\0001')))
SeqCheck $r.Name 'phase=race-existing'          ($r.Seq.Phase -eq 'race-existing') "phase=$($r.Seq.Phase)"
SeqCheck $r.Name 'device creation NOT attempted' ($r.RegCalls.Count -eq 0) "reg=$($r.RegCalls.Count)"
SeqCheck $r.Name 'foreign instance NOT deleted' ($r.RemCalls.Count -eq 0) "remove=$($r.RemCalls.Count)"

# S5: bind fails with a confirmed ownership token -> remove EXACTLY that ID
$r = Invoke-SeqScenario -Name 'S5-setupapi-fail-owned-rollback' -ListResults (,@(EnumOk @()) + ,@(EnumOk @())) -RegisterExit 5 -CreatedId 'ROOT\TextDeckHidDriver\0000'
SeqCheck $r.Name 'phase=setupapi-failed (code 5)' ($r.Seq.Phase -eq 'setupapi-failed' -and $r.Seq.RegisterCode -eq 5) "phase=$($r.Seq.Phase) code=$($r.Seq.RegisterCode)"
SeqCheck $r.Name 'rollback removed owned instance' (($r.RemCalls.Count -eq 1) -and ($r.RemCalls -contains 'ROOT\TextDeckHidDriver\0000')) "removed=$($r.RemCalls -join ',')"
SeqCheck $r.Name 'ownership recorded in result' (($r.Seq.CreatedInstanceId -eq 'ROOT\TextDeckHidDriver\0000') -and ($r.Seq.Rollback.Count -eq 1) -and ($r.Seq.Rollback[0].Result -eq 0) -and $r.Seq.Rollback[0].OwnedByThisRun) "created=$($r.Seq.CreatedInstanceId) rollback=$($r.Seq.Rollback | ConvertTo-Json -Compress)"
SeqCheck $r.Name 'no 3rd enumeration drives rollback' (($r.Order -join ',') -eq 'list,stage,list,register,remove') "order=$($r.Order -join ',')"

# S6: bind fails BEFORE any confirmed instance (ownership unknown) -> delete NOTHING
$r = Invoke-SeqScenario -Name 'S6-setupapi-fail-ownership-unknown' -ListResults (,@(EnumOk @()) + ,@(EnumOk @()) + ,@(EnumOk @('ROOT\TextDeckHidDriver\0000','ROOT\TextDeckHidDriver\0001'))) -RegisterExit 5 -CreatedId $null
$logTxt = $script:SeqLog -join "`n"
SeqCheck $r.Name 'phase=setupapi-failed, created=unconfirmed' ($r.Seq.Phase -eq 'setupapi-failed' -and $null -eq $r.Seq.CreatedInstanceId) "phase=$($r.Seq.Phase) created=$($r.Seq.CreatedInstanceId)"
SeqCheck $r.Name 'NO device deleted (fail-safe)'  ($r.RemCalls.Count -eq 0 -and $r.Seq.Rollback.Count -eq 0) "remove=$($r.RemCalls.Count)"
SeqCheck $r.Name 'reports manual action required' ($logTxt -like '*deleting NOTHING*' -and $logTxt -like '*MANUAL ACTION REQUIRED*') 'log says deleting NOTHING + MANUAL ACTION REQUIRED'

# S7: reboot-required success -> reported, no reboot, no removals
$r = Invoke-SeqScenario -Name 'S7-reboot-required' -ListResults (,@(EnumOk @()) + ,@(EnumOk @())) -RegisterExit 999
SeqCheck $r.Name 'phase=ok-reboot, Ok=true'     ($r.Seq.Phase -eq 'ok-reboot' -and $r.Seq.Ok) "phase=$($r.Seq.Phase)"

# S8: SetupAPI snapshot API failure -> refuse BEFORE staging (nothing mutates)
$r = Invoke-SeqScenario -Name 'S8-snapshot-api-fail' -ListResults (,@(EnumFail 87 'SetupDiGetClassDevs failed'))
SeqCheck $r.Name 'phase=snapshot-failed (code 87)' ($r.Seq.Phase -eq 'snapshot-failed' -and $r.Seq.ErrorCode -eq 87) "phase=$($r.Seq.Phase) code=$($r.Seq.ErrorCode)"
SeqCheck $r.Name 'staging refused'              ($r.StageCalls.Count -eq 0 -and ($r.Order -notcontains 'stage')) "order=$($r.Order -join ',')"
SeqCheck $r.Name 'no create/bind/remove'        ($r.RegCalls.Count -eq 0 -and $r.RemCalls.Count -eq 0) "reg=$($r.RegCalls.Count) remove=$($r.RemCalls.Count)"
SeqCheck $r.Name 'not misreported as no-devices' ($r.Seq.Phase -ne 'refused-existing') "phase=$($r.Seq.Phase)"

# S9: mid-enumeration error -> refuse (an error is never 'no devices')
$r = Invoke-SeqScenario -Name 'S9-enum-midfail' -ListResults (,@(EnumFail 31 'SetupDiEnumDeviceInfo failed at index 2 (mid-enumeration)'))
SeqCheck $r.Name 'phase=snapshot-failed (code 31)' ($r.Seq.Phase -eq 'snapshot-failed' -and $r.Seq.ErrorCode -eq 31) "phase=$($r.Seq.Phase) code=$($r.Seq.ErrorCode)"
SeqCheck $r.Name 'staging refused'              ($r.StageCalls.Count -eq 0) "stage=$($r.StageCalls.Count)"

# S10: post-staging re-enumeration fails -> no device creation/binding
$r = Invoke-SeqScenario -Name 'S10-recheck-fail' -ListResults (,@(EnumOk @()) + ,@(EnumFail 31 'SetupDiEnumDeviceInfo failed at index 0 (mid-enumeration)'))
SeqCheck $r.Name 'phase=recheck-failed (code 31)' ($r.Seq.Phase -eq 'recheck-failed' -and $r.Seq.ErrorCode -eq 31) "phase=$($r.Seq.Phase) code=$($r.Seq.ErrorCode)"
SeqCheck $r.Name 'staged once, then aborted'    ($r.StageCalls.Count -eq 1) "stage=$($r.StageCalls.Count)"
SeqCheck $r.Name 'create/bind/remove never called' ($r.RegCalls.Count -eq 0 -and $r.RemCalls.Count -eq 0) "reg=$($r.RegCalls.Count) remove=$($r.RemCalls.Count)"

# S11: external instance created by another process DURING bind; our bind still
# succeeds on our own instance -> success path removes/updates NOTHING else
# (the bind itself is device-scoped: DiInstallDevice on our SP_DEVINFO_DATA,
# proven by the static assertions in section [3])
$r = Invoke-SeqScenario -Name 'S11-external-during-bind-ok' -ListResults (,@(EnumOk @()) + ,@(EnumOk @())) -RegisterExit 0 -CreatedId 'ROOT\TextDeckHidDriver\0001'
SeqCheck $r.Name 'phase=ok'                     ($r.Seq.Phase -eq 'ok' -and $r.Seq.Ok) "phase=$($r.Seq.Phase)"
SeqCheck $r.Name 'no removal on success path'   ($r.RemCalls.Count -eq 0) "remove=$($r.RemCalls.Count)"
SeqCheck $r.Name 'bind attempted exactly once'  ($r.RegCalls.Count -eq 1) "reg=$($r.RegCalls.Count)"

# S12: bind fails while an EXTERNAL instance exists alongside ours -> only our
# owned ID is removed; the external instance is never passed to RemoveInstance
$r = Invoke-SeqScenario -Name 'S12-external-never-removed' -ListResults (,@(EnumOk @()) + ,@(EnumOk @())) -RegisterExit 5 -CreatedId 'ROOT\TextDeckHidDriver\0001'
SeqCheck $r.Name 'phase=setupapi-failed'        ($r.Seq.Phase -eq 'setupapi-failed') "phase=$($r.Seq.Phase)"
SeqCheck $r.Name 'remove called with ONLY our owned ID' (($r.RemCalls.Count -eq 1) -and ($r.RemCalls[0] -eq 'ROOT\TextDeckHidDriver\0001')) "removed=$($r.RemCalls -join ',')"
SeqCheck $r.Name 'external instance never targeted' (-not ($r.RemCalls -contains 'ROOT\TextDeckHidDriver\0000')) "external ROOT\TextDeckHidDriver\0000 absent from remove calls"

# S13: target package SELECTION fails inside bind (script-level -3: no single
# identity-verified store package) with confirmed ownership -> rollback
# removes exactly the owned instance, nothing else
$r = Invoke-SeqScenario -Name 'S13-select-fail-owned-rollback' -ListResults (,@(EnumOk @()) + ,@(EnumOk @())) -RegisterExit -3 -CreatedId 'ROOT\TextDeckHidDriver\0000'
SeqCheck $r.Name 'phase=setupapi-failed (code -3)' (($r.Seq.Phase -eq 'setupapi-failed') -and ($r.Seq.RegisterCode -eq -3)) "phase=$($r.Seq.Phase) code=$($r.Seq.RegisterCode)"
SeqCheck $r.Name 'rollback removed only owned instance' (($r.RemCalls.Count -eq 1) -and ($r.RemCalls -contains 'ROOT\TextDeckHidDriver\0000')) "removed=$($r.RemCalls -join ',')"

# S14: target identity could not be established (script-level -4; the real
# flow returns this BEFORE creating any device instance) -> nothing to roll
# back: zero deletions + explicit manual-action message
$r = Invoke-SeqScenario -Name 'S14-target-identity-fail' -ListResults (,@(EnumOk @()) + ,@(EnumOk @())) -RegisterExit -4 -CreatedId $null
$logTxt = $script:SeqLog -join "`n"
SeqCheck $r.Name 'phase=setupapi-failed, created=unconfirmed' (($r.Seq.Phase -eq 'setupapi-failed') -and ($null -eq $r.Seq.CreatedInstanceId)) "phase=$($r.Seq.Phase)"
SeqCheck $r.Name 'NO device deleted (fail-safe)' ($r.RemCalls.Count -eq 0) "remove=$($r.RemCalls.Count)"
SeqCheck $r.Name 'manual-action message present' (($logTxt -like '*deleting NOTHING*') -and ($logTxt -like '*MANUAL ACTION REQUIRED*')) 'log says deleting NOTHING + MANUAL ACTION REQUIRED'

# ---------------------------------------------------------------------------
# 5. -CheckOnly preflight: zero side effects (before/after snapshot)
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== [5] -CheckOnly preflight side-effect check (read-only snapshots) ==='
function Get-EnvSnapshot {
    return @{
        Devices  = (@(Get-PnpDevice -InstanceId '*ROOT\TEXTDECKHIDDRIVER*' -ErrorAction SilentlyContinue | ForEach-Object { $_.InstanceId }) -join ';')
        Drivers  = (& pnputil /enum-drivers 2>&1 | Out-String)
        CertRoot = (Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue).Count
        CertMy   = (Get-ChildItem Cert:\LocalMachine\My -ErrorAction SilentlyContinue).Count
        CertUserMy = (Get-ChildItem Cert:\CurrentUser\My -ErrorAction SilentlyContinue).Count
    }
}
$beforeSnap = Get-EnvSnapshot

# 4a. unelevated CheckOnly -> policy unknown -> must refuse (fail closed)
$p1 = Start-Process powershell -NoNewWindow -Wait -PassThru -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',$installer,'-CheckOnly'
$unelevExit = $p1.ExitCode
Check 'unelevated -CheckOnly refuses (policy unknown)' ($unelevExit -eq 1) "exit=$unelevExit"

# 4b. elevated CheckOnly -> policy known; outcome must match the real package
#     state (refused when unsigned / accepted when correctly signed), and the
#     before/after snapshots below must show ZERO side effects either way.
$elevOut = Join-Path $env:TEMP 'vhid_elev_check.out.txt'
$wrap = Join-Path $env:TEMP 'vhid_elev_check.ps1'
$wrapBody = "& '" + $installer + "' -CheckOnly *>'" + $elevOut + "'`r`nexit `$LASTEXITCODE"
Set-Content -Path $wrap -Value $wrapBody -Encoding ASCII
$elevExit = $null
try {
    $p2 = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',$wrap
    $elevExit = $p2.ExitCode
} catch {
    $elevExit = 'UAC-denied'
}
if ($elevExit -eq 'UAC-denied') {
    Check 'elevated -CheckOnly behaved as expected' $false 'UAC prompt denied; elevated run skipped'
} else {
    $elevText = Get-Content $elevOut -Raw -ErrorAction SilentlyContinue
    $refused  = ($elevText -like '*REFUSING to install*')
    $accepted = ($elevText -like '*pre-flight checks passed*')
    # A run must end in exactly ONE of the two defined terminal states.
    Check 'elevated -CheckOnly reached a defined terminal state' ($refused -xor $accepted) "exit=$elevExit refused=$refused accepted=$accepted"
    # ...and that state must agree with the machine + package reality:
    #   * signed package + no device instance -> the gate accepts
    #   * signed package BUT a device instance already exists -> the installer
    #     MUST still refuse (first-install-only tool); that is correct behaviour,
    #     not a signing failure
    #   * unsigned package -> refuse
    $pkgSigned = ((Get-AuthenticodeSignature -LiteralPath (Join-Path $pkgDir 'TextDeckHidDriver.cat')).Status.ToString() -eq 'Valid')
    $devCount  = @(Get-PnpDevice -InstanceId '*ROOT\TEXTDECKHIDDRIVER*' -ErrorAction SilentlyContinue).Count
    if ($pkgSigned -and $devCount -eq 0) {
        Check 'elevated outcome matches measured package state' $accepted "pkgSigned=True devices=0 -> accepted"
    } elseif ($pkgSigned -and $devCount -gt 0) {
        Check 'elevated outcome matches measured package state' ($refused -and ($elevText -like '*pre-existing*')) "pkgSigned=True devices=$devCount -> correctly refused as already installed"
    } else {
        Check 'elevated outcome matches measured package state' $refused "pkgSigned=False -> refused"
    }
    if ($refused) {
        Check 'elevated refusal names a reason' (($elevText -split "`r?`n" | Select-String 'REFUSING|policy|status').Count -gt 0) (($elevText -split "`r?`n" | Select-String 'REFUSING|policy|status' | Select-Object -First 4) -join ' | ')
    }
}

$afterSnap = Get-EnvSnapshot
Check 'no device instance appeared'        ($beforeSnap.Devices -eq $afterSnap.Devices) "before='$($beforeSnap.Devices)' after='$($afterSnap.Devices)'"
Check 'driver store unchanged'             ($beforeSnap.Drivers -ceq $afterSnap.Drivers) "pnputil /enum-drivers byte-identical"
Check 'LocalMachine\Root cert count same' ($beforeSnap.CertRoot -eq $afterSnap.CertRoot) "$($beforeSnap.CertRoot) -> $($afterSnap.CertRoot)"
Check 'LocalMachine\My cert count same'   ($beforeSnap.CertMy -eq $afterSnap.CertMy) "$($beforeSnap.CertMy) -> $($afterSnap.CertMy)"
Check 'CurrentUser\My cert count same'    ($beforeSnap.CertUserMy -eq $afterSnap.CertUserMy) "$($beforeSnap.CertUserMy) -> $($afterSnap.CertUserMy)"

# ---------------------------------------------------------------------------
# 6. Native API contract + target-driver selection (compiled helper; NO devices)
#    These are compile-time/reflection/pure-logic checks against the in-memory
#    P/Invoke helper compiled by the -LibraryOnly dot-source. They verify the
#    DECLARED native signatures (per Microsoft docs), the x64 struct layouts
#    (Marshal.SizeOf/OffsetOf), the call-site argument wiring, and the pure
#    selection / probe-classification / identifier-comparison logic. They do
#    NOT call any native device-installation function and must never be read
#    as proof that a real driver installation or HID input works.
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '=== [6] Native API contract + target-driver selection (no devices) ==='

# --- 6a. DiInstallDevice P/Invoke contract (reflection; MSDN-corrected) ---
$diM = [PnpSetup].GetMethod('DiInstallDevice')
$diP = $diM.GetParameters()
Check 'DiInstallDevice: 6 parameters' ($diP.Length -eq 6) "count=$($diP.Length)"
Check 'DiInstallDevice: param4 is ref SP_DRVINFO_DATA (not INF string)' ($diP[3].ParameterType.IsByRef -and ($diP[3].ParameterType.GetElementType() -eq [PnpSetup+SP_DRVINFO_DATA])) "$($diP[3].ParameterType)"
Check 'DiInstallDevice: param5 Flags is uint' ($diP[4].ParameterType -eq [uint32]) "$($diP[4].ParameterType)"
Check 'DiInstallDevice: param6 is out bool (NeedReboot)' ($diP[5].ParameterType.IsByRef -and ($diP[5].ParameterType.GetElementType() -eq [bool])) "$($diP[5].ParameterType)"
$diAttr = @($diM.GetCustomAttributes([System.Runtime.InteropServices.DllImportAttribute], $false))[0]
Check 'DiInstallDevice imports newdev.dll (Unicode)' (($null -ne $diAttr) -and ($diAttr.Value -eq 'newdev.dll') -and ($diAttr.CharSet -eq [System.Runtime.InteropServices.CharSet]::Unicode)) "$($diAttr.Value)/$($diAttr.CharSet)"

# driver-list APIs: signatures per setupapi.h (SP_DRVINFO_DATA wiring)
$edM = [PnpSetup].GetMethod('SetupDiEnumDriverInfo'); $edP = $edM.GetParameters()
Check 'SetupDiEnumDriverInfo: param5 is ref SP_DRVINFO_DATA' ($edP[4].ParameterType.IsByRef -and ($edP[4].ParameterType.GetElementType() -eq [PnpSetup+SP_DRVINFO_DATA])) "$($edP[4].ParameterType)"
$gdM = [PnpSetup].GetMethod('SetupDiGetDriverInfoDetail'); $gdP = $gdM.GetParameters()
Check 'SetupDiGetDriverInfoDetail: param3 ref SP_DRVINFO_DATA, param4 raw buffer' (($gdP[2].ParameterType.IsByRef -and ($gdP[2].ParameterType.GetElementType() -eq [PnpSetup+SP_DRVINFO_DATA])) -and ($gdP[3].ParameterType -eq [IntPtr])) "$($gdP[2].ParameterType)/$($gdP[3].ParameterType)"
$bdM = [PnpSetup].GetMethod('SetupDiBuildDriverInfoList'); $bdP = $bdM.GetParameters()
Check 'SetupDiBuildDriverInfoList: param2 ref SP_DEVINFO_DATA, param3 uint type' (($bdP[1].ParameterType.IsByRef -and ($bdP[1].ParameterType.GetElementType() -eq [PnpSetup+SP_DEVINFO_DATA])) -and ($bdP[2].ParameterType -eq [uint32])) "$($bdP[1].ParameterType)/$($bdP[2].ParameterType)"
Check 'SetupDiDestroyDriverInfoList declared (list cleanup)' ($null -ne [PnpSetup].GetMethod('SetupDiDestroyDriverInfoList')) 'SetupDiDestroyDriverInfoList present'
$gslM = [PnpSetup].GetMethod('SetupGetInfDriverStoreLocation'); $gslP = $gslM.GetParameters()
Check 'SetupGetInfDriverStoreLocation: 6 params, probe-safe types' (($gslP.Length -eq 6) -and ($gslP[0].ParameterType -eq [string]) -and ($gslP[1].ParameterType -eq [IntPtr]) -and ($gslP[2].ParameterType -eq [string]) -and ($gslP[3].ParameterType -eq [System.Text.StringBuilder]) -and ($gslP[4].ParameterType -eq [uint32]) -and $gslP[5].ParameterType.IsByRef) "count=$($gslP.Length)"
$gslAttr = @($gslM.GetCustomAttributes([System.Runtime.InteropServices.DllImportAttribute], $false))[0]
Check 'SetupGetInfDriverStoreLocation imports setupapi.dll (Unicode)' (($null -ne $gslAttr) -and ($gslAttr.Value -eq 'setupapi.dll') -and ($gslAttr.CharSet -eq [System.Runtime.InteropServices.CharSet]::Unicode)) "$($gslAttr.Value)/$($gslAttr.CharSet)"
$gpnM = [PnpSetup].GetMethod('SetupGetInfPublishedName'); $gpnP = $gpnM.GetParameters()
Check 'SetupGetInfPublishedName: 4 params (store->published reverse)' (($gpnP.Length -eq 4) -and ($gpnP[0].ParameterType -eq [string]) -and ($gpnP[1].ParameterType -eq [System.Text.StringBuilder]) -and $gpnP[3].ParameterType.IsByRef) "count=$($gpnP.Length)"

# --- 6b. x64 ABI struct layouts (docs-derived expectations) ---
# NOTE: PS 5.1 would bind Marshal's static SizeOf/OffsetOf to the GENERIC
# overloads when given a RuntimeType; select the non-generic overloads via
# reflection so the layout probe is deterministic.
$mSizeOf = [System.Runtime.InteropServices.Marshal].GetMethod('SizeOf', [type[]]@([System.Type]))
$mOffOf  = [System.Runtime.InteropServices.Marshal].GetMethod('OffsetOf', [type[]]@([System.Type], [string]))
$szDev  = $mSizeOf.Invoke($null, @([PnpSetup+SP_DEVINFO_DATA]))
$szDrv  = $mSizeOf.Invoke($null, @([PnpSetup+SP_DRVINFO_DATA]))
$szDet  = $mSizeOf.Invoke($null, @([PnpSetup+SP_DRVINFO_DETAIL_DATA]))
$offInf = $mOffOf.Invoke($null, @([PnpSetup+SP_DRVINFO_DETAIL_DATA], 'InfFileName'))
$offHw  = $mOffOf.Invoke($null, @([PnpSetup+SP_DRVINFO_DETAIL_DATA], 'HardwareID'))
Check 'SP_DEVINFO_DATA x64 sizeof == 32'            ($szDev -eq 32)   "sizeof=$szDev"
Check 'SP_DRVINFO_DATA x64 sizeof == 1568 (V2)'     ($szDrv -eq 1568) "sizeof=$szDrv"
Check 'SP_DRVINFO_DETAIL_DATA x64 sizeof == 1584'   ($szDet -eq 1584) "sizeof=$szDet"
Check 'DETAIL.InfFileName offset == 544'            ($offInf -eq 544) "offset=$offInf"
Check 'DETAIL.HardwareID offset == 1576'            ($offHw -eq 1576) "offset=$offHw"

# --- 6c. identity-verified target selection (in-memory FS via seams) -------
# Dependency-injected simulation of the published-INF -> driver store mapping
# and package file content identity. Nothing here touches a real Driver Store
# path, a real device, or any real file write: the production seams
# (mapping/reverse-lookup/existence/hashing/reading) are redirected to an
# in-memory FS (real package bytes seeded from $pkgDir, read-only) and the
# production natives are restored via ResetSeams() afterwards. These tests
# verify the DECISION logic only; the real SetupGetInfDriverStoreLocation /
# SetupGetInfPublishedName / on-disk SHA-256 reads are NOT executed here.
$script:vfs       = @{}   # path (lower-invariant) -> byte[]
$script:fsMap     = @{}   # candidate INF path (lower) -> store INF path
$script:fsRev     = @{}   # store INF path (lower)     -> published INF path
$script:fsMapFail = $false
$script:fsHashFail = @()  # paths (lower) whose hashing "fails" (read error)
function FsReset { $script:vfs = @{}; $script:fsMap = @{}; $script:fsRev = @{}; $script:fsMapFail = $false; $script:fsHashFail = @() }
function FsPut([string]$p, [byte[]]$b) { $script:vfs[$p.ToLowerInvariant()] = $b }
function FsSeed([string]$dir) {   # replicate the REAL package bytes at $dir
    foreach ($f in (Get-ChildItem -LiteralPath $pkgDir -File)) {
        FsPut (Join-Path $dir $f.Name) ([System.IO.File]::ReadAllBytes($f.FullName))
    }
}
[PnpSetup]::MapPublishedToStore = {
    param([string]$cand)
    $mr = New-Object PnpSetup+MapResult
    $mr.StoreInfPath = $null
    if ($script:fsMapFail) { $mr.Ok = $false; $mr.Err = 2; return $mr }
    $k = "$cand".ToLowerInvariant()
    if ($script:fsMap.ContainsKey($k)) {
        $v = $script:fsMap[$k]
        if ([string]::IsNullOrEmpty($v)) { $mr.Ok = $false; $mr.Err = -1; return $mr }
        $mr.Ok = $true; $mr.Err = 0; $mr.StoreInfPath = $v; return $mr
    }
    $mr.Ok = $false; $mr.Err = 2; return $mr
}
[PnpSetup]::LookupPublishedName = {
    param([string]$store)
    $k = "$store".ToLowerInvariant()
    if ($script:fsRev.ContainsKey($k)) { return $script:fsRev[$k] }
    return $null
}
[PnpSetup]::FileExists  = { param([string]$p) return $script:vfs.ContainsKey("$p".ToLowerInvariant()) }
[PnpSetup]::HashFile    = {
    param([string]$p)
    $k = "$p".ToLowerInvariant()
    if ($script:fsHashFail -contains $k) { return $null }
    if (-not $script:vfs.ContainsKey($k)) { return $null }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $h = $sha.ComputeHash($script:vfs[$k]) } finally { $sha.Dispose() }
    return (($h | ForEach-Object { $_.ToString('x2') }) -join '')
}
[PnpSetup]::ReadInfText = {
    param([string]$p)
    $k = "$p".ToLowerInvariant()
    if (-not $script:vfs.ContainsKey($k)) { return $null }
    return [System.Text.Encoding]::UTF8.GetString($script:vfs[$k])
}

function New-DrvRec([string]$Inf, [string]$Hwid, [string]$Section = 'TextDeck_Device.NT', [string]$Desc = 'Virtual HID Device') {
    $d = New-Object PnpSetup+DrvDetail
    $d.InfFileName = $Inf; $d.HardwareId = $Hwid
    # NB: PS coerces a $null assignment on a string field to ''; to model a
    # record whose install section is genuinely MISSING, pass $null and skip
    # the assignment entirely (the new object's field stays null).
    if ($null -ne $Section) { $d.SectionName = $Section }
    $d.Description = $Desc
    $d.ProviderName = 'Virtual HID Inc.'
    return $d
}
function New-DrvList([object[]]$Recs) {
    $l = New-Object 'System.Collections.Generic.List[PnpSetup+DrvDetail]'
    foreach ($x in $Recs) { [void]$l.Add($x) }
    return $l
}

# Seed the in-memory FS: the REAL target package bytes at their build paths,
# plus an identical replica as driver store package A published as oem57.inf
$hw = 'Root\TextDeckHidDriver'
$storeA = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testA'
$storeB = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testB'
$storeC = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testC'
$storeD = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testD'
$storeE = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testE'
$storeF = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testF'
$storeG = 'C:\Windows\System32\DriverStore\FileRepository\foreigndriver.inf_amd64_testG'
FsReset
FsSeed $pkgDir
FsSeed $storeA
$script:fsMap['c:\windows\inf\oem57.inf'] = "$storeA\TextDeckHidDriver.inf"
$script:fsRev["$storeA\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem57.inf'
$script:fsMap["$storeA\TextDeckHidDriver.INF".ToLowerInvariant()] = "$storeA\TextDeckHidDriver.inf"

$targetInfReal = Join-Path $pkgDir 'TextDeckHidDriver.inf'
$tid = [PnpSetup]::BuildIdentity($targetInfReal)
Check 'target identity established from real package bytes' ([string]::IsNullOrEmpty($tid.Error)) "err=$($tid.Error)"
Check 'target identity covers INF + declared CAT + SYS' ((($tid.KeyFileNames -join ',') -match 'TextDeckHidDriver\.cat') -and (($tid.KeyFileNames -join ',') -match 'TextDeckHidDriver\.sys')) "files=$($tid.KeyFileNames -join ',')"

# R1: standard target package: candidate C:\Windows\INF\oem57.inf maps to the
# correct driver store package, passes content identity -> uniquely chosen
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem57.inf' $hw)), $tid, $hw)
Check 'R1 oem57.inf -> store package identity-verified -> Code=0' (($sel.Result.Code -eq 0) -and ($sel.Result.Index -eq 0)) "code=$($sel.Result.Code) msg=$($sel.Result.Message)"
Check 'R1 outcome records the resolved store INF' ($sel.Outcomes[0].StoreInf -eq "$storeA\TextDeckHidDriver.inf") "store=$($sel.Outcomes[0].StoreInf)"

# R2: path/name case variants resolve to the same package (no prefix games)
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'c:\windows\INF\OEM57.INF' $hw)), $tid, $hw)
Check 'R2 case-insensitive published path still selects' ($sel.Result.Code -eq 0) "code=$($sel.Result.Code)"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec "$storeA\TextDeckHidDriver.INF" $hw)), $tid, $hw)
Check 'R2 store-form INF path also identity-verifies -> Code=0' ($sel.Result.Code -eq 0) "code=$($sel.Result.Code)"

# R3: same INF file NAME, different INF CONTENT -> rejected by content hash
FsSeed $storeB
$mutInf = [byte[]][System.IO.File]::ReadAllBytes((Join-Path $pkgDir 'TextDeckHidDriver.inf')).Clone()
$mutInf[100] = $mutInf[100] -bxor 0x20   # flip one bit inside the INF text
FsPut "$storeB\TextDeckHidDriver.inf" $mutInf
$script:fsMap['c:\windows\inf\oem58.inf'] = "$storeB\TextDeckHidDriver.inf"
$script:fsRev["$storeB\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem58.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem58.inf' $hw)), $tid, $hw)
Check 'R3 same-name different INF content -> rejected (INF hash)' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'INF content hash differs')) "reason=$($sel.Outcomes[0].Reason)"

# R4: identical INF but different SYS / different CAT -> rejected per declared file
FsSeed $storeC
$mutSys = [byte[]][System.IO.File]::ReadAllBytes((Join-Path $pkgDir 'TextDeckHidDriver.sys')).Clone()
$mutSys[64] = $mutSys[64] -bxor 0xFF
FsPut "$storeC\TextDeckHidDriver.sys" $mutSys
$script:fsMap['c:\windows\inf\oem59.inf'] = "$storeC\TextDeckHidDriver.inf"
$script:fsRev["$storeC\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem59.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem59.inf' $hw)), $tid, $hw)
Check 'R4 identical INF but different SYS -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'declared file: TextDeckHidDriver\.sys')) "reason=$($sel.Outcomes[0].Reason)"
FsSeed $storeD
$mutCat = [byte[]][System.IO.File]::ReadAllBytes((Join-Path $pkgDir 'textdeckhiddriver.cat')).Clone()
$mutCat[8] = $mutCat[8] -bxor 0x55
FsPut "$storeD\textdeckhiddriver.cat" $mutCat
$script:fsMap['c:\windows\inf\oem60.inf'] = "$storeD\TextDeckHidDriver.inf"
$script:fsRev["$storeD\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem60.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem60.inf' $hw)), $tid, $hw)
Check 'R4 identical INF but different CAT -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'declared file: TextDeckHidDriver\.cat')) "reason=$($sel.Outcomes[0].Reason)"

# R5: foreign hardware ID excluded before any identity work; a foreign
# package that claims OUR hardware ID still fails content identity
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem57.inf' 'ROOT\OTHER\DEVICE')), $tid, $hw)
Check 'R5 foreign hardware ID excluded' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'hardware ID differs')) "reason=$($sel.Outcomes[0].Reason)"
FsPut "$storeG\TextDeckHidDriver.inf" ([System.Text.Encoding]::UTF8.GetBytes("[Version]`r`nDriverVer = 01/01/2020,1.0.0.0`r`nCatalogFile=TextDeckHidDriver.cat`r`n[SourceDisksFiles]`r`nTextDeckHidDriver.sys = 1,,`r`n"))
FsPut "$storeG\TextDeckHidDriver.cat" ([byte[]](1,2,3,4))
FsPut "$storeG\TextDeckHidDriver.sys" ([byte[]](5,6,7,8))
$script:fsMap['c:\windows\inf\oem77.inf'] = "$storeG\TextDeckHidDriver.inf"
$script:fsRev["$storeG\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem77.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem77.inf' $hw)), $tid, $hw)
Check 'R5 foreign same-hwid package -> identity rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'package identity rejected')) "reason=$($sel.Outcomes[0].Reason)"

# R6: broken/failed mapping paths -> reject (mapping success is never identity)
$script:fsMapFail = $true
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem57.inf' $hw)), $tid, $hw)
Check 'R6a mapping API failure -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'SetupGetInfDriverStoreLocation failed.*error 2')) "reason=$($sel.Outcomes[0].Reason)"
$script:fsMapFail = $false
$script:fsMap['c:\windows\inf\oem62.inf'] = 'C:\nonexistent\dir\TextDeckHidDriver.inf'
$script:fsRev['c:\nonexistent\dir\textdeckhiddriver.inf'] = 'C:\Windows\INF\oem62.inf'   # round-trip OK; failure must come from FileExists
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem62.inf' $hw)), $tid, $hw)
Check 'R6b mapping to nonexistent store path -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'INF not found')) "reason=$($sel.Outcomes[0].Reason)"
$script:fsMap['c:\windows\inf\oem63.inf'] = ''
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem63.inf' $hw)), $tid, $hw)
Check 'R6c empty mapping result -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'SetupGetInfDriverStoreLocation failed')) "reason=$($sel.Outcomes[0].Reason)"
$script:fsRev["$storeA\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem99.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem57.inf' $hw)), $tid, $hw)
Check 'R6d published-name round-trip mismatch -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'round-trip mismatch')) "reason=$($sel.Outcomes[0].Reason)"
$script:fsRev.Remove("$storeA\TextDeckHidDriver.inf".ToLowerInvariant())
$script:fsMap['c:\windows\inf\oem65.inf'] = "$storeA\TextDeckHidDriver.inf"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem65.inf' $hw)), $tid, $hw)
Check 'R6e published candidate without reverse mapping -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'reverse lookup failed')) "reason=$($sel.Outcomes[0].Reason)"
$script:fsRev["$storeA\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem57.inf'

# R7: duplicate candidates for the SAME package de-duplicate by identity; two
# DISTINCT identical-content store packages are ambiguous -> refuse
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @((New-DrvRec 'C:\Windows\INF\oem57.inf' $hw), (New-DrvRec "$storeA\TextDeckHidDriver.INF" $hw))), $tid, $hw)
Check 'R7a alias entries of one package de-duplicate -> Code=0' (($sel.Result.Code -eq 0) -and ($sel.Outcomes.Count -eq 2) -and $sel.Outcomes[0].IdentityMatch -and $sel.Outcomes[1].IdentityMatch) "code=$($sel.Result.Code)"
FsSeed $storeE
$script:fsMap['c:\windows\inf\oem66.inf'] = "$storeE\TextDeckHidDriver.inf"
$script:fsRev["$storeE\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem66.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @((New-DrvRec 'C:\Windows\INF\oem57.inf' $hw), (New-DrvRec 'C:\Windows\INF\oem66.inf' $hw))), $tid, $hw)
Check 'R7b two distinct identical-content packages -> Code=3 ambiguous' ($sel.Result.Code -eq 3) "code=$($sel.Result.Code) msg=$($sel.Result.Message)"

# R8: incomplete package file sets -> fail closed (never identity-verified)
FsSeed $storeF
$script:vfs.Remove("$storeF\textdeckhiddriver.cat".ToLowerInvariant()) | Out-Null
$script:fsMap['c:\windows\inf\oem67.inf'] = "$storeF\TextDeckHidDriver.inf"
$script:fsRev["$storeF\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem67.inf'
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem67.inf' $hw)), $tid, $hw)
Check 'R8a missing declared CAT -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'declared package file missing: TextDeckHidDriver\.cat')) "reason=$($sel.Outcomes[0].Reason)"
FsSeed $storeF   # restore the CAT, then simulate a hash read failure on the SYS
$script:fsHashFail = @("$storeF\TextDeckHidDriver.sys".ToLowerInvariant())
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem67.inf' $hw)), $tid, $hw)
Check 'R8b declared-file hash read failure -> rejected' (($sel.Result.Code -eq 2) -and ($sel.Outcomes[0].Reason -match 'declared package file hash failed')) "reason=$($sel.Outcomes[0].Reason)"
$script:fsHashFail = @()

# R9: distribution-media layout vs Driver Store FLAT layout. The INF declares
# a media-relative source path (SourceDisksNames path + SourceDisksFiles
# subdir), yet the same bytes stored FLAT (store) and SUBLAYOUTED (media)
# must both establish an identity -- and the identities must link by content.
$nl = "`r`n"
$layoutLines = @(
    '[Version]'
    'DriverVer = 01/01/2020,1.0.0.0'
    'CatalogFile=pkg01.cat'
    '[SourceDisksNames]'
    '1 = %DiskName%,,,"\WinNT"'
    '[SourceDisksFiles]'
    'sub.sys = 1,"\x86"'
    '[DDInstall]'
    'CopyFiles=FL'
    '[FL]'
    'sub.sys'
    '[Strings]'
    'DiskName = "Disc 1"'
) -join $nl
$layoutBytes = [System.Text.Encoding]::UTF8.GetBytes($layoutLines)
$storeH = 'C:\Windows\System32\DriverStore\FileRepository\pkg01.inf_amd64_testH'
$mediaRoot = 'D:\media\disc01'                    # fake ORIGINAL media layout (subdirs)
FsPut "$storeH\pkg01.inf" $layoutBytes            # Driver Store package: FLAT
FsPut "$storeH\pkg01.cat" ([byte[]](9,9,9,9))
FsPut "$storeH\sub.sys"   ([byte[]](7,7,7,7))
FsPut "$mediaRoot\pkg01.inf" $layoutBytes         # media package: WinNT\x86 subdirs
FsPut "$mediaRoot\pkg01.cat" ([byte[]](9,9,9,9))
FsPut "$mediaRoot\WinNT\x86\sub.sys" ([byte[]](7,7,7,7))
$idFlat = [PnpSetup]::BuildIdentity("$storeH\pkg01.inf")
$idOrig = [PnpSetup]::BuildIdentity("$mediaRoot\pkg01.inf")
Check 'R9a flat Driver Store layout: identity established' ([string]::IsNullOrEmpty($idFlat.Error)) "err=$($idFlat.Error)"
Check 'R9b media layout: identity established via composed source path' ([string]::IsNullOrEmpty($idOrig.Error)) "err=$($idOrig.Error)"
$why9 = 'x'
Check 'R9c both layouts link to ONE content identity (hashes equal)' ([PnpSetup]::IdentityMatches($idFlat, $idOrig, [ref]$why9)) "why=$why9"
# traversal inside the media path must never resolve outside the root
$evilLines = @(
    '[Version]'
    'DriverVer = 01/01/2020,1.0.0.0'
    'CatalogFile=pkg02.cat'
    '[SourceDisksNames]'
    '1 = "d",,,"\bin"'
    '[SourceDisksFiles]'
    'sub.sys = 1,"..\..\escape"'
) -join $nl
$storeJ = 'C:\Windows\System32\DriverStore\FileRepository\pkg02.inf_amd64_testJ'
FsPut "$storeJ\pkg02.inf" ([System.Text.Encoding]::UTF8.GetBytes($evilLines))
FsPut "$storeJ\pkg02.cat" ([byte[]](1,1,1,1))
FsPut "$storeJ\sub.sys"   ([byte[]](2,2,2,2))
$idEvil = [PnpSetup]::BuildIdentity("$storeJ\pkg02.inf")
Check 'R9d traversal in SourceDisksFiles subdir -> identity REFUSED with reason' ((-not [string]::IsNullOrEmpty($idEvil.Error)) -and ($idEvil.Error -match 'traversal')) "err=$($idEvil.Error)"

# ONE identity-verified package referenced by MULTIPLE driver records.
# Same package does NOT imply same install semantics: only records that all
# report the SAME non-empty install section may de-duplicate; anything else
# is an explicit ambiguity refusal (never a first-entry pick).
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @((New-DrvRec 'C:\Windows\INF\oem57.inf' $hw), (New-DrvRec "$storeA\TextDeckHidDriver.INF" $hw))), $tid, $hw)
Check 'R10a all verified entries share one section -> Code=0 + section named' (($sel.Result.Code -eq 0) -and ($sel.Result.Message -match "install section 'TextDeck_Device\.NT' unique") -and ($sel.Result.Message -match 'de-duplicated')) "code=$($sel.Result.Code) msg=$($sel.Result.Message)"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @((New-DrvRec 'C:\Windows\INF\oem57.inf' $hw 'TextDeck_Device.NT'), (New-DrvRec "$storeA\TextDeckHidDriver.INF" $hw 'TextDeckHidDriver_Alt.NT'))), $tid, $hw)
Check 'R10b different install sections -> Code=3, Index=-1, both sections listed' (($sel.Result.Code -eq 3) -and ($sel.Result.Index -eq -1) -and ($sel.Result.Message -match 'DIFFERENT install sections') -and ($sel.Result.Message -match 'TextDeck_Device\.NT') -and ($sel.Result.Message -match 'TextDeckHidDriver_Alt\.NT') -and ($sel.Result.Message -match 'refusing to choose arbitrarily')) "code=$($sel.Result.Code) msg=$($sel.Result.Message)"
Check 'R10b DiInstallDevice unreachable on ambiguity (Index stays -1)' ($sel.Result.Index -eq -1) "index=$($sel.Result.Index)"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem57.inf' $hw $null)), $tid, $hw)
Check 'R10c verified record with MISSING section name -> Code=3 refused' (($sel.Result.Code -eq 3) -and ($sel.Result.Index -eq -1) -and ($sel.Result.Message -match 'no install section name')) "code=$($sel.Result.Code) msg=$($sel.Result.Message)"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @((New-DrvRec 'C:\Windows\INF\oem57.inf' $hw), (New-DrvRec "$storeA\TextDeckHidDriver.INF" $hw), (New-DrvRec 'C:\Windows\INF\oem57.inf' $hw 'Other.NT'))), $tid, $hw)
Check 'R10d 2+1 record split -> Code=3, differing records identified by index' (($sel.Result.Code -eq 3) -and ($sel.Result.Message -match "in \[0\]") -and ($sel.Result.Message -match "'Other\.NT' in \[2\]")) "msg=$($sel.Result.Message)"
$nullRecs = New-Object 'System.Collections.Generic.List[PnpSetup+DrvDetail]'
[void]$nullRecs.Add($null)   # NB: @($null) is an EMPTY array in PS -- add explicitly
$sel = [PnpSetup]::SelectTargetDriver($nullRecs, $tid, $hw)
Check 'R10e null driver record -> Code=2 fail closed (field incompleteness)' (($sel.Result.Code -eq 2) -and ($sel.Result.Message -match 'driver record is null')) "msg=$($sel.Result.Message)"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec '' $hw)), $tid, $hw)
Check 'R10f record without INF name -> Code=2 fail closed (detail incomplete)' (($sel.Result.Code -eq 2) -and ($sel.Result.Message -match 'driver detail reported no INF name')) "msg=$($sel.Result.Message)"

# A CopyINF reference blocks the WHOLE identity chain: the
# external package(s) it recursively imports cannot be inventoried or hashed
# here, so BuildIdentity refuses with NO key files, the package can never
# match the target identity, and a driver record pointing at it can never
# reach candidate selection / install (Code=2, Index=-1).
$storeK = 'C:\Windows\System32\DriverStore\FileRepository\textdeckhiddriver.inf_amd64_testK'
FsSeed $storeK
$realInfText9 = [System.IO.File]::ReadAllText((Join-Path $pkgDir 'TextDeckHidDriver.inf'))
$copyInfText9 = $realInfText9 + "`r`n[Round9CopyInf]`r`nCopyINF = extra.inf`r`n"
FsPut "$storeK\TextDeckHidDriver.inf" ([System.Text.Encoding]::UTF8.GetBytes($copyInfText9))
$script:fsMap['c:\windows\inf\oem90.inf'] = "$storeK\TextDeckHidDriver.inf"
$script:fsRev["$storeK\TextDeckHidDriver.inf".ToLowerInvariant()] = 'C:\Windows\INF\oem90.inf'
$idK = [PnpSetup]::BuildIdentity("$storeK\TextDeckHidDriver.inf")
Check 'R11a CopyINF package -> BuildIdentity REFUSED with the named directive' ((-not [string]::IsNullOrEmpty($idK.Error)) -and ($idK.Error -match 'CopyINF') -and ($idK.Error -match 'external driver package')) "err=$($idK.Error)"
Check 'R11b refused identity exposes ZERO key files (never a partial hash set)' ($idK.KeyFileNames.Count -eq 0) "count=$($idK.KeyFileNames.Count)"
$why11 = 'x'
$m11 = [PnpSetup]::IdentityMatches($tid, $idK, [ref]$why11)
Check 'R11c CopyINF package can never match the target identity' ((-not $m11) -and ($why11 -match 'candidate identity unavailable')) "why=$why11"
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @(New-DrvRec 'C:\Windows\INF\oem90.inf' $hw)), $tid, $hw)
Check 'R11d CopyINF record -> selection Code=2 + Index=-1, reason names CopyINF' (($sel.Result.Code -eq 2) -and ($sel.Result.Index -eq -1) -and ($sel.Outcomes[0].Reason -match 'CopyINF')) "code=$($sel.Result.Code) idx=$($sel.Result.Index) reason=$($sel.Outcomes[0].Reason)"

# regression: empty driver list -> refuse
$sel = [PnpSetup]::SelectTargetDriver((New-DrvList @()), $tid, $hw)
Check 'selection: empty driver list -> Code=2' ($sel.Result.Code -eq 2) "code=$($sel.Result.Code)"

[PnpSetup]::ResetSeams()   # restore the production natives/readers/hashes

# --- 6d. hardware-ID probe classification (every documented branch) ---
Check 'probe: FALSE+122+size -> proceed (expected probe)'    ([PnpSetup]::ClassifyHwidProbe($false, 122, 64) -eq 1) 'documented buffer-size probe'
Check 'probe: FALSE+122+size0 -> fail closed'                ([PnpSetup]::ClassifyHwidProbe($false, 122, 0) -eq 0) 'insufficient buffer without size is indeterminate'
Check 'probe: FALSE+13 -> fail closed (absent OR invalid)'   ([PnpSetup]::ClassifyHwidProbe($false, 13, 0) -eq 0) 'ERROR_INVALID_DATA can never prove absence'
Check 'probe: FALSE+259 -> fail closed (undocumented here)'  ([PnpSetup]::ClassifyHwidProbe($false, 259, 0) -eq 0) 'ERROR_NO_MORE_ITEMS is not an absence proof for this API'
Check 'probe: FALSE+5 -> fail closed (access denied)'        ([PnpSetup]::ClassifyHwidProbe($false, 5, 0) -eq 0) 'real read error'
Check 'probe: TRUE+size -> proceed'                          ([PnpSetup]::ClassifyHwidProbe($true, 0, 64) -eq 1) 'consistent probe'
Check 'probe: TRUE+size0 -> fail closed (inconsistent)'      ([PnpSetup]::ClassifyHwidProbe($true, 0, 0) -eq 0) 'success with no size is indeterminate'

# --- 6e. exact full-identifier comparison (no substring/prefix matches) ---
Check 'hwid: case-insensitive equality'          ([PnpSetup]::HwidEquals('Root\TextDeckHidDriver', 'root\textdeckhiddriver')) 'ORDINAL ignore-case equality'
Check 'hwid: instance-suffixed ID NOT equal'     (-not [PnpSetup]::HwidEquals('Root\TextDeckHidDriver\0000', 'Root\TextDeckHidDriver')) 'full identifier compare'
Check 'hwid: longer same-prefix ID NOT equal'    (-not [PnpSetup]::HwidEquals('Root\TextDeckHidDriver2', 'Root\TextDeckHidDriver')) 'no prefix/substring match'
Check 'hwid: MULTI_SZ exact member match'        ([PnpSetup]::MultiSzContainsExact("foo`0Root\TextDeckHidDriver`0bar`0", 'Root\TextDeckHidDriver')) 'exact member found'
Check 'hwid: MULTI_SZ substring-only rejected'   (-not [PnpSetup]::MultiSzContainsExact("Root\TextDeckHidDriverX`0", 'Root\TextDeckHidDriver')) 'prefix member rejected'
Check 'hwid: MULTI_SZ case-insensitive member'   ([PnpSetup]::MultiSzContainsExact("ROOT\TEXTDECKHIDDRIVER`0", 'Root\TextDeckHidDriver')) 'ignore-case member found'

# --- 6f. INF package inventory parser (documented subset, fail-closed) -----
# Deliberately LIMITED parser (not a full Windows INF parser): every construct
# outside the documented subset must fail closed WITH a reason; these checks
# assert the reason text, not just "returned an error".
$infTextReal = [System.IO.File]::ReadAllText((Join-Path $pkgDir 'TextDeckHidDriver.inf'))   # read-only
function Get-InvLeaves($Inv) { return ((@($Inv) | ForEach-Object { $_.LeafName }) -join ',') }
function Test-LeafSet([string]$Actual, [string[]]$Expect) {
    $a = @("$Actual" -split ',' | ForEach-Object { "$_".Trim() } | Where-Object { $_ } | Sort-Object)
    $e = @($Expect | Sort-Object)
    if ($a.Count -ne $e.Count) { return $false }
    for ($i = 0; $i -lt $a.Count; $i++) { if ($a[$i] -ne $e[$i]) { return $false } }
    return $true
}
function New-Inf([string[]]$Lines) { return (($Lines -join "`r`n") + "`r`n") }
function Get-Decl($Inv, [string]$Leaf) {
    return (@($Inv) | Where-Object { $_.LeafName -eq $Leaf }) | Select-Object -First 1
}

# -- the REAL project INF parses fully (no unsupported construct present) ----
$perr = 'sentinel'
$inv = [PnpSetup]::ParseInfInventory($infTextReal, [ref]$perr)
Check '6f real INF: inventory parses with no error' ($null -eq $perr) "err=$perr"
Check '6f real INF: declared set = the INF own CAT + SYS, each once' (Test-LeafSet (Get-InvLeaves $inv) @('TextDeckHidDriver.cat','TextDeckHidDriver.sys')) "leaves=$(Get-InvLeaves $inv)"
$sysDecl = Get-Decl $inv 'TextDeckHidDriver.sys'
Check '6f real INF: SYS source path = package root (empty subdir + empty disk path)' (($null -ne $sysDecl) -and ($sysDecl.SourceRelPath -eq '') -and ($sysDecl.Via -eq 'SourceDisksFiles')) "rel=$($sysDecl.SourceRelPath) via=$($sysDecl.Via)"
Check '6f real INF: CAT recorded via CatalogFile directive' ((Get-Decl $inv 'TextDeckHidDriver.cat').Via -eq 'CatalogFile') "via=$((Get-Decl $inv 'TextDeckHidDriver.cat').Via)"
$perrDup = 'sentinel'; $invDupA = [PnpSetup]::ParseInfInventory($infTextReal, [ref]$perrDup)
$perrDup2 = 'sentinel'; $invDupB = [PnpSetup]::ParseInfInventory($infTextReal, [ref]$perrDup2)
Check '6f real INF: inventory deterministic across runs (same order, same count)' ((Get-InvLeaves $invDupA) -eq (Get-InvLeaves $invDupB)) "a=$(Get-InvLeaves $invDupA) b=$(Get-InvLeaves $invDupB)"

# -- C1.1/C1.2 SourceDisksFiles subdir variants (empty vs non-empty) ---------
$inf1 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = %DiskName%,,,""','[SourceDisksFiles]','a.sys = 1,,','[Strings]','DiskName = "D1"')
$perr1 = 'sentinel'; $inv1 = [PnpSetup]::ParseInfInventory($inf1, [ref]$perr1)
Check '6f C1.1 empty subdir -> rel path is the package root' (($null -eq $perr1) -and ((Get-Decl $inv1 'a.sys').SourceRelPath -eq '')) "err=$perr1 rel=$((Get-Decl $inv1 'a.sys').SourceRelPath)"
$inf2 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1,"bin"')
$perr2 = 'sentinel'; $inv2 = [PnpSetup]::ParseInfInventory($inf2, [ref]$perr2)
Check '6f C1.2 non-empty subdir -> media-relative subdir recorded' (($null -eq $perr2) -and ((Get-Decl $inv2 'a.sys').SourceRelPath -eq 'bin')) "err=$perr2 rel=$((Get-Decl $inv2 'a.sys').SourceRelPath)"
# -- C1.3/C1.4 SourceDisksNames path alone and combined with subdir ----------
$inf3 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,"\WinNT"','[SourceDisksFiles]','a.sys = 1')
$perr3 = 'sentinel'; $inv3 = [PnpSetup]::ParseInfInventory($inf3, [ref]$perr3)
Check '6f C1.3 SourceDisksNames path used as source root' (($null -eq $perr3) -and ((Get-Decl $inv3 'a.sys').SourceRelPath -eq 'WinNT')) "err=$perr3 rel=$((Get-Decl $inv3 'a.sys').SourceRelPath)"
$inf4 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,"\WinNT"','[SourceDisksFiles]','a.sys = 1,"x86"')
$perr4 = 'sentinel'; $inv4 = [PnpSetup]::ParseInfInventory($inf4, [ref]$perr4)
Check '6f C1.4 disk path + subdir compose to WinNT\x86' (($null -eq $perr4) -and ((Get-Decl $inv4 'a.sys').SourceRelPath -eq 'WinNT\x86')) "err=$perr4 rel=$((Get-Decl $inv4 'a.sys').SourceRelPath)"
# -- C1.5 CopyFiles: SOURCE name is identity-relevant, destination is not ----
$inf5 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=FL','[FL]','dst.sys,src.sys,0x00000001')
$perr5 = 'sentinel'; $inv5 = [PnpSetup]::ParseInfInventory($inf5, [ref]$perr5)
Check '6f C1.5 CopyFiles source name != dest: source hashed, dest ignored' (($null -eq $perr5) -and (Test-LeafSet (Get-InvLeaves $inv5) @('k.cat','src.sys')) -and ($null -eq (Get-Decl $inv5 'dst.sys'))) "err=$perr5 leaves=$(Get-InvLeaves $inv5)"
# -- C1.6 CopyFiles documented alternatives (docs: @filename | sections) ------
# NB: the previously tested MIXED form '@direct.dll,FL' is NOT valid
# syntax (the official CopyFiles directive defines @filename and file-list
# section names as mutually exclusive alternatives) - the valid forms are
# tested separately here and the mixed form is REFUSED (see C1.16).
$inf6 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=@direct.dll')
$perr6 = 'sentinel'; $inv6 = [PnpSetup]::ParseInfInventory($inf6, [ref]$perr6)
Check '6f C1.6a CopyFiles=@filename (single direct-copy file) inventoried' (($null -eq $perr6) -and (Test-LeafSet (Get-InvLeaves $inv6) @('k.cat','direct.dll'))) "err=$perr6 leaves=$(Get-InvLeaves $inv6)"
$inf6b = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=FL,FL2','[FL]','one.sys','[FL2]','two.sys')
$perr6b = 'sentinel'; $inv6b = [PnpSetup]::ParseInfInventory($inf6b, [ref]$perr6b)
Check '6f C1.6b CopyFiles with MULTIPLE file-list sections inventoried' (($null -eq $perr6b) -and (Test-LeafSet (Get-InvLeaves $inv6b) @('k.cat','one.sys','two.sys'))) "err=$perr6b leaves=$(Get-InvLeaves $inv6b)"
# -- C1.7 one file referenced by SDF + CopyFiles + ServiceBinary: ONE decl ---
$inf7 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','dup.sys = 1,,','[DDInstall]','CopyFiles=FL','[FL]','dup.sys','[Service]','ServiceBinary = %12%\dup.sys')
$perr7 = 'sentinel'; $inv7 = [PnpSetup]::ParseInfInventory($inf7, [ref]$perr7)
$dupCount = @($inv7 | Where-Object { $_.LeafName -eq 'dup.sys' }).Count
Check '6f C1.7 triply-declared file yields exactly ONE deterministic entry' (($null -eq $perr7) -and ($dupCount -eq 1) -and ((Get-Decl $inv7 'dup.sys').Via -eq 'SourceDisksFiles')) "err=$perr7 count=$dupCount via=$((Get-Decl $inv7 'dup.sys').Via)"
# -- C1.8 architecture decorations: .amd64 applies+overrides, .x86 excluded --
$inf8 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1','[SourceDisksFiles.amd64]','a.sys = 1,"amd64bin"','[SourceDisksFiles.x86]','x86only.sys = 1')
$perr8 = 'sentinel'; $inv8 = [PnpSetup]::ParseInfInventory($inf8, [ref]$perr8)
Check '6f C1.8a .amd64 section overrides generic entry for same file' (($null -eq $perr8) -and ((Get-Decl $inv8 'a.sys').SourceRelPath -eq 'amd64bin')) "err=$perr8 rel=$((Get-Decl $inv8 'a.sys').SourceRelPath)"
Check '6f C1.8b other-architecture section (.x86) excluded from identity' ($null -eq (Get-Decl $inv8 'x86only.sys')) "leaves=$(Get-InvLeaves $inv8)"
$inf8b = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles.ntamd64]','a.sys = 1')
$perr8b = 'sentinel'
[PnpSetup]::ParseInfInventory($inf8b, [ref]$perr8b) | Out-Null
Check '6f C1.8c .ntamd64 decoration on SourceDisksFiles -> rejected with reason' ($perr8b -match "unsupported decoration '\.ntamd64'") "err=$perr8b"
# -- C1.9 multi-level [Strings] expansion ------------------------------------
$inf9 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=%C%.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1','[Strings]','C = %B%_drv','B = VHID')
$perr9 = 'sentinel'; $inv9 = [PnpSetup]::ParseInfInventory($inf9, [ref]$perr9)
Check '6f C1.9 recursive %token% expansion (C -> B -> value)' (($null -eq $perr9) -and (Test-LeafSet (Get-InvLeaves $inv9) @('VHID_drv.cat','a.sys'))) "err=$perr9 leaves=$(Get-InvLeaves $inv9)"
# -- C1.9b decorated CatalogFile directive (CatalogFile.NTamd64) ------------
$inf9b = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile.NTamd64 = %MyCat%.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1','[Strings]','MyCat = "myc"')
$perr9b = 'sentinel'; $inv9b = [PnpSetup]::ParseInfInventory($inf9b, [ref]$perr9b)
Check '6f C1.9b CatalogFile.NTamd64 + token -> decorated catalog inventoried' (($null -eq $perr9b) -and (Test-LeafSet (Get-InvLeaves $inv9b) @('myc.cat','a.sys'))) "err=$perr9b leaves=$(Get-InvLeaves $inv9b)"
# -- C1.10 comments, quotes, duplicate entries, legal comma fields ----------
$inf10 = New-Inf @('; leading comment','[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile = "k.cat" ; trailing comment','[SourceDisksNames]','1 = "Disk, One",tag,,""','[SourceDisksFiles]','"q.sys" = 1,,','q.sys = 1,, ; duplicate, same value')
$perr10 = 'sentinel'; $inv10 = [PnpSetup]::ParseInfInventory($inf10, [ref]$perr10)
$qCount = @($inv10 | Where-Object { $_.LeafName -eq 'q.sys' }).Count
Check '6f C1.10 comments+quoted names+comma-in-quotes+identical duplicate' (($null -eq $perr10) -and (Test-LeafSet (Get-InvLeaves $inv10) @('k.cat','q.sys')) -and ($qCount -eq 1)) "err=$perr10 leaves=$(Get-InvLeaves $inv10) q=$qCount"
# -- C1.11 unresolvable tokens / structurally broken INF / bad ServiceBinary -
$inf11a = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1,"%Sub%"')
$perr11a = 'sentinel'
[PnpSetup]::ParseInfInventory($inf11a, [ref]$perr11a) | Out-Null
Check '6f C1.11a unknown macro in subdir -> rejected with token reason' ($perr11a -match 'unresolvable token') "err=$perr11a"
$perr11b = 'sentinel'
[PnpSetup]::ParseInfInventory("OrphanKey = 1`r`n[Version]`r`nCatalogFile=k.cat`r`n", [ref]$perr11b) | Out-Null
Check '6f C1.11b entry before any section -> rejected as broken INF' ($perr11b -match 'outside any section') "err=$perr11b"
$inf11c = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[Service]','ServiceBinary = %12%\%Drv%.sys')
$perr11c = 'sentinel'
[PnpSetup]::ParseInfInventory($inf11c, [ref]$perr11c) | Out-Null
Check '6f C1.11c token inside ServiceBinary leaf -> rejected' ($perr11c -match 'contains a % token') "err=$perr11c"
# -- C1.12 path traversal / drive-absolute / UNC source paths ---------------
$inf12a = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1,"..\..\x"')
$perr12a = 'sentinel'
[PnpSetup]::ParseInfInventory($inf12a, [ref]$perr12a) | Out-Null
Check '6f C1.12a subdir traversal (..) -> rejected' ($perr12a -match "traversal \('\.\.'\) rejected") "err=$perr12a"
$inf12b = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,"C:\abs"','[SourceDisksFiles]','a.sys = 1')
$perr12b = 'sentinel'
[PnpSetup]::ParseInfInventory($inf12b, [ref]$perr12b) | Out-Null
Check '6f C1.12b drive-absolute disk path -> rejected' ($perr12b -match 'drive-absolute source path rejected') "err=$perr12b"
$inf12c = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,"\\srv\share"','[SourceDisksFiles]','a.sys = 1')
$perr12c = 'sentinel'
[PnpSetup]::ParseInfInventory($inf12c, [ref]$perr12c) | Out-Null
Check '6f C1.12c UNC disk path -> rejected' ($perr12c -match 'UNC source path rejected') "err=$perr12c"
# -- C1.13 unsupported constructs: Include/Needs/LayoutFile/cab -------------
$perr13a = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','Needs = system.inf','[SourceDisksFiles]','a.sys = 1')), [ref]$perr13a) | Out-Null
Check '6f C1.13a Needs= directive -> rejected (external INF unauditable)' ($perr13a -match 'cannot be inventoried') "err=$perr13a"
$perr13b = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','Include = other.inf')), [ref]$perr13b) | Out-Null
Check '6f C1.13b Include= directive -> rejected with reason' ($perr13b -match 'external INFs cannot be inventoried') "err=$perr13b"
$perr13c = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','LayoutFile = extra.inx')), [ref]$perr13c) | Out-Null
Check '6f C1.13c LayoutFile -> rejected (system-supplied layout unsupported)' ($perr13c -match 'system-supplied INF layout is unsupported') "err=$perr13c"
$perr13d = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d","files.cab",,','[SourceDisksFiles]','a.sys = 1')), [ref]$perr13d) | Out-Null
Check '6f C1.13d .cab source cabinet -> rejected (files not loose content)' ($perr13d -match 'cannot be identity-verified') "err=$perr13d"
# -- C1.13e/f/g missing/undefined references fail closed --------------------
$perr13e = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1')), [ref]$perr13e) | Out-Null
Check '6f C1.13e missing CatalogFile directive -> rejected' ($perr13e -match 'declares no CatalogFile directive') "err=$perr13e"
$perr13f = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksFiles]','a.sys = 7')), [ref]$perr13f) | Out-Null
Check '6f C1.13f undefined disk id -> rejected (SourceDisksNames required)' ($perr13f -match 'not defined in \[SourceDisksNames\]') "err=$perr13f"
$perr13g = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=Nope')), [ref]$perr13g) | Out-Null
Check '6f C1.13g CopyFiles to missing file-list section -> rejected' ($perr13g -match 'missing file-list section') "err=$perr13g"
# -- C1.13h/i duplicate/invalid entries with divergent or broken fields -----
$perr13h = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','2 = "d",,,"\bin"','[SourceDisksFiles]','a.sys = 1','a.sys = 2')), [ref]$perr13h) | Out-Null
Check '6f C1.13h same file twice with DIFFERENT source paths -> rejected' ($perr13h -match 'defined twice with different source paths') "err=$perr13h"
$perr13i = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=FL','[FL]','x.sys = y.sys')), [ref]$perr13i) | Out-Null
Check '6f C1.13i equal-sign entry inside file-list section -> rejected' ($perr13i -match "not valid in a file-list section") "err=$perr13i"
# -- C1.13j filename constraints (tokens/paths are never valid file names) ---
$perr13j = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','%Name%.sys = 1')), [ref]$perr13j) | Out-Null
Check '6f C1.13j %token% as SourceDisksFiles file NAME -> rejected (docs forbid)' ($perr13j -match 'must be literal') "err=$perr13j"
$perr13k = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','sub\a.sys = 1')), [ref]$perr13k) | Out-Null
Check '6f C1.13k path characters inside a file NAME -> rejected' ($perr13k -match 'not a plain file name') "err=$perr13k"
# -- C1.14 CopyINF: explicit fail-closed (docs: INF CopyINF directive) --------
# (a) the REAL project INF uses no CopyINF/Include/Needs/LayoutFile -> null
$perrExt = 'sentinel'
$realExt = [PnpSetup]::RejectExternalInfRefs([PnpSetup]::TokenizeInf($infTextReal))
Check '6f C1.14a real project INF: no CopyINF/Include/Needs/LayoutFile (null)' ($null -eq $realExt) "ext=$realExt"
# (b) a non-empty CopyINF directive is REFUSED with the named directive + why
$perr14 = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyINF = other.inf')), [ref]$perr14) | Out-Null
Check '6f C1.14b non-empty CopyINF -> refused, directive + reason named' (($perr14 -match 'CopyINF') -and ($perr14 -match 'external driver package')) "err=$perr14"
# (c) casing/whitespace/quotes/multi-value/section-scope cannot bypass the scan
$variantInfs = @{
    'lowercase key'     = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','copyinf=other.inf')
    'mixed-case key'    = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyInf = other.inf')
    'quoted key'        = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','"CopyINF" = other.inf')
    'upper+multi-value' = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','COPYINF = other.inf,b.inf')
    'outside DDInstall' = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[CustomSection]','CopyINF=other.inf')
}
foreach ($vk in ($variantInfs.Keys | Sort-Object)) {
    $pv = 'sentinel'
    [PnpSetup]::ParseInfInventory($variantInfs[$vk], [ref]$pv) | Out-Null
    Check "6f C1.14c CopyINF variant [$vk] cannot bypass" (($pv -match 'CopyINF') -and ($pv -match 'external driver package')) "err=$pv"
}
# (d) CopyINF inside a COMMENT must not trigger the refusal
$inf14d = New-Inf @('; CopyINF=other.inf','[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1')
$perr14d = 'sentinel'; $inv14d = [PnpSetup]::ParseInfInventory($inf14d, [ref]$perr14d)
Check '6f C1.14d CopyINF inside a comment does NOT trigger refusal' (($null -eq $perr14d) -and (Test-LeafSet (Get-InvLeaves $inv14d) @('k.cat','a.sys'))) "err=$perr14d leaves=$(Get-InvLeaves $inv14d)"
# (e) an EMPTY CopyINF value carries no references -> not refused (same
#     convention as an empty Include/Needs); it still adds NO files
$inf14e = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyINF =','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1')
$perr14e = 'sentinel'; $inv14e = [PnpSetup]::ParseInfInventory($inf14e, [ref]$perr14e)
Check '6f C1.14e EMPTY CopyINF value -> not refused, no files added' (($null -eq $perr14e) -and (Test-LeafSet (Get-InvLeaves $inv14e) @('k.cat','a.sys'))) "err=$perr14e leaves=$(Get-InvLeaves $inv14e)"
# (f) end-to-end blocking (BuildIdentity -> candidate selection) is covered
#     in section [6c] on the in-memory FS.
# -- C1.15 SourceDisksNames architecture priority (per-disk-id, docs) --------
$inf15a = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,"\WinNT"','[SourceDisksFiles]','a.sys = 1')
$perr15a = 'sentinel'; $inv15a = [PnpSetup]::ParseInfInventory($inf15a, [ref]$perr15a)
Check '6f C1.15a generic-only SourceDisksNames parses (baseline)' (($null -eq $perr15a) -and ((Get-Decl $inv15a 'a.sys').SourceRelPath -eq 'WinNT')) "err=$perr15a rel=$((Get-Decl $inv15a 'a.sys').SourceRelPath)"
# (b) SAME disk id in generic + .amd64 with DIFFERENT paths: the .amd64
#     definition WINS (legal override, previously misreported as a conflict)
$inf15b = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksNames.amd64]','1 = "d",,,"\amd64dir"','[SourceDisksFiles]','a.sys = 1')
$perr15b = 'sentinel'; $inv15b = [PnpSetup]::ParseInfInventory($inf15b, [ref]$perr15b)
Check '6f C1.15b .amd64 override of same disk id is LEGAL (uses amd64 path)' (($null -eq $perr15b) -and ((Get-Decl $inv15b 'a.sys').SourceRelPath -eq 'amd64dir')) "err=$perr15b rel=$((Get-Decl $inv15b 'a.sys').SourceRelPath)"
# (c) disk id missing from .amd64 falls back to the generic definition
$inf15c = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','2 = "d",,,"\gen2"','[SourceDisksNames.amd64]','3 = "d",,,"\amd3"','[SourceDisksFiles]','a.sys = 1','b.sys = 2','c.sys = 3')
$perr15c = 'sentinel'; $inv15c = [PnpSetup]::ParseInfInventory($inf15c, [ref]$perr15c)
Check '6f C1.15c id absent in .amd64 falls back to generic; amd64-only id resolves' (($null -eq $perr15c) -and ((Get-Decl $inv15c 'a.sys').SourceRelPath -eq '') -and ((Get-Decl $inv15c 'b.sys').SourceRelPath -eq 'gen2') -and ((Get-Decl $inv15c 'c.sys').SourceRelPath -eq 'amd3')) "err=$perr15c a=$((Get-Decl $inv15c 'a.sys').SourceRelPath) b=$((Get-Decl $inv15c 'b.sys').SourceRelPath) c=$((Get-Decl $inv15c 'c.sys').SourceRelPath)"
# (d) conflicting duplicate WITHIN the .amd64 section -> ambiguous, refused
$perr15d = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksNames.amd64]','1 = "d",,,"\one"','1 = "d",,,"\two"','[SourceDisksFiles]','a.sys = 1')), [ref]$perr15d) | Out-Null
Check '6f C1.15d conflicting duplicate INSIDE .amd64 section -> refused' (($perr15d -match 'defined twice in the same section') -and ($perr15d -match 'SourceDisksNames\.amd64')) "err=$perr15d"
# (e) conflicting duplicate WITHIN the generic section -> ambiguous, refused
$perr15e = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','1 = "d",,,"\x"','[SourceDisksFiles]','a.sys = 1')), [ref]$perr15e) | Out-Null
Check '6f C1.15e conflicting duplicate INSIDE generic section -> refused' (($perr15e -match 'defined twice in the same section') -and ($perr15e -match '\[SourceDisksNames\]')) "err=$perr15e"
# (e2) an IDENTICAL duplicate within one section is harmless -> not refused
$inf15e2 = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','1 = "d",,,','[SourceDisksFiles]','a.sys = 1')
$perr15e2 = 'sentinel'; $inv15e2 = [PnpSetup]::ParseInfInventory($inf15e2, [ref]$perr15e2)
Check '6f C1.15f identical duplicate within one section stays legal' (($null -eq $perr15e2) -and ((Get-Decl $inv15e2 'a.sys').SourceRelPath -eq '')) "err=$perr15e2"
# (f) non-target-architecture sections never override .amd64 and never
#     provide disk ids for this target
$inf15f = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksNames.amd64]','1 = "d",,,"\amd64dir"','[SourceDisksNames.x86]','1 = "d",,,"\x86dir"','[SourceDisksFiles]','a.sys = 1')
$perr15f = 'sentinel'; $inv15f = [PnpSetup]::ParseInfInventory($inf15f, [ref]$perr15f)
Check '6f C1.15g .x86 definition does NOT override the .amd64 definition' (($null -eq $perr15f) -and ((Get-Decl $inv15f 'a.sys').SourceRelPath -eq 'amd64dir')) "err=$perr15f rel=$((Get-Decl $inv15f 'a.sys').SourceRelPath)"
$perr15g = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames.x86]','2 = "d",,,"\x86only"','[SourceDisksFiles]','d.sys = 2')), [ref]$perr15g) | Out-Null
Check '6f C1.15h disk id defined ONLY in .x86 -> not defined for this target' ($perr15g -match 'not defined in \[SourceDisksNames\]') "err=$perr15g"
# (g) path validation still applies inside the architecture-specific section
$perr15h = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[SourceDisksNames]','1 = "d",,,','[SourceDisksNames.amd64]','1 = "d",,,"C:\abs"','[SourceDisksFiles]','a.sys = 1')), [ref]$perr15h) | Out-Null
Check '6f C1.15i drive-absolute path INSIDE .amd64 section still refused' ($perr15h -match 'drive-absolute source path rejected') "err=$perr15h"
# -- C1.16 CopyFiles strict syntax (docs: @filename | file-list-section...) --
$perr16a = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=@direct.dll,FL','[FL]','other.sys')), [ref]$perr16a) | Out-Null
Check '6f C1.16a MIXED @filename+section list -> refused (invalid syntax)' ($perr16a -match 'mixes @filename') "err=$perr16a"
$perr16b = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=@a.dll,@b.dll')), [ref]$perr16b) | Out-Null
Check '6f C1.16b multiple @filename entries -> refused (single @ is the only form)' ($perr16b -match 'lists multiple @filename') "err=$perr16b"
$perr16c = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=FL,','[FL]','x.sys')), [ref]$perr16c) | Out-Null
Check '6f C1.16c EMPTY item in CopyFiles list -> refused' ($perr16c -match 'empty item') "err=$perr16c"
$perr16d = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=FL.ntamd64','[FL]','x.sys')), [ref]$perr16d) | Out-Null
Check '6f C1.16d platform-decorated file-list section ref -> refused' ($perr16d -match 'does not support decorating a file-list-section name') "err=$perr16d"
$perr16e = 'sentinel'
[PnpSetup]::ParseInfInventory((New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=@sub\a.dll')), [ref]$perr16e) | Out-Null
Check '6f C1.16e path inside @filename -> refused as invalid file name' ($perr16e -match 'not a plain file name') "err=$perr16e"
$inf16f = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles =','[SourceDisksNames]','1 = "d",,,','[SourceDisksFiles]','a.sys = 1')
$perr16f = 'sentinel'; $inv16f = [PnpSetup]::ParseInfInventory($inf16f, [ref]$perr16f)
Check '6f C1.16f EMPTY CopyFiles value -> ignored, other declarations unaffected' (($null -eq $perr16f) -and (Test-LeafSet (Get-InvLeaves $inv16f) @('k.cat','a.sys'))) "err=$perr16f leaves=$(Get-InvLeaves $inv16f)"
$inf16g = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','CatalogFile=k.cat','[DDInstall]','CopyFiles=FL,FL2','[FL]','x.sys','[FL2]','x.sys')
$perr16g = 'sentinel'; $inv16g = [PnpSetup]::ParseInfInventory($inf16g, [ref]$perr16g)
$dup16 = @($inv16g | Where-Object { $_.LeafName -eq 'x.sys' }).Count
Check '6f C1.16g same source file via two sections -> ONE deterministic entry' (($null -eq $perr16g) -and ($dup16 -eq 1)) "err=$perr16g count=$dup16"
# -- C1: missing/unreadable package files are covered by R8/R8b on the
#        in-memory FS in section [6c] (declared file absent / hash read fails
#        => identity refused with the specific reason).

function New-Ident($Err, [string]$InfHash, [string[]]$Names, [string[]]$Hashes) {
    $id = New-Object PnpSetup+PackageIdentity
    # NB: PS coerces a $null assignment on a string field to ''; only assign
    # an error when one is actually wanted so the field stays truly null.
    if ($null -ne $Err) { $id.Error = $Err }
    $id.InfHash = $InfHash
    $id.KeyFileNames  = New-Object 'System.Collections.Generic.List[string]'
    $id.KeyFileHashes = New-Object 'System.Collections.Generic.List[string]'
    if ($Names)  { foreach ($n in $Names)  { [void]$id.KeyFileNames.Add($n) } }
    if ($Hashes) { foreach ($h in $Hashes) { [void]$id.KeyFileHashes.Add($h) } }
    return $id
}
$okT = New-Ident $null 'aaa' @('sys.bin','k.cat') @('h1','h2')
$why = 'x'
$m = [PnpSetup]::IdentityMatches($okT, (New-Ident $null 'aaa' @('sys.bin','k.cat') @('h1','h2')), [ref]$why)
Check 'identity: identical hash sets match' $m "why=$why"
$m = [PnpSetup]::IdentityMatches($okT, (New-Ident $null 'bbb' @('sys.bin','k.cat') @('h1','h2')), [ref]$why)
Check 'identity: different INF hash rejected' ((-not $m) -and ($why -match 'INF content hash differs')) "why=$why"
$m = [PnpSetup]::IdentityMatches($okT, (New-Ident $null 'aaa' @('sys.bin') @('h1')), [ref]$why)
Check 'identity: missing declared file rejected' ((-not $m) -and ($why -match 'missing declared file')) "why=$why"
$m = [PnpSetup]::IdentityMatches($okT, (New-Ident $null 'aaa' @('sys.bin','k.cat') @('h1','HXX')), [ref]$why)
Check 'identity: different declared file hash rejected' ((-not $m) -and ($why -match 'declared file: k\.cat')) "why=$why"
$m = [PnpSetup]::IdentityMatches($okT, (New-Ident 'boom' 'aaa' @() @()), [ref]$why)
Check 'identity: incomplete candidate identity rejected' ((-not $m) -and ($why -match 'candidate identity unavailable')) "why=$why"

# --- 6g. REAL INF models/section audit + models parser boundary (pure) -----
# Task B first step: prove the project's own INF maps the target hardware ID
# through exactly ONE models entry (no INF-side multi-mapping), and prove the
# audit fails closed outside its documented subset.
$merr = 'sentinel'
$models = [PnpSetup]::ParseInfModelsForHwid($infTextReal, $hw, [ref]$merr)
Check '6g real INF: exactly ONE models entry targets Root\TextDeckHidDriver' (($null -eq $merr) -and ($models.Count -eq 1)) "err=$merr count=$($models.Count)"
Check '6g real INF: entry maps through the declared NTamd64 models section' (($models.Count -eq 1) -and ($models[0].ModelsSection -like '*.NTamd64') -and ($models[0].SectionBase -eq 'TextDeck_Device') -and ($models[0].HardwareId -eq 'Root\TextDeckHidDriver')) "sec=$($models[0].ModelsSection) base=$($models[0].SectionBase)"
$merr2 = 'sentinel'
$m2 = [PnpSetup]::ParseInfModelsForHwid($infTextReal, 'Root\OtherDevice', [ref]$merr2)
Check '6g real INF: foreign hwid -> ZERO target entries, no parse error (target section absent)' (($null -eq $merr2) -and ($m2.Count -eq 0)) "err=$merr2 count=$($m2.Count)"
# two model entries, SAME hardware ID, DIFFERENT install sections -> BOTH are
# reported (the audit surfaces INF-side ambiguity; the selection side refuses)
$infAmb = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','[Manufacturer]','%M%=Std,NTamd64','[Std.NTamd64]','%Desc1%=SecOne, Root\VHID','%Desc2%=SecTwo, Root\VHID','[Strings]','M = X','Desc1 = D1','Desc2 = D2')
$merr3 = 'sentinel'
$m3 = [PnpSetup]::ParseInfModelsForHwid($infAmb, 'Root\VHID', [ref]$merr3)
Check '6g same hwid via TWO install sections -> both entries reported (count=2)' (($null -eq $merr3) -and ($m3.Count -eq 2)) "err=$merr3 count=$($m3.Count)"
Check '6g differing install-section bases identified (SecOne vs SecTwo)' (($m3.Count -eq 2) -and ($m3[0].SectionBase -eq 'SecOne') -and ($m3[1].SectionBase -eq 'SecTwo')) "bases=$($m3[0].SectionBase),$($m3[1].SectionBase)"
# unsupported Manufacturer decoration -> fail closed, never guessed
$infDec = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','[Manufacturer]','%M%=Std,NTx86','[Std.NTx86]','%Desc%=S, Root\VHID','[Strings]','M = X','Desc = D')
$merr4 = 'sentinel'
[PnpSetup]::ParseInfModelsForHwid($infDec, 'Root\VHID', [ref]$merr4) | Out-Null
Check '6g unsupported Manufacturer decoration (NTx86) -> parseError with reason' ($merr4 -match "unsupported decoration 'NTx86'") "err=$merr4"
# [Manufacturer] pointing at a section that does not exist -> fail closed
$infMiss = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','[Manufacturer]','%M%=Std,NTamd64','[Strings]','M = X')
$merr5 = 'sentinel'
[PnpSetup]::ParseInfModelsForHwid($infMiss, 'Root\VHID', [ref]$merr5) | Out-Null
Check '6g missing models section referenced -> parseError with reason' ($merr5 -match 'missing models section') "err=$merr5"
# models entry without a hardware ID -> fail closed (incomplete record)
$infNoId = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','[Manufacturer]','%M%=Std,NTamd64','[Std.NTamd64]','%Desc%=SecOnly','[Strings]','M = X','Desc = D')
$merr6 = 'sentinel'
[PnpSetup]::ParseInfModelsForHwid($infNoId, 'Root\VHID', [ref]$merr6) | Out-Null
Check '6g models entry without hardware ID -> parseError (fail closed)' ($merr6 -match 'has no hardware ID') "err=$merr6"
# external INF dependency blocks the audit entirely
$infExt = New-Inf @('[Version]','DriverVer = 01/01/2020,1.0.0.0','[Manufacturer]','%M%=Std,NTamd64','[Std.NTamd64]','%Desc%=Sec, Root\VHID','Needs = base.inf','[Strings]','M = X','Desc = D')
$merr7 = 'sentinel'
[PnpSetup]::ParseInfModelsForHwid($infExt, 'Root\VHID', [ref]$merr7) | Out-Null
Check '6g models audit refuses INFs with Needs= dependency' ($merr7 -match 'cannot be inventoried') "err=$merr7"

Write-Host '  NOTE: [6] verifies declarations/layouts/pure logic via compile+reflection and seam-'
Write-Host '        injected simulation over an in-memory FS; it does NOT call the real SetupGetInf*'
Write-Host '        APIs on a real Driver Store, does NOT run native device installation, and proves'
Write-Host '        no real HID functionality.'

# ---------------------------------------------------------------------------
Write-Host ''
if ($script:Failures.Count -eq 0) {
    Write-Host "RESULT: ALL CHECKS PASSED"
    exit 0
} else {
    Write-Host "RESULT: $($script:Failures.Count) FAILURE(S):"
    $script:Failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
