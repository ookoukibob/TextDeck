# ---------------------------------------------------------------------------
# guest_ops.ps1 -- run vmrun guest operations with credentials taken from
#                  Windows Credential Manager (never hard-coded, never logged).
#
# Credential targets (CRED_TYPE_GENERIC, CurrentUser scope):
#   TextDeckGuest:<vmx-path>          UserName = guest user, Blob = guest password
#   VMware Encrypted VM: <vmx-path>   VMware VM encryption password (VMware's own)
#
# The guest password is held in memory only for the duration of the vmrun call.
# It is never written to disk, never echoed, and never assembled through a shell
# string: argument escaping follows the MSVC runtime rules so quotes/backslashes
# in the secret cannot corrupt the command line.
#
# Usage:
#   .\guest_ops.ps1 -Vmx <path> -StoreCredential        # one-time, prompts
#   .\guest_ops.ps1 -Vmx <path> -Test                   # auth + Tools check
#   .\guest_ops.ps1 -Vmx <path> -ListDirectory <guestpath>
#   .\guest_ops.ps1 -Vmx <path> -RunProgram <exe> -ProgramArgs <args>
# ---------------------------------------------------------------------------
[CmdletBinding(DefaultParameterSetName = 'Test')]
param(
    [Parameter(Mandatory = $true)][string]$Vmx,
    [Parameter(ParameterSetName = 'ListDirectory', Mandatory = $true)][string]$ListDirectory,
    [Parameter(ParameterSetName = 'RunProgram', Mandatory = $true)][string]$RunProgram,
    [Parameter(ParameterSetName = 'RunProgram')][string]$ProgramArgs = '',
    [Parameter(ParameterSetName = 'Test')][switch]$Test,
    [Parameter(ParameterSetName = 'Store', Mandatory = $true)][switch]$StoreCredential,
    [string]$CredentialTargetPrefix = 'TextDeckGuest',
    [int]$TimeoutSeconds = 60,
    # Non-interactive store path: password is taken from the named environment
    # variable so it never appears on a command line or in a file. The variable
    # is read and cleared by this process only.
    [string]$GuestUser,
    [string]$GuestPasswordEnvVar = 'TD_GUEST_PASSWORD',
    # Path to vmrun.exe on YOUR machine. Kept as a parameter rather than a
    # hard-coded drive letter so this script carries no local install path into
    # the repository. Override it, or set the VMWARE_VMRUN environment variable.
    [string]$VmrunExe = ''
)

