# Hidden, muted test run of the window build: no console window (the game shares this
# script's hidden console), the game window opens off-screen without focus or a taskbar
# button, sound goes to SDL's dummy driver. Captures PNGs at the given seconds.
#   powershell -File tools\run_test.ps1 -Name c1 -Seconds 60 -Script "600:A,780:A" -Shots 20,30
param([string]$Name = "run", [int]$Seconds = 40, [string]$Script = "", [int[]]$Shots = @(), [switch]$TraceDma)
$root = Split-Path -Parent $PSScriptRoot
$d = "$root\host\build-win"; $shotDir = "$root\test-shots"
New-Item -ItemType Directory -Force $shotDir | Out-Null
$env:WW_TEST_OFFSCREEN = "1"; $env:SDL_AUDIODRIVER = "dummy"
if ($Script) { $env:WW_INPUT_SCRIPT = $Script }
if ($TraceDma) { $env:WW_TRACE_DMA = "1" }
$p = Start-Process -FilePath "$d\WildWatersPC.exe" -ArgumentList "--seconds", "$Seconds" -WorkingDirectory $d `
    -NoNewWindow -RedirectStandardOutput "$d\out_$Name.txt" -RedirectStandardError "$d\err_$Name.txt" -PassThru
$prev = 0
foreach ($t in $Shots) {
    Start-Sleep -Seconds ($t - $prev); $prev = $t
    if ($p.HasExited) { break }
    & "$PSScriptRoot\capture.ps1" -ProcessId $p.Id -Out "$shotDir\${Name}_$t.png" | Out-Null
}
if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { $p.Kill(); Write-Output "killed" }
Remove-Item Env:WW_TEST_OFFSCREEN, Env:SDL_AUDIODRIVER, Env:WW_INPUT_SCRIPT, Env:WW_TRACE_DMA -ErrorAction SilentlyContinue
Write-Output "--- stdout tail"; Get-Content "$d\out_$Name.txt" -Tail 4
Write-Output "--- stderr head"; Get-Content "$d\err_$Name.txt" -TotalCount 14
