# Renders every restored song to a WAV with the headless build (no window, no sound):
# a Devil's Elbow race with the song forced, recorded from the game's audio output.
#   powershell -File tools\render_songs.ps1 [-Seconds 75] [-Out ..\extracted\music_wav]
param([int]$Seconds = 75, [string]$Out = "")
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $root) "extracted\music_wav" }
New-Item -ItemType Directory -Force $Out | Out-Null
$d = "$root\host\build-headless"
$names = @("song1","song2","song3","song4","short_song","old01","old02","old03","old04","old05","old06","old07","old08","old09","old10","old11","old12","old13")
for ($i = 1; $i -le $names.Count; $i++) {
    $env:WW_MUSIC_SONG = "$i"; $env:WW_AUDIO_DUMP = "$Out\raw_$($names[$i-1]).wav"
    $env:WW_INPUT_SCRIPT = "600:A,780:A,960:A,1140:A,1320:A"
    $p = Start-Process -FilePath "$d\WildWatersPC.exe" -ArgumentList "--headless","--seconds","$Seconds" -WorkingDirectory $d `
        -NoNewWindow -RedirectStandardOutput "$d\out_song$i.txt" -RedirectStandardError "$d\err_song$i.txt" -PassThru
    if (-not $p.WaitForExit(($Seconds + 60) * 1000)) { $p.Kill() }
    $err = Get-Content "$d\err_song$i.txt" -TotalCount 1
    Write-Output ("{0} {1} {2}" -f $i, $names[$i-1], $(if ($err) { "CRASH: $err" } else { "ok" }))
}
Remove-Item Env:WW_MUSIC_SONG, Env:WW_AUDIO_DUMP, Env:WW_INPUT_SCRIPT -ErrorAction SilentlyContinue
