# read_notepad.ps1 - Read Notepad window text via UI Automation (read-only)
# ASCII-only on purpose: PS 5.1 misreads UTF-8 without BOM.
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$procs = Get-Process -Name notepad -ErrorAction SilentlyContinue
if (-not $procs) {
    Write-Output "NO NOTEPAD PROCESS FOUND"
    exit 0
}

foreach ($p in $procs) {
    if ($p.MainWindowHandle -eq 0) { continue }
    $w = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
    Write-Output ("WINDOW: class=" + $w.Current.ClassName + " name=" + $w.Current.Name)

    $desc = $w.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)

    foreach ($e in $desc) {
        $ct = $e.Current.ControlType.ProgrammaticName
        if ($ct -match 'Document|Edit') {
            $val = $null
            try {
                $pat = $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
                if ($pat) { $val = $pat.Current.Value }
            } catch {}
            if ($null -eq $val) {
                try {
                    $pat = $e.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
                    if ($pat) { $val = $pat.DocumentRange.GetText(-1) }
                } catch {}
            }
            Write-Output ("  [" + $ct + "] name=" + $e.Current.Name +
                          " class=" + $e.Current.ClassName +
                          " automationId=" + $e.Current.AutomationId)
            if ($null -ne $val) {
                $shown = $val -replace "`r", "<CR>" -replace "`n", "<LF>"
                Write-Output ("  CONTENT>>>" + $shown)
            } else {
                Write-Output "  (no Value/Text pattern available)"
            }
        }
    }
}
