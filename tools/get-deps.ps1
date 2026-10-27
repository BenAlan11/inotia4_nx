# get-deps.ps1 -- fetches the two prebuilt dependencies, so no Mesa and no
# libnx32 build is needed (neither has a Windows build, and building them
# needs Docker + a container, ~1 hour).
#
#   .\tools\get-deps.ps1
#
# It downloads the GitHub release assets and lays them out as:
#
#   inotia4_nx\third_party\libnx32\   lib\libnx.a, include\switch.h, ...   (mounted over the image's)
#   inotia4_nx\portlibs32\            lib\libEGL.a, include\...            (runtime.mk picks Mesa 26 from it)
#
# Assets come through api.github.com: github.com/releases/download is blocked
# on some networks, the API is not, and it redirects to the same file.
param(
  [string]$RepoLibnx = "aks796/libnx32",
  [string]$RepoMesa  = "aks796/mesa-switch32"
)

$ErrorActionPreference = "Stop"
$Port = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Tmp  = Join-Path $Port "third_party\_dl"
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

function Get-ReleaseAsset($repo, $name, $dest) {
  Write-Host "== $repo : $name"
  $api = "https://api.github.com/repos/$repo/releases"
  $rels = Invoke-RestMethod -Uri $api -Headers @{ "User-Agent" = "inotia4_nx-setup" }
  foreach ($r in $rels) {
    $a = $r.assets | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if ($a) {
      Write-Host ("   {0} ({1:N1} MB) from {2}" -f $a.name, ($a.size / 1MB), $r.tag_name)
      if ((Test-Path $dest) -and ((Get-Item $dest).Length -eq $a.size)) {
        Write-Host "   already downloaded"
        return
      }
      Invoke-WebRequest -Uri $a.url -OutFile $dest `
        -Headers @{ "User-Agent" = "inotia4_nx-setup"; "Accept" = "application/octet-stream" }
      if ((Get-Item $dest).Length -ne $a.size) {
        throw "size mismatch: got $((Get-Item $dest).Length) bytes, expected $($a.size)"
      }
      return
    }
  }
  throw "no asset named '$name' in the releases of $repo"
}

$libnxZip = Join-Path $Tmp "libnx32.zip"
$mesaZip  = Join-Path $Tmp "mesa-switch32.zip"
Get-ReleaseAsset $RepoLibnx "libnx32.zip"      $libnxZip
Get-ReleaseAsset $RepoMesa  "mesa-switch32.zip" $mesaZip

# ---------------------------------------------------------------- extract
function Reset-Dir($p) {
  if (Test-Path $p) { Remove-Item -Recurse -Force $p }
  New-Item -ItemType Directory -Force -Path $p | Out-Null
}

$thirdParty = Join-Path $Port "third_party"
Reset-Dir (Join-Path $thirdParty "libnx32")
Expand-Archive -Path $libnxZip -DestinationPath (Join-Path $thirdParty "libnx32") -Force

# The archives may carry a top-level folder; find where libnx.a / libEGL.a is.
function Find-File($root, $name) {
  Get-ChildItem -Path $root -Recurse -File -Filter $name | Select-Object -First 1
}
$libnx = Find-File (Join-Path $thirdParty "libnx32") "libnx.a"
if (-not $libnx) { throw "libnx.a is not in libnx32.zip" }
$libnxPrefix = Split-Path (Split-Path $libnx.FullName -Parent) -Parent
Write-Host "== libnx32 prefix: $libnxPrefix"

Reset-Dir (Join-Path $Port "portlibs32")
Expand-Archive -Path $mesaZip -DestinationPath (Join-Path $Port "portlibs32") -Force
$egl = Find-File (Join-Path $Port "portlibs32") "libEGL.a"
if (-not $egl) { throw "libEGL.a is not in mesa-switch32.zip" }
$mesaPrefix = Split-Path (Split-Path $egl.FullName -Parent) -Parent
Write-Host "== mesa prefix:    $mesaPrefix"

# Flatten when the archive used a top-level folder, so both are plain prefixes.
foreach ($pair in @(@{ root = (Join-Path $thirdParty "libnx32"); prefix = $libnxPrefix },
                    @{ root = (Join-Path $Port "portlibs32");     prefix = $mesaPrefix })) {
  if ($pair.prefix -ne $pair.root) {
    Write-Host "   flattening $($pair.prefix) -> $($pair.root)"
    Get-ChildItem -Path $pair.prefix -Force | ForEach-Object {
      Move-Item -Force $_.FullName (Join-Path $pair.root $_.Name)
    }
    Get-ChildItem -Path $pair.root -Directory -Force |
      Where-Object { (Get-ChildItem $_.FullName -Force | Measure-Object).Count -eq 0 } |
      Remove-Item -Force
  }
}

# ---------------------------------------------------------------- report
Write-Host ""
foreach ($p in @(
    (Join-Path $thirdParty "libnx32\lib\libnx.a"),
    (Join-Path $thirdParty "libnx32\include\switch.h"),
    (Join-Path $Port "portlibs32\lib\libEGL.a"),
    (Join-Path $Port "portlibs32\lib\libGLESv1_CM.a"),
    (Join-Path $Port "portlibs32\lib\libnouveau_horizon.a"))) {
  $ok = Test-Path $p
  Write-Host ("{0} {1}" -f $(if ($ok) { "ok     " } else { "MISSING" }), $p) `
    -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
}

Write-Host ""
Write-Host "next: .\tools\build-docker.ps1" -ForegroundColor Cyan
