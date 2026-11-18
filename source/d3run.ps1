# Test run of the Diablo III recomp: starts the game for N seconds (default 40), then stops this test window only.
# Log goes to work\runs\run_<time>.log. Usage: powershell -File d3run.ps1 [seconds] [extra env NAME=VALUE ...]
param([int]$Seconds = 40, [string[]]$EnvVars = @())
$root = '<project folder>\src\d3recomp'
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$env:LARECOMP_NO_LAUNCHER = '1'
$env:LARECOMP_HIDDEN = '1'  # never shown: renders off screen, never takes the tester's focus
$env:LARECOMP_NO_AUDIO = '1'
$env:LARECOMP_NO_MOVIE_AUDIO = '1'
$env:LARECOMP_SETTINGS = '<project folder>\work\test_settings.ini'  # (fixed 4K test settings; the tester's larecomp.ini untouched)
$env:LARECOMP_SAVE_DIR = "<project folder>\work\testsave\run_$(Get-Date -Format yyyyMMdd_HHmmss)"  # fresh saves each run (LARECOMP_SAVE_DIR=... to reuse one)
foreach ($kv in $EnvVars) { $k, $v = $kv.Split('=', 2); Set-Item "env:$k" $v }
New-Item -ItemType Directory -Force <project folder>\work\runs, $env:LARECOMP_SAVE_DIR | Out-Null
$log = "<project folder>\work\runs\run_$(Get-Date -Format yyyyMMdd_HHmmss).log"
$exe = if ($env:D3RUN_EXE) { $env:D3RUN_EXE } else { "$root\build\lasrecomp.exe" }  # (D3RUN_EXE=...: another build)
$p = Start-Process -FilePath $exe -ArgumentList '--play' -WorkingDirectory $root `
    -RedirectStandardError $log -RedirectStandardOutput "$log.out" -PassThru -WindowStyle Hidden
$t0 = $p.StartTime.AddSeconds(-2)
$exited = $p.WaitForExit($Seconds * 1000)
if (-not $exited) { Stop-Process -Id $p.Id -Force; "stopped after $Seconds s" } else { "exited by itself, code $($p.ExitCode)" }
# (two hidden test copies once kept running for an hour after this stop: any window-less copy started by this run goes
# too - the tester's own game always has a window and started earlier)
Start-Sleep -Milliseconds 500
Get-Process lasrecomp -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $t0 -and $_.MainWindowHandle -eq 0 } |
    ForEach-Object { Stop-Process -Id $_.Id -Force; "also stopped leftover test copy $($_.Id)" }
"log: $log"
