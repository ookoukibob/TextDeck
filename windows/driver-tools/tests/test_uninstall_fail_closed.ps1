# Static regression checks for fail-closed behaviour in uninstall_driver.ps1.
# Safe by construction: this test reads source text only and never invokes PnPUtil,
# changes device state, imports certificates, or touches boot/security settings.
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$sourcePath = Join-Path $repoRoot 'windows\driver-tools\uninstall_driver.ps1'
$source = Get-Content -LiteralPath $sourcePath -Raw

$checks = New-Object System.Collections.Generic.List[object]
function Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $checks.Add([pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail })
}

Check 'Registry enumeration is not silently treated as empty' (
    $source -notmatch 'Get-ChildItem \$enumRoot -ErrorAction SilentlyContinue' -and
    $source -notmatch 'Get-ItemProperty \$inst\.PSPath -ErrorAction SilentlyContinue'
) 'Potential TextDeck device enumeration must fail closed.'

Check 'Registry enumeration uses terminating errors' (
    $source -match 'Get-ChildItem \$enumRoot -ErrorAction Stop' -and
    $source -match 'Get-ItemProperty -LiteralPath \$inst\.PSPath -ErrorAction Stop'
) 'Errors while enumerating possible TextDeck instances must abort the operation.'

$preflightAt = $source.IndexOf('$null = (& pnputil /enum-drivers')
$preflightExitAt = -1
$removeAt = -1
if ($preflightAt -ge 0) {
    $preflightExitAt = $source.IndexOf('$preflightExit = $LASTEXITCODE', $preflightAt)
    if ($preflightExitAt -ge 0) {
        $removeAt = $source.IndexOf('& pnputil /remove-device', $preflightExitAt)
    }
}
Check 'Package inventory is checked before device mutation' (
    $preflightAt -ge 0 -and $preflightExitAt -gt $preflightAt -and $removeAt -gt $preflightExitAt
) 'A failed inventory command must stop before the first remove-device call.'

Check 'Package enumeration exit codes are checked' (
    ([regex]::Matches($source, '\$enumExit = \$LASTEXITCODE')).Count -ge 2
) 'Both package enumeration passes must inspect the native exit code.'

Check 'A residual package makes uninstall fail' (
    $source -match '(?s)VERIFY FAILED: a textdeckhiddriver\.inf package is still in the Driver Store.*?\$failures\+\+'
) 'A remaining package must not result in exit code 0.'

$failed = @($checks | Where-Object { -not $_.Ok })
foreach ($c in $checks) {
    Write-Host ("  [{0}] {1} :: {2}" -f $(if ($c.Ok) { 'PASS' } else { 'FAIL' }), $c.Name, $c.Detail)
}
Write-Host ("{0} checks, {1} failure(s)" -f $checks.Count, $failed.Count)
if ($failed.Count) { exit 1 }
exit 0
