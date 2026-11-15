<#
    tools\check_exe.ps1 - sanity check of the built bin\ScreenMemory.exe:
      * both icons (ID 101 = save, ID 102 = restore) are embedded;
      * the manifest (RT_MANIFEST) is embedded and contains PerMonitorV2 + asInvoker;
      * VERSIONINFO is readable.

    Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_exe.ps1
    Exit code: 0 - all good, 1 - something is wrong.

    NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 without a BOM
    as ANSI, so non-ASCII text breaks the parser.
#>
[CmdletBinding()]
param(
    [string]$ExePath
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $ProjectRoot 'bin\ScreenMemory.exe'
}
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "not found: $ExePath (run build.bat first)"
}

Add-Type -Namespace SMCheck -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr LoadLibraryExW(string lpLibFileName, System.IntPtr hFile, uint dwFlags);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern bool FreeLibrary(System.IntPtr hModule);
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern System.IntPtr LoadIconW(System.IntPtr hInstance, System.IntPtr lpIconName);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindResourceW(System.IntPtr hModule, System.IntPtr lpName, System.IntPtr lpType);
'@

$LOAD_LIBRARY_AS_DATAFILE = 0x00000002
$problems = 0

$hmod = [SMCheck.Native]::LoadLibraryExW($ExePath, [System.IntPtr]::Zero, $LOAD_LIBRARY_AS_DATAFILE)
if ($hmod -eq [System.IntPtr]::Zero) {
    throw ("LoadLibraryExW failed, error {0}" -f [System.Runtime.InteropServices.Marshal]::GetLastWin32Error())
}

try {
    foreach ($pair in @(@{ Id = 101; Name = 'IDI_SAVE' }, @{ Id = 102; Name = 'IDI_RESTORE' })) {
        $icon = [SMCheck.Native]::LoadIconW($hmod, [System.IntPtr]$pair.Id)
        if ($icon -eq [System.IntPtr]::Zero) {
            Write-Host ("[check] FAIL icon {0} (id {1}) is missing" -f $pair.Name, $pair.Id)
            $problems++
        }
        else {
            Write-Host ("[check] ok   icon {0} (id {1})" -f $pair.Name, $pair.Id)
        }
    }

    $manifest = [SMCheck.Native]::FindResourceW($hmod, [System.IntPtr]1, [System.IntPtr]24)  # RT_MANIFEST = 24
    if ($manifest -eq [System.IntPtr]::Zero) {
        Write-Host '[check] FAIL manifest is not embedded'
        $problems++
    }
    else {
        Write-Host '[check] ok   manifest embedded'
    }
}
finally {
    [void][SMCheck.Native]::FreeLibrary($hmod)
}

# The manifest is plain text inside the exe: look for the key strings in the file bytes.
$bytes = [System.IO.File]::ReadAllBytes($ExePath)
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
foreach ($needle in @('PerMonitorV2', 'asInvoker', 'Microsoft.Windows.Common-Controls')) {
    if ($text.Contains($needle)) {
        Write-Host ("[check] ok   manifest contains: {0}" -f $needle)
    }
    else {
        Write-Host ("[check] FAIL manifest has no: {0}" -f $needle)
        $problems++
    }
}

$info = (Get-Item -LiteralPath $ExePath).VersionInfo
Write-Host ("[check] version: {0} file={1}" -f $info.FileVersion, (Split-Path -Leaf $ExePath))

if ($problems -eq 0) {
    Write-Host '[check] PASS'
    exit 0
}
Write-Host ("[check] FAIL, problems: {0}" -f $problems)
exit 1
