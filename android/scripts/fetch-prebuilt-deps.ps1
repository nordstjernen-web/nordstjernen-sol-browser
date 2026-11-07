# Nordstjernen - fetch Android prebuilt dependency sysroots.
#
# Verifies each asset against SHA256SUMS and, with an authenticated gh CLI
# (GH_TOKEN on CI), against its GitHub build-provenance attestation. A missing
# gh, missing auth or a missing attestation (Sigstore outage at publish time)
# only warns unless NORDSTJERNEN_REQUIRE_ATTESTATION=1; an invalid one is fatal.
# Pass -Tag sysroot-<date>-<sha> (an immutable per-commit release) to pin.

param(
    [string]$Sysroot = "$env:USERPROFILE\.cache\nordstjernen-android-sysroot",
    [string]$Repo = "nordstjernen-web/nordstjernen-dependencies-build",
    [string]$Tag = "sysroot-latest",
    [string[]]$Abi = @("arm64-v8a", "x86_64"),
    [string]$Token = ""
)

$ErrorActionPreference = "Stop"

function Write-Step($Message) {
    Write-Host "[deps] $Message"
}

function Test-BuildProvenance($File, $Digest) {
    $name = [System.IO.Path]::GetFileName($File)
    $required = $env:NORDSTJERNEN_REQUIRE_ATTESTATION -and $env:NORDSTJERNEN_REQUIRE_ATTESTATION -ne "0"
    $softFail = {
        param($Reason)
        if ($required) { throw "$Reason (NORDSTJERNEN_REQUIRE_ATTESTATION is set)" }
        Write-Step "$Reason; skipping provenance verification of $name"
    }
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if (-not $gh) { & $softFail "gh CLI not found"; return }
    & $gh.Source auth status 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { & $softFail "gh CLI is not authenticated (set GH_TOKEN)"; return }
    $count = & $gh.Source api "repos/$Repo/attestations/sha256:$Digest" --jq ".attestations | length" 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $count) { & $softFail "could not query the attestations API for $name"; return }
    if ([int]$count -eq 0) { & $softFail "no build-provenance attestation is published for $name"; return }
    & $gh.Source attestation verify $File --repo $Repo | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "provenance attestation check FAILED for $name (repo $Repo); refusing to install"
    }
    Write-Step "verified build provenance of $name"
}

function Invoke-GitHubDownload($Url, $OutFile) {
    $headers = @{
        "User-Agent" = "nordstjernen-android-fetch"
    }
    if ($Token) {
        $headers["Authorization"] = "Bearer $Token"
    }
    Invoke-WebRequest -Headers $headers -MaximumRedirection 5 -Uri $Url -OutFile $OutFile
}

$Abi = @($Abi | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$validAbi = @("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
foreach ($item in $Abi) {
    if ($validAbi -notcontains $item) {
        throw "invalid ABI: $item"
    }
}

New-Item -ItemType Directory -Force -Path $Sysroot | Out-Null
$Sysroot = (Resolve-Path $Sysroot).Path
$sysrootFull = [System.IO.Path]::GetFullPath($Sysroot)
$sysrootPrefix = $sysrootFull.TrimEnd([char[]]@("\", "/")) + [System.IO.Path]::DirectorySeparatorChar
$logDir = Join-Path (Resolve-Path "$PSScriptRoot\..") ".build\logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir "fetch-prebuilt-deps-$stamp.log"
Start-Transcript -Path $log -Force | Out-Null

try {
    Write-Step "repo: $Repo"
    Write-Step "tag: $Tag"
    Write-Step "sysroot: $Sysroot"
    Write-Step "abis: $($Abi -join ', ')"

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "nordstjernen-android-deps-$stamp"
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $baseUrl = "https://github.com/$Repo/releases/download/$Tag"
    $shaFile = Join-Path $tmp "SHA256SUMS"
    Invoke-GitHubDownload "$baseUrl/SHA256SUMS" $shaFile
    $shaLines = Get-Content $shaFile

    foreach ($item in $Abi) {
        $asset = "nordstjernen-android-sysroot-$item.tar.gz"
        $line = $shaLines | Where-Object { $_ -match [regex]::Escape($asset) } | Select-Object -First 1
        if (-not $line) {
            throw "checksum not found for $asset"
        }

        $expected = (($line -split "\s+") | Where-Object { $_ })[0].ToLowerInvariant()
        $archive = Join-Path $tmp $asset
        $unpack = Join-Path $tmp "unpack-$item"
        Write-Step "downloading $asset"
        Invoke-GitHubDownload "$baseUrl/$asset" $archive
        $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            throw "checksum mismatch for $asset"
        }
        Test-BuildProvenance $archive $actual

        New-Item -ItemType Directory -Force -Path $unpack | Out-Null
        tar -xzf $archive -C $unpack
        $source = Join-Path $unpack $item
        if (-not (Test-Path $source)) {
            throw "archive did not contain expected $item directory"
        }
        $dest = Join-Path $Sysroot $item
        $destFull = [System.IO.Path]::GetFullPath($dest)
        if (-not $destFull.StartsWith($sysrootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "refusing to replace path outside sysroot: $destFull"
        }
        if (Test-Path $dest) {
            Remove-Item -Recurse -Force -LiteralPath $dest
        }
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Copy-Item -Recurse -Force -Path (Join-Path $source "*") -Destination $dest
        $localPrefix = $destFull.Replace("\", "/")
        # The published sysroot's pkg-config / cmake metadata bakes in an absolute
        # prefix from the dependency-build repo's CI workspace
        # (/home/runner/work/<owner>/<repo>/sysroot/<abi>); rewrite it to the local
        # unpack path. Sysroots published before the dependency repo was renamed
        # carry the former nordstjernen-android workspace path, so rewrite both.
        $ciPrefixes = @(
            "/home/runner/work/nordstjernen-dependencies-build/nordstjernen-dependencies-build/sysroot/$item",
            "/home/runner/work/nordstjernen-android/nordstjernen-android/sysroot/$item"
        )
        $metadata = Get-ChildItem -Path $dest -Recurse -File -Include *.pc,*.cmake,*.la,*.pri
        foreach ($file in $metadata) {
            $text = [System.IO.File]::ReadAllText($file.FullName)
            $updated = $text
            foreach ($ciPrefix in $ciPrefixes) {
                $updated = $updated.Replace($ciPrefix, $localPrefix)
            }
            if ($updated -ne $text) {
                $utf8 = [System.Text.UTF8Encoding]::new($false)
                [System.IO.File]::WriteAllText($file.FullName, $updated, $utf8)
            }
        }
        Write-Step "installed $item -> $dest"
    }

    Write-Step "done"
    Write-Step "log: $log"
} catch {
    Write-Step "failed: $($_.Exception.Message)"
    Write-Step "log: $log"
    throw
} finally {
    Stop-Transcript | Out-Null
}
