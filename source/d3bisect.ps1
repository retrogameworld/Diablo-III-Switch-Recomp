# Runs one hidden copy of the game per fragment-shader hash, each skipping that shader's draws, and saves a screenshot
# at frame 2400+i (build\frames\frame_<n>.png). Used to find which shader draws a broken part of the picture.
# Usage: & d3bisect.ps1 -Hashes h1,h2,... [-Seconds 45]
param([string[]]$Hashes, [int]$Seconds = 45)
$root = '<project folder>\src\d3recomp'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$env:LARECOMP_NO_LAUNCHER = '1'; $env:LARECOMP_HIDDEN = '1'; $env:LARECOMP_NO_AUDIO = '1'; $env:LARECOMP_NO_MOVIE_AUDIO = '1'
$env:LARECOMP_SAVE_DIR = '<project folder>\work\testsave'
$procs = @()
for ($i = 0; $i -lt $Hashes.Count; $i++) {
    $env:LARECOMP_SKIP_FSHASH = $Hashes[$i]
    $env:LARECOMP_SHOT_AT = "$(2400 + $i)"
    $log = "<project folder>\work\runs\bisect_$i.log"
    $procs += Start-Process -FilePath "$root\build\lasrecomp.exe" -ArgumentList '--play' -WorkingDirectory $root `
        -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Hidden
}
Start-Sleep -Seconds $Seconds
foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
for ($i = 0; $i -lt $Hashes.Count; $i++) { "$i $($Hashes[$i]) -> frame_$(2400 + $i).png $(Test-Path "$root\build\frames\frame_$(2400 + $i).png")" }