$ErrorActionPreference = 'Stop'
if (-not $VmrunExe -and $env:VMWARE_VMRUN) { $VmrunExe = $env:VMWARE_VMRUN }
if (-not $VmrunExe) {
    # Fall back to the documented default install locations when they exist, so
    # the script still works unconfigured on a standard VMware Workstation setup.
    foreach ($c in @(
            (Join-Path $env:ProgramFiles 'VMware\VMware Workstation\vmrun.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'VMware\VMware Workstation\vmrun.exe'))) {
        if ($c -and (Test-Path -LiteralPath $c)) { $VmrunExe = $c; break }
    }
}
if (-not $VmrunExe) {
    Write-Host '[guest_ops] FAIL: vmrun.exe not found. Pass -VmrunExe <path> or set VMWARE_VMRUN.' -ForegroundColor Red
    exit 2
}

function Write-Info($m) { Write-Host ('[guest_ops] ' + $m) }
function Write-Fail($m) { Write-Host ('[guest_ops] FAIL: ' + $m) -ForegroundColor Red }

if (-not ('CredNative' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public class CredNative {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct CREDENTIAL {
        public UInt32 Flags;
        public UInt32 Type;
        public IntPtr TargetName;
        public IntPtr Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public UInt32 CredentialBlobSize;
        public IntPtr CredentialBlob;
        public UInt32 Persist;
        public UInt32 AttributeCount;
        public IntPtr Attributes;
        public IntPtr TargetAlias;
        public IntPtr UserName;
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CredReadW(string target, uint type, uint flags, out IntPtr credential);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CredWriteW(ref CREDENTIAL credential, uint flags);

    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern void CredFree(IntPtr cred);

    public static bool Read(string target, out string userName, out byte[] blob) {
        userName = null; blob = null;
        IntPtr p;
        if (!CredReadW(target, 1, 0, out p)) return false;
        try {
            CREDENTIAL c = (CREDENTIAL)Marshal.PtrToStructure(p, typeof(CREDENTIAL));
            userName = c.UserName == IntPtr.Zero ? null : Marshal.PtrToStringUni(c.UserName);
            blob = new byte[c.CredentialBlobSize];
            if (c.CredentialBlobSize > 0) Marshal.Copy(c.CredentialBlob, blob, 0, (int)c.CredentialBlobSize);
            return true;
        } finally { CredFree(p); }
    }

    public static bool Write(string target, string userName, byte[] blob) {
        CREDENTIAL c = new CREDENTIAL();
        c.Type = 1;
        c.Persist = 2;
        c.TargetName = Marshal.StringToCoTaskMemUni(target);
        c.UserName = Marshal.StringToCoTaskMemUni(userName);
        c.CredentialBlobSize = (uint)blob.Length;
        c.CredentialBlob = Marshal.AllocCoTaskMem(blob.Length);
        Marshal.Copy(blob, 0, c.CredentialBlob, blob.Length);
        try { return CredWriteW(ref c, 0); }
        finally {
            Marshal.FreeCoTaskMem(c.TargetName);
            Marshal.FreeCoTaskMem(c.UserName);
            Marshal.FreeCoTaskMem(c.CredentialBlob);
        }
    }
}
'@
}

if (-not ('ArgEsc' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System.Text;
public class ArgEsc {
    public static string Escape(string s) {
        if (string.IsNullOrEmpty(s)) return "\"\"";
        if (s.IndexOfAny(new char[] { ' ', '\t', '"', '\\' }) < 0) return s;
        StringBuilder sb = new StringBuilder();
        sb.Append('"');
        int bs = 0;
        foreach (char ch in s) {
            if (ch == '\\') { bs++; }
            else if (ch == '"') { sb.Append('\\', bs * 2 + 1); sb.Append('"'); bs = 0; }
            else { sb.Append('\\', bs); sb.Append(ch); bs = 0; }
        }
        sb.Append('\\', bs * 2);
        sb.Append('"');
        return sb.ToString();
    }
}
'@
}

function Invoke-Vmrun {
    param([string[]]$VmrunArgs, [int]$TimeoutSec = 60)
    $cmd = ($VmrunArgs | ForEach-Object { [ArgEsc]::Escape($_) }) -join ' '
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $VmrunExe
    $psi.Arguments = $cmd
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $so = New-Object System.Text.StringBuilder
    $se = New-Object System.Text.StringBuilder
    $pr = New-Object System.Diagnostics.Process
    $pr.StartInfo = $psi
    $evO = Register-ObjectEvent -InputObject $pr -EventName OutputDataReceived -Action {
        if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
    } -MessageData $so
    $evE = Register-ObjectEvent -InputObject $pr -EventName ErrorDataReceived -Action {
        if ($EventArgs.Data) { [void]$Event.MessageData.AppendLine($EventArgs.Data) }
    } -MessageData $se
    [void]$pr.Start()
    $pr.BeginOutputReadLine()
    $pr.BeginErrorReadLine()
    $exited = $pr.WaitForExit($TimeoutSec * 1000)
    if ($exited) { Start-Sleep -Milliseconds 250 }
    $exitCode = -1
    if ($exited) { $exitCode = $pr.ExitCode } else { try { $pr.Kill() } catch { } }
    Unregister-Event -SourceIdentifier $evO.Name -ErrorAction SilentlyContinue
    Unregister-Event -SourceIdentifier $evE.Name -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        ExitCode = $exitCode
        StdOut = $so.ToString().Trim()
        StdErr = $se.ToString().Trim()
        TimedOut = (-not $exited)
    }
}

if (-not (Test-Path $VmrunExe)) { Write-Fail ('vmrun not found: ' + $VmrunExe); exit 2 }
if (-not (Test-Path $Vmx)) { Write-Fail ('VMX not found: ' + $Vmx); exit 2 }
$Vmx = (Resolve-Path $Vmx).Path
$guestTarget = $CredentialTargetPrefix + ':' + $Vmx
$encTarget = 'VMware Encrypted VM: ' + $Vmx

if ($StoreCredential) {
    # User name: -GuestUser if given, otherwise prompt.
    $user = $GuestUser
    if (-not $user) { $user = Read-Host 'Guest Windows user name' }

    # Password: read from the environment variable when it is set, so the
    # value never enters a command line, a script file or a log.
    $plain = $null
    $fromEnv = $false
    if (Test-Path -LiteralPath ('Env:' + $GuestPasswordEnvVar)) {
        $plain = [Environment]::GetEnvironmentVariable($GuestPasswordEnvVar)
        $fromEnv = $true
        Remove-Item -LiteralPath ('Env:' + $GuestPasswordEnvVar) -ErrorAction SilentlyContinue
    }

    $bstr = [IntPtr]::Zero
    try {
        if (-not $fromEnv) {
            $sec = Read-Host 'Guest Windows password' -AsSecureString
            $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($sec)
            $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
        }
        if ([string]::IsNullOrEmpty($plain)) { Write-Fail 'no password supplied'; exit 1 }
        $bytes = [Text.Encoding]::UTF8.GetBytes($plain)
        $ok = [CredNative]::Write($guestTarget, $user, $bytes)
        [Array]::Clear($bytes, 0, $bytes.Length)
        if ($ok) { Write-Info ('stored credential for user: ' + $user + ' (source: ' + $(if ($fromEnv) { 'environment variable' } else { 'prompt' }) + ')') }
        else { Write-Fail ('CredWrite failed, Win32=' + [Runtime.InteropServices.Marshal]::GetLastWin32Error()); exit 1 }
    } finally {
        if ($bstr -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
        $plain = $null
    }
    exit 0
}

$gu = $null; $gb = $null
if (-not [CredNative]::Read($guestTarget, [ref]$gu, [ref]$gb)) {
    Write-Fail ('guest credential not found: ' + $guestTarget)
    Write-Info ('run with -StoreCredential to save it once')
    exit 3
}

$vp = $null
$vu = $null; $vb = $null
if ([CredNative]::Read($encTarget, [ref]$vu, [ref]$vb)) {
    if ($vb -and $vb.Length -gt 0) { $vp = [Text.Encoding]::UTF8.GetString($vb) }
}

Write-Info ('vmx        = ' + $Vmx)
Write-Info ('guest user = ' + $gu)
if ($vp) { Write-Info 'vm encryption password = present (Credential Manager)' }
else { Write-Info 'vm encryption password = absent' }
Write-Info ''

$gp = [Text.Encoding]::UTF8.GetString($gb)

try {
    Write-Info '=== 1. VMware Tools state ==='
    $base = @('-T', 'workstation')
    if ($vp) { $base += @('-vp', $vp) }

    $tools = Invoke-Vmrun -VmrunArgs ($base + @('checkToolsState', $Vmx)) -TimeoutSec 20
    Write-Info ('  checkToolsState exit=' + $tools.ExitCode + ' -> ' + $tools.StdOut)

    $ip = Invoke-Vmrun -VmrunArgs ($base + @('getGuestIPAddress', $Vmx)) -TimeoutSec 20
    Write-Info ('  getGuestIPAddress exit=' + $ip.ExitCode + ' -> ' + $ip.StdOut)

    if ($tools.StdOut -notmatch 'running') {
        Write-Fail 'VMware Tools is not running in the guest'
        exit 4
    }

    Write-Info ''
    Write-Info '=== 2. guest authentication ==='
    $authArgs = $base + @('-gu', $gu, '-gp', $gp)

    if ($PSCmdlet.ParameterSetName -eq 'ListDirectory') {
        $r = Invoke-Vmrun -VmrunArgs ($authArgs + @('listDirectoryInGuest', $Vmx, $ListDirectory)) -TimeoutSec $TimeoutSeconds
        Write-Info ('  exit=' + $r.ExitCode)
        if ($r.StdOut) { $r.StdOut -split "`n" | Select-Object -First 20 | ForEach-Object { Write-Info ('    ' + $_) } }
        if ($r.StdErr) { Write-Info ('  stderr: ' + $r.StdErr) }
        if ($r.ExitCode -ne 0) { Write-Fail ('listDirectoryInGuest failed'); exit 5 }
        Write-Info '  AUTH + TOOLS + GUEST OPS: PASS'
    }
    elseif ($PSCmdlet.ParameterSetName -eq 'RunProgram') {
        $args2 = $authArgs + @('runProgramInGuest', $Vmx, $RunProgram)
        if ($ProgramArgs) { $args2 += $ProgramArgs }
        $r = Invoke-Vmrun -VmrunArgs $args2 -TimeoutSec $TimeoutSeconds
        Write-Info ('  exit=' + $r.ExitCode)
        if ($r.StdOut) { Write-Info ('  stdout: ' + $r.StdOut) }
        if ($r.StdErr) { Write-Info ('  stderr: ' + $r.StdErr) }
        if ($r.ExitCode -ne 0) { Write-Fail 'runProgramInGuest failed'; exit 5 }
        Write-Info '  PASS'
    }
    else {
        $r = Invoke-Vmrun -VmrunArgs ($authArgs + @('listDirectoryInGuest', $Vmx, 'C:\Windows')) -TimeoutSec $TimeoutSeconds
        Write-Info ('  listDirectoryInGuest(C:\Windows) exit=' + $r.ExitCode)
        if ($r.StdOut) { $r.StdOut -split "`n" | Select-Object -First 10 | ForEach-Object { Write-Info ('    ' + $_) } }
        if ($r.ExitCode -ne 0) { Write-Fail ('auth failed'); exit 5 }
        Write-Info '  AUTH: PASS'
    }
}
finally {
    if ($gb) { [Array]::Clear($gb, 0, $gb.Length) }
    if ($vb) { [Array]::Clear($vb, 0, $vb.Length) }
    $gp = $null; $vp = $null
}
