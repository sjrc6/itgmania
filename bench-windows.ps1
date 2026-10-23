# Benchmark-only: memory card tests on Windows. Not for upstream.
#
# Runs, against the profile and songs in <Card>\ITGmania:
#   1. <Reps> profile loads, each followed by a profile save, with per-song,
#      per-event and (on the PR branch) per-operation timings.
#   2. The data-checking stress test (several threads reading, seeking, copying
#      and writing files on the card, comparing every result with plain reads).
#
# Usage, from PowerShell in this folder:
#   .\bench-windows.ps1 -GameDir <folder with Program\ITGmania.exe> -Name branch
#   .\bench-windows.ps1 -GameDir <beta build folder> -Name beta
#
# The game window opens for each run. Results go to <Results>\<Name>.
# Unplug and replug the card before a run to start with Windows' cache empty.
param(
  [Parameter(Mandatory = $true)] [string] $GameDir,
  [Parameter(Mandatory = $true)] [string] $Name,
  [string] $Card = "F:",
  [int] $Reps = 5,
  [int] $StressSeconds = 60,
  [string] $Results = "C:\itgmania-bench"
)
$ErrorActionPreference = "Stop"

$exe = Join-Path $GameDir "Program\ITGmania.exe"
if (!(Test-Path $exe)) { throw "Not found: $exe" }
if (!(Test-Path "$Card\ITGmania\Stats.xml")) { throw "No profile in $Card\ITGmania" }
$out = Join-Path $Results $Name
New-Item -ItemType Directory -Force $out | Out-Null

# Portable mode keeps Save and Logs inside GameDir. Back up existing prefs once.
$portable = Join-Path $GameDir "Portable.ini"
if (!(Test-Path $portable)) { New-Item -ItemType File $portable | Out-Null }
$save = Join-Path $GameDir "Save"
New-Item -ItemType Directory -Force $save | Out-Null
$prefs = Join-Path $save "Preferences.ini"
if ((Test-Path $prefs) -and !(Test-Path "$prefs.bench-backup")) {
  Copy-Item $prefs "$prefs.bench-backup"
}
@"
[Options]
CustomSongsEnable=1
CustomSongsLoadTimeout=100000
CustomSongsMaxCount=100000
CustomSongsMaxMegabytes=100000
CustomSongsMaxSeconds=100000
EventMode=1
LogToDisk=1
MemoryCardDriver=Directory
MemoryCardOsMountPointP1=$Card/
MemoryCardOsMountPointP2=
MemoryCardProfileSubdir=ITGmania
MemoryCardProfiles=1
MemoryCards=1
ShowLogOutput=0
SoundDrivers=Null
Vsync=0
Windowed=1
DisplayWidth=640
DisplayHeight=480
"@ | Set-Content -Encoding ascii $prefs

function Run-Game([string] $Label, [hashtable] $Vars) {
  foreach ($k in $Vars.Keys) { Set-Item "Env:$k" $Vars[$k] }
  try {
    Write-Host "$Name ${Label}: running"
    $p = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru
    if (!$p.WaitForExit(30 * 60 * 1000)) {
      $p.Kill()
      Write-Warning "$Label didn't finish in 30 minutes"
    }
  } finally {
    foreach ($k in $Vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
  }
  $log = Join-Path $GameDir "Logs\log.txt"
  if (Test-Path $log) { Copy-Item $log (Join-Path $out "$Label-log.txt") }
}

for ($i = 1; $i -le $Reps; $i++) {
  Run-Game "load$i" @{
    ITGMANIA_BENCH_AUTORUN = "1"
    ITGMANIA_BENCH_SAVE    = "1"
    ITGMANIA_BENCH_TAG     = "$Name|$i"
    ITGMANIA_BENCH_CSV     = (Join-Path $out "songs.csv")
    ITGMANIA_BENCH_EVENTS  = (Join-Path $out "events.csv")
    ITGMANIA_BENCH_OPLOG   = (Join-Path $out "ops.csv")
  }
}

# Last, so it runs after the loads have touched the files. It's one long card
# access, so lift the access time limit (operations keep their own timeouts).
Run-Game "stress" @{
  ITGMANIA_BENCH_AUTORUN        = "1"
  ITGMANIA_BENCH_STRESS         = "$StressSeconds"
  ITGMANIA_BENCH_STRESS_OUT     = (Join-Path $out "stress.txt")
  ITGMANIA_BENCH_MOUNT_TIMEOUT  = "3600"
  ITGMANIA_BENCH_OPLOG          = (Join-Path $out "stress-ops.csv")
}

Write-Host ""
if (Test-Path (Join-Path $out "stress.txt")) {
  Get-Content (Join-Path $out "stress.txt") | Select-Object -First 1
}
$events = Join-Path $out "events.csv"
if (Test-Path $events) {
  Import-Csv $events |
    Where-Object { $_.event -in "load_profiles_total", "save_profile" } |
    Format-Table tag, event, wall, count -AutoSize
}
Write-Host "Results: $out"
