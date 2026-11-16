# check-windows-bundle.ps1 — start the packaged Windows bundle with only the Windows system folders on PATH.
# Fails when a DLL the bundle needs is missing from it (the MSIX package never searches PATH either)
# or when start-up blocks, e.g. on a DLL's error dialog.
param(
    [string]$Bundle = "",
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"
if (-not $Bundle) {
    $root = Resolve-Path (Join-Path $PSScriptRoot "..")
    $Bundle = Join-Path $root "dist\nordstjernen-win64"
}
$checks = @(
    @{ Exe = "nordstjernen.exe"; Args = "--print-config"; Expect = "" },
    @{ Exe = "app\nordstjernen-ui.exe"; Args = "--print-config"; Expect = "" }
)
if (Test-Path -LiteralPath (Join-Path $Bundle "app\nordstjernen-audio.exe")) {
    $checks += @{ Exe = "app\nordstjernen-audio.exe"; Args = ""; Expect = "ready" }
}
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class NsBundleErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);
}
"@
[NsBundleErrorMode]::SetErrorMode(0x8003) | Out-Null
$systemPath = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
foreach ($check in $checks) {
    $target = Join-Path $Bundle $check.Exe
    if (-not (Test-Path -LiteralPath $target)) {
        throw "Bundle executable not found: $target"
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo $target, $check.Args
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.WorkingDirectory = $env:SystemRoot
    $info.EnvironmentVariables["PATH"] = $systemPath
    $info.EnvironmentVariables["NS_ALLOW_ROOT"] = "1"
    $process = [System.Diagnostics.Process]::Start($info)
    $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        Get-Process -Name nordstjernen, nordstjernen-ui, nordstjernen-audio -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
        throw "$target did not exit within $TimeoutSeconds s; a DLL is probably showing an error dialog"
    }
    $process.WaitForExit()
    $output = ($stdout.Result + $stderr.Result).Trim()
    if ($process.ExitCode -ne 0) {
        throw ("{0} {1} exited with 0x{2:X8}: {3}" -f $target, $check.Args, $process.ExitCode, $output)
    }
    if ($check.Expect -and -not $output.Contains($check.Expect)) {
        throw "$target printed no '$($check.Expect)': $output"
    }
    $first = ($output -split "`n")[0]
    "{0} {1}: ok ({2})" -f $check.Exe, $check.Args, $first
}
"bundle-check-ok"
