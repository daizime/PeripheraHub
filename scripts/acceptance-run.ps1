# 逐步执行 ACCEPTANCE_CHECKLIST 中需要人在场的四项，并把原始输出记进同一个记录文件。
# 默认只打印计划；加 -Run 才会真正执行（其中 A1/B1 会写入设备）。
# 本文件必须保存为 UTF-8 **带 BOM**：无 BOM 时 Windows PowerShell 按 ANSI 解码，
# 中文会拆坏字符串字面量，脚本直接语法错误。
param(
    [switch]$Run,
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root "work\native-write"
$probe = Join-Path $tools "vgn_dpi_write_probe.exe"
$n9probe = Join-Path $tools "n9_peq_write_probe.exe"
if (-not $OutputPath) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir = Join-Path $root "work\verification\acceptance"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $OutputPath = Join-Path $dir "$stamp.md"
}

$record = New-Object System.Collections.Generic.List[string]
function Note([string]$text) {
    Write-Host $text
    $record.Add($text)
}
function Step([string]$title, [string]$why) {
    Write-Host ""
    Write-Host "=== $title ===" -ForegroundColor Cyan
    Write-Host $why -ForegroundColor DarkGray
    $record.Add("")
    $record.Add("## $title")
    $record.Add($why)
}
function Confirm-Step {
    if (-not $Run) { return $false }
    $answer = Read-Host "回车执行，输入 q 跳过本项"
    return ($answer -ne "q")
}
function Run-Tool([string]$label, [string[]]$arguments) {
    $line = ".\work\native-write\$label " + ($arguments -join " ")
    Note "命令：$line"
    $text = & (Join-Path $tools $label) @arguments 2>&1 | Out-String
    foreach ($item in ($text -split "`r?`n")) { if ($item.Trim()) { Note "  $item" } }
    if ($text -match "READ_FAILED|NO_DEVICE") {
        Note "结果：设备没有响应。请持续移动鼠标保持清醒后重跑本项。"
        return $false
    }
    return $true
}
function Save { $record -join "`r`n" | Set-Content -Path $OutputPath -Encoding UTF8 }

Note "# 实机验收记录"
Note "时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')（Asia/Shanghai）"
Note "模式：$(if ($Run) { '执行' } else { '仅打印计划' })"
Note "探针目录：work/native-write"

# ---------- A1 物理效果 ----------
Step "A1 物理效果（VGN 第 4 档临时改 2800，然后改回本次读到的原值）" "需要你看着指针速度是否变化。"
$originalDpi = 0
if (Confirm-Step) {
    # 不能假设原值是 3200：先只读一次拿到真实值，测试后按这个值恢复。
    $readText = & $probe --stage 3 --x 400 --y 400 2>&1 | Out-String
    if ($readText -match "stage\s+3\s+x\s+(\d+)") { $originalDpi = [int]$matches[1] }
    if ($originalDpi -le 0) {
        Note "读不到第 4 档当前值（鼠标可能休眠），未执行任何写入。请持续移动鼠标后重跑。"
    } else {
        Note "第 4 档当前值：$originalDpi。测试结束后会改回这个值，而不是固定的 3200。"
        if (Run-Tool "vgn_dpi_write_probe.exe" @("--stage","3","--x","2800","--y","2800","--apply")) {
            $felt = Read-Host "指针速度是否明显变慢？(y/n)"
            Note "用户观察：$felt"
            Run-Tool "vgn_dpi_write_probe.exe" @("--stage","3","--x","$originalDpi","--y","$originalDpi","--apply") | Out-Null
            Note "已尝试改回 $originalDpi，请看最后一行是否为 VERIFIED；若不是，重跑一次上面的只读命令确认实际值。"
        }
    }
} else { Note "（未执行）" }
Save

# ---------- A2 断电保存 ----------
Step "A2 断电保存" "请关闭鼠标电源，等 10 秒，再打开，然后持续移动鼠标。"
if (Confirm-Step) {
    Read-Host "鼠标已重新打开并保持移动后，按回车继续"
    if (Run-Tool "vgn_dpi_write_probe.exe" @("--stage","3","--x","400","--y","400")) {
        if ($originalDpi -gt 0) { Note "请确认上面的 stage 3 是否仍为 $originalDpi（A1 记录的原值）。" }
        else { Note "请确认上面的 stage 3 与测试前一致。" }
    }
} else { Note "（未执行）" }
Save

# ---------- B1 N9 模式切换 ----------
Step "B1 N9 EQ 模式切换与单点写入" "会短暂切到 Custom 再切回；探针现在会在目标模式下重读，值不一致时拒绝写入。"
if (Confirm-Step) {
    Run-Tool "n9_peq_write_probe.exe" @("--point","0","--hz","64","--gain","-2.0","--q","0.9","--type","1","--set-eq-mode","2","--restore-eq-mode","--apply") | Out-Null
    Note "若出现 REFUSED，说明 Custom 预设该点与请求不同，探针没有写入——这是预期保护，不要为了绕过它加 --allow-change。"
} else { Note "（未执行）" }
Save

# ---------- E1 休眠唤醒 ----------
Step "E1 休眠唤醒" "请让系统睡眠再唤醒，之后检查日志里的两行。"
if (Confirm-Step) {
    Read-Host "唤醒完成并等程序刷新后，按回车继续"
    $logs = Get-ChildItem -Path (Join-Path $root "outputs\PeripheralHub-0.0\data"), (Join-Path $root "work\build-vs\data") -Filter "peripheral-hub.log" -ErrorAction SilentlyContinue
    if (-not $logs) {
        Note "没有找到日志文件：先运行一次程序，或指定 --data-dir。"
    } else {
        foreach ($log in $logs) {
            Note "日志：$($log.FullName)"
            $lines = [System.IO.File]::ReadAllLines($log.FullName, [System.Text.Encoding]::UTF8) |
                Where-Object { $_ -match "System resume|Resume refresh completed" } |
                Select-Object -Last 4
            if (-not $lines) { Note "  没有唤醒记录。" }
            foreach ($item in $lines) { Note "  $item" }
        }
    }
} else { Note "（未执行）" }
Save

Write-Host ""
Write-Host "记录已写入：$OutputPath" -ForegroundColor Green
if (-not $Run) { Write-Host "这是计划模式，没有执行任何设备操作。加 -Run 才会执行。" }
