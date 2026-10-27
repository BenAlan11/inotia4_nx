# build-docker.ps1 -- builds the 32-bit wrapper on Windows, without WSL.
#
# runtime/tools/docker_build.sh does this for a POSIX shell; this is the same
# docker run with Windows paths, so PowerShell can drive it directly (the
# container's paths must not go through Git Bash's MSYS translation).
#
#   .\tools\build-docker.ps1                 # -> inotia4_nx.nsp
#   .\tools\build-docker.ps1 -MakeArgs clean
#   .\tools\build-docker.ps1 -Libnx32 D:\nx\libnx32\prefix
#
# Needs Docker Desktop running. The image is ghcr.io/vita2hos/devcontainer/
# vita2hos, the AArch32 toolchain (devkitARM + switch-tools + miniz).
param(
  [string]$Libnx32 = "",
  [string]$Image = "ghcr.io/vita2hos/devcontainer/vita2hos:latest",
  [string[]]$MakeArgs = @()
)

$ErrorActionPreference = "Stop"
$Port = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$NXD = "/opt/devkitpro/libnx32"

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function ToDockerPath($p) { (Resolve-Path $p).Path -replace '\\', '/' }

# ---------------------------------------------------------------- docker
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
  Fail "docker not found. Install Docker Desktop, start it, then reopen this window."
}
docker info *> $null
if ($LASTEXITCODE -ne 0) { Fail "the docker engine is not running. Start Docker Desktop and wait for 'Engine running'." }

# --------------------------------------------------------------- libnx32
# A prefix with lib/libnx.a + include/switch.h. The prebuilt release is what
# tools/get-deps.ps1 puts in third_party/libnx32.
$candidates = @()
if ($Libnx32) { $candidates += $Libnx32 }
$candidates += @(
  (Join-Path $Port "third_party\libnx32"),
  (Join-Path $Port "..\libnx32\prefix"),
  (Join-Path $Port "..\libnx32"),
  (Join-Path $Port "libnx32")
)
$NX = $null
foreach ($c in $candidates) {
  if ($c -and (Test-Path (Join-Path $c "lib\libnx.a")) -and (Test-Path (Join-Path $c "include\switch.h"))) {
    $NX = (Resolve-Path $c).Path; break
  }
}
if (-not $NX) {
  Fail "libnx32 not found. Run .\tools\get-deps.ps1 first (it downloads the prebuilt), or pass -Libnx32 <prefix>."
}

if (-not (Test-Path (Join-Path $Port "portlibs32\lib\libEGL.a"))) {
  Write-Host "WARNING: portlibs32\lib\libEGL.a is missing: the build will use the null renderer." -ForegroundColor Yellow
  Write-Host "         Run .\tools\get-deps.ps1 to put the prebuilt Mesa there." -ForegroundColor Yellow
}

# ------------------------------------------------------------ the command
$portPath = ToDockerPath $Port
$nxPath   = ToDockerPath $NX

$dockerArgs = @(
  "run", "--rm", "--platform", "linux/amd64",
  "-v", "${portPath}:/work",
  "-v", "${nxPath}/include/switch:${NXD}/include/switch:ro",
  "-v", "${nxPath}/include/switch.h:${NXD}/include/switch.h:ro",
  "-v", "${nxPath}/lib/libnx.a:${NXD}/lib/libnx.a:ro",
  "-v", "${nxPath}/lib/libnxd.a:${NXD}/lib/libnxd.a:ro",
  "-w", "/work",
  $Image, "bash", "-lc", 'exec make -j"$(nproc)" "$@"', "make"
) + $MakeArgs

Write-Host "port:    $Port"
Write-Host "libnx32: $NX"
Write-Host "image:   $Image"
Write-Host ""

& docker @dockerArgs
if ($LASTEXITCODE -ne 0) { Fail "the build failed. The compiler output is above." }

if (Test-Path (Join-Path $Port "inotia4_nx.nsp")) {
  Write-Host ""
  Write-Host "built: $Port\inotia4_nx.nsp" -ForegroundColor Green
  Get-ChildItem (Join-Path $Port "inotia4_nx.nsp"), (Join-Path $Port "inotia4_nx.build") |
    Select-Object Name, Length | Format-Table -AutoSize
} else {
  Write-Host "make finished but inotia4_nx.nsp is not there: check the output above." -ForegroundColor Yellow
}
