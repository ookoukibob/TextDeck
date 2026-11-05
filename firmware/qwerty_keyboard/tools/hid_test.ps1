# HID 键盘自动化测试脚本
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File .\hid_test.ps1 -Action read
#   powershell -ExecutionPolicy Bypass -File .\hid_test.ps1 -Action clear
#   powershell -ExecutionPolicy Bypass -File .\hid_test.ps1 -Action wait -Expect "abc" -Timeout 15
#   powershell -ExecutionPolicy Bypass -File .\hid_test.ps1 -Action check -Expect "abc"
#
# 依赖: 记事本 (notepad.exe) + UIAutomation (Windows 自带)

param(
    [string]$Action = "read",
    [string]$Expect = "",
    [int]$Timeout = 15
)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

function Get-NotepadElement {
    $procs = Get-Process notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    if (-not $procs) { return $null }
    $p = $procs | Select-Object -First 1
    try {
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
        if (-not $root) { return $null }
        $cond = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Document)
        $doc = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
        return $doc
    } catch {
        return $null
    }
}

function Read-NotepadText {
    $doc = Get-NotepadElement
    if (-not $doc) { return $null }
    try {
        $vp = $doc.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        return $vp.Current.Value
    } catch {
        return $null
    }
}

function Ensure-Notepad {
    $p = Get-Process notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    if (-not $p) {
        Start-Process notepad.exe
        Start-Sleep -Seconds 2
        $p = Get-Process notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
    }
    if ($p) {
        $sig = @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@
        $u = Add-Type -MemberDefinition $sig -Name "Win32HidTest" -Namespace "HidTestNs" -PassThru
        $u::ShowWindow($p.MainWindowHandle, 9) | Out-Null
        $u::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    }
    return $p
}

function Clear-Notepad {
    $p = Ensure-Notepad
    if (-not $p) { Write-Host "记事本不可用" -ForegroundColor Red; return $false }

    $doc = Get-NotepadElement
    if (-not $doc) { Write-Host "无法获取文档元素" -ForegroundColor Red; return $false }

    try {
        $vp = $doc.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        $vp.SetValue("")
        Start-Sleep -Milliseconds 300
        Write-Host "记事本已清空" -ForegroundColor Green
        return $true
    } catch {
        Write-Host "清空失败: $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
}

switch ($Action.ToLower()) {

    "read" {
        $t = Read-NotepadText
        if ($null -eq $t) {
            Write-Host "无法读取记事本（未打开？）" -ForegroundColor Yellow
            exit 1
        }
        Write-Host "=== 记事本内容 ($($t.Length) 字符) ===" -ForegroundColor Cyan
        Write-Host $t
        exit 0
    }

    "clear" {
        if (Clear-Notepad) { exit 0 } else { exit 1 }
    }

    "wait" {
        if (-not $Expect) {
            Write-Host "-Expect 不能为空" -ForegroundColor Red
            exit 1
        }
        Ensure-Notepad | Out-Null
        Write-Host "等待内容包含 '$Expect' (最多 $Timeout 秒)..." -ForegroundColor Cyan
        $deadline = (Get-Date).AddSeconds($Timeout)
        while ((Get-Date) -lt $deadline) {
            $t = Read-NotepadText
            if ($t -and $t.Contains($Expect)) {
                Write-Host "*** 匹配成功 ***" -ForegroundColor Green
                Write-Host "当前内容: '$t'"
                exit 0
            }
            Start-Sleep -Milliseconds 250
        }
        $t = Read-NotepadText
        Write-Host "*** 超时，未匹配 ***" -ForegroundColor Yellow
        Write-Host "当前内容: '$t'"
        exit 2
    }

    "check" {
        $t = Read-NotepadText
        if ($null -eq $t) {
            Write-Host "无法读取记事本" -ForegroundColor Red
            exit 1
        }
        if ($t.Contains($Expect)) {
            Write-Host "PASS: 内容包含 '$Expect'" -ForegroundColor Green
            exit 0
        } else {
            Write-Host "FAIL: 内容为 '$t'，不包含 '$Expect'" -ForegroundColor Red
            exit 1
        }
    }

    default {
        Write-Host "未知动作: $Action" -ForegroundColor Red
        Write-Host "可用: read | clear | wait | check"
        exit 1
    }
}
