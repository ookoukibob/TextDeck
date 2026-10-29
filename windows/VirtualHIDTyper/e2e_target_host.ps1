# ---------------------------------------------------------------------------
# e2e_target_host.ps1
#
# Dedicated, self-owned E2E target window for software-input-mode tests.
# Creates a Form with a text control (RichTextBox with AcceptsTab, or a classic
# multiline TextBox), shows it, and writes its current text to a snapshot file
# on a timer so an external orchestrator can read exactly what was typed.
# The orchestrator closes this process; it never touches user windows.
#
#   -Title      unique window title (orchestrator finds the window by it)
#   -Snapshot   file path the current text is written to (UTF-8, no BOM)
#   -Control    RichTextBox (default; accepts Tab) | TextBox (classic Edit)
# Exit code 0.
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Snapshot,
    [ValidateSet('RichTextBox', 'TextBox')][string]$Control = 'RichTextBox'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = $Title
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point(60, 60)
$form.Size = New-Object System.Drawing.Size(760, 520)

if ($Control -eq 'RichTextBox') {
    $box = New-Object System.Windows.Forms.RichTextBox
    $box.AcceptsTab = $true      # Tab inserts a tab character (E2E requirement)
    $box.DetectUrls = $false
} else {
    $box = New-Object System.Windows.Forms.TextBox
    $box.Multiline = $true
    $box.AcceptsReturn = $true
    $box.WordWrap = $false
    $box.ScrollBars = 'Both'
}
$box.Dock = 'Fill'
$box.Font = New-Object System.Drawing.Font('Consolas', 12)
$form.Controls.Add($box)

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({
    try {
        [System.IO.File]::WriteAllText($Snapshot, $box.Text, $utf8NoBom)
    } catch { }   # transient sharing conflict with the reader is fine
})
$timer.Start()

$form.Add_Shown({
    $box.Focus()
    $form.Activate()
})

[void]$form.ShowDialog()

$timer.Stop()
try {
    [System.IO.File]::WriteAllText($Snapshot, $box.Text, $utf8NoBom)
} catch { }
exit 0
