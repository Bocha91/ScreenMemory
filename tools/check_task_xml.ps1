<#
    tools\check_task_xml.ps1 - verifies the Task Scheduler XML produced by
    "ScreenMemory.exe AutoSave --dry-run" and "AutoRestore --dry-run" (0.9.0).

    What is checked (no administrator rights needed):
      * the exe writes its XML to %TEMP% and exits with 0;
      * the file is UTF-16LE with BOM and well-formed XML in the task namespace;
      * principal: user SID, InteractiveToken, LeastPrivilege;
      * settings: IgnoreNew, StartWhenAvailable, ExecutionTimeLimit, Priority;
      * triggers: CalendarTrigger with the interval from Config.ini (AutoSave),
        LogonTrigger with the delay from Config.ini (AutoRestore);
      * action: command, arguments ("save --hidden" / "restore --hidden"),
        working directory.

    With -Register the script additionally proves that the Task Scheduler accepts the
    XML: a copy is registered under a temporary name and removed at once. The AutoSave
    XML goes in as is. The AutoRestore copy gets a TimeTrigger instead of the logon
    trigger, because Windows requires elevation for logon-trigger tasks (checked with
    "schtasks /Create /SC ONLOGON", which fails the same way in a normal session) and
    this script has to work without elevation; the rest of the XML is untouched.

    Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_task_xml.ps1 [-Register]
    Exit code: 0 - all good, 1 - something is wrong.

    NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 without a BOM
    as ANSI, so non-ASCII text breaks the parser.
#>
[CmdletBinding()]
param(
    [string]$ExePath,
    [switch]$Register
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Join-Path $ProjectRoot 'bin\ScreenMemory.exe'
}
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "not found: $ExePath (run build.bat first)"
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$ExeDir = Split-Path -Parent $ExePath
$ConfigPath = Join-Path $ExeDir 'Config.ini'

$TaskNs = 'http://schemas.microsoft.com/windows/2004/02/mit/task'
$problems = 0

function Check($ok, [string]$what, [string]$detail) {
    if ($ok) {
        Write-Host ("[check] ok   {0}" -f $what)
    }
    else {
        if ([string]::IsNullOrEmpty($detail)) {
            Write-Host ("[check] FAIL {0}" -f $what)
        }
        else {
            Write-Host ("[check] FAIL {0} (got: {1})" -f $what, $detail)
        }
        $script:problems++
    }
}

# First non-comment "key = number" from Config.ini, or the default value.
function Get-ConfigNumber([string]$key, [int]$defaultValue) {
    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        return $defaultValue
    }
    foreach ($line in (Get-Content -LiteralPath $ConfigPath -Encoding UTF8)) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            continue
        }
        $m = [regex]::Match($trimmed, '^([A-Za-z0-9_\-]+)\s*=\s*(.*)$')
        if (-not $m.Success) {
            continue
        }
        if ($m.Groups[1].Value.ToLowerInvariant() -ne $key.ToLowerInvariant()) {
            continue
        }
        $value = 0
        if ([int]::TryParse($m.Groups[2].Value.Trim(), [ref]$value)) {
            return $value
        }
        return $defaultValue
    }
    return $defaultValue
}

# Same ISO-8601 shapes as IsoInterval / IsoDelay in src\scheduler.cpp.
function Get-IsoInterval([int]$minutes) {
    if ($minutes -ge 1440 -and ($minutes % 1440) -eq 0) {
        return ('P{0}D' -f [int]($minutes / 1440))
    }
    if ($minutes -ge 60 -and ($minutes % 60) -eq 0) {
        return ('PT{0}H' -f [int]($minutes / 60))
    }
    return ('PT{0}M' -f $minutes)
}

function Get-IsoDelay([int]$seconds) {
    if ($seconds -le 0) {
        return ''
    }
    if (($seconds % 60) -eq 0) {
        return ('PT{0}M' -f [int]($seconds / 60))
    }
    return ('PT{0}S' -f $seconds)
}

function Get-NodeText($doc, $nsMgr, [string]$xpath) {
    $node = $doc.SelectSingleNode($xpath, $nsMgr)
    if ($null -eq $node) {
        return ''
    }
    return $node.InnerText.Trim()
}

# Runs "<exe> <command> --dry-run" and leaves the XML where the exe puts it.
function Invoke-DryRun([string]$command, [string]$xmlPath) {
    if (Test-Path -LiteralPath $xmlPath) {
        Remove-Item -LiteralPath $xmlPath -Force
    }
    $stdout = Join-Path $env:TEMP 'check_task_xml_out.txt'
    $stderr = Join-Path $env:TEMP 'check_task_xml_err.txt'
    $p = Start-Process -FilePath $ExePath -ArgumentList ($command + ' --dry-run') -Wait -PassThru `
         -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    return $p.ExitCode
}

function Invoke-Schtasks([string]$cmdLine) {
    $stdout = Join-Path $env:TEMP 'check_task_xml_schtasks.txt'
    $stderr = Join-Path $env:TEMP 'check_task_xml_schtasks_err.txt'
    $p = Start-Process -FilePath 'schtasks.exe' -ArgumentList $cmdLine -Wait -PassThru `
         -NoNewWindow -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    return $p.ExitCode
}

# Common part of both tasks plus the trigger shape: -Logon switches from the
# "every N minutes" calendar trigger to the "at logon" trigger.
function Test-Task([string]$label, [string]$xmlPath, [string]$arguments, [string]$timeLimit,
                   [switch]$logon, [string]$intervalIso, [string]$delayIso) {
    if (-not (Test-Path -LiteralPath $xmlPath)) {
        Check $false ("{0}: XML file exists" -f $label) $xmlPath
        return
    }
    $bytes = [System.IO.File]::ReadAllBytes($xmlPath)
    $bom = ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE)
    Check $bom ("{0}: UTF-16LE BOM" -f $label) ('{0:X2} {1:X2}' -f $bytes[0], $bytes[1])

    $doc = New-Object System.Xml.XmlDocument
    try {
        $doc.Load($xmlPath)
        Check $true ("{0}: well-formed XML" -f $label)
    }
    catch {
        Check $false ("{0}: well-formed XML" -f $label) $_.Exception.Message
        return
    }
    $nsMgr = New-Object System.Xml.XmlNamespaceManager($doc.NameTable)
    $nsMgr.AddNamespace('t', $TaskNs)
    $root = $doc.DocumentElement
    Check ($root.LocalName -eq 'Task' -and $root.NamespaceURI -eq $TaskNs) ("{0}: root <Task> in the task namespace" -f $label) $root.NamespaceURI

    $sid = Get-NodeText $doc $nsMgr '/t:Task/t:Principals/t:Principal/t:UserId'
    Check ($sid -match '^S-1-') ("{0}: principal is the current user SID" -f $label) $sid
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Principals/t:Principal/t:LogonType') -eq 'InteractiveToken') ("{0}: LogonType=InteractiveToken (no password stored)" -f $label)
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Principals/t:Principal/t:RunLevel') -eq 'LeastPrivilege') ("{0}: RunLevel=LeastPrivilege" -f $label)

    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Settings/t:MultipleInstancesPolicy') -eq 'IgnoreNew') ("{0}: MultipleInstancesPolicy=IgnoreNew" -f $label)
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Settings/t:StartWhenAvailable') -eq 'true') ("{0}: StartWhenAvailable=true" -f $label)
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Settings/t:ExecutionTimeLimit') -eq $timeLimit) ("{0}: ExecutionTimeLimit={1}" -f $label, $timeLimit)
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Settings/t:Priority') -eq '7') ("{0}: Priority=7 (background)" -f $label)

    if ($logon) {
        Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:LogonTrigger') -ne '') ("{0}: LogonTrigger present" -f $label)
        Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:LogonTrigger/t:Enabled') -eq 'true') ("{0}: LogonTrigger enabled" -f $label)
        $delay = Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:LogonTrigger/t:Delay'
        if ([string]::IsNullOrEmpty($delayIso)) {
            Check ([string]::IsNullOrEmpty($delay)) ("{0}: no <Delay> when the delay is 0" -f $label) $delay
        }
        else {
            Check ($delay -eq $delayIso) ("{0}: Delay={1}" -f $label, $delayIso) $delay
        }
        Check ($null -eq $doc.SelectSingleNode('/t:Task/t:Triggers/t:CalendarTrigger', $nsMgr)) ("{0}: no CalendarTrigger" -f $label)
    }
    else {
        $trigger = Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:CalendarTrigger'
        Check ($trigger -ne '') ("{0}: CalendarTrigger present" -f $label)
        Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:CalendarTrigger/t:Repetition/t:Interval') -eq $intervalIso) ("{0}: Repetition/Interval={1}" -f $label, $intervalIso)
        Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:CalendarTrigger/t:ScheduleByDay/t:DaysInterval') -eq '1') ("{0}: ScheduleByDay/DaysInterval=1" -f $label)
        $start = Get-NodeText $doc $nsMgr '/t:Task/t:Triggers/t:CalendarTrigger/t:StartBoundary'
        $startOk = $false
        try {
            $when = [datetime]::ParseExact($start, 'yyyy-MM-ddTHH:mm:ss', $null)
            # First run must be in the future: otherwise StartWhenAvailable would fire at once.
            $startOk = $when -gt (Get-Date).AddMinutes(-2)
        }
        catch {
            $startOk = $false
        }
        Check $startOk ("{0}: StartBoundary is a valid future time" -f $label) $start
        Check ($null -eq $doc.SelectSingleNode('/t:Task/t:Triggers/t:LogonTrigger', $nsMgr)) ("{0}: no LogonTrigger" -f $label)
    }

    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Actions/t:Exec/t:Command') -eq $ExePath) ("{0}: Command is this exe" -f $label) (Get-NodeText $doc $nsMgr '/t:Task/t:Actions/t:Exec/t:Command')
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Actions/t:Exec/t:Arguments') -eq $arguments) ("{0}: Arguments='{1}'" -f $label, $arguments) (Get-NodeText $doc $nsMgr '/t:Task/t:Actions/t:Exec/t:Arguments')
    Check ((Get-NodeText $doc $nsMgr '/t:Task/t:Actions/t:Exec/t:WorkingDirectory') -eq $ExeDir) ("{0}: WorkingDirectory is the exe folder" -f $label) (Get-NodeText $doc $nsMgr '/t:Task/t:Actions/t:Exec/t:WorkingDirectory')
}


Write-Host ("[check] exe: {0}" -f $ExePath)
$autoSaveMinutes = [Math]::Min([Math]::Max((Get-ConfigNumber 'AutoSave' 10), 1), 1440)
$autoRestoreDelay = [Math]::Min([Math]::Max((Get-ConfigNumber 'AutoRestoreDelay' 30), 0), 3600)
$intervalIso = Get-IsoInterval $autoSaveMinutes
$delayIso = Get-IsoDelay $autoRestoreDelay
Write-Host ("[check] Config.ini: AutoSave={0} min ({1}), AutoRestoreDelay={2} s ({3})" -f `
            $autoSaveMinutes, $intervalIso, $autoRestoreDelay, $delayIso)

$autoSaveXml = Join-Path $env:TEMP 'ScreenMemory-AutoSave.xml'
$rc = Invoke-DryRun 'AutoSave' $autoSaveXml
Check ($rc -eq 0) 'AutoSave --dry-run exits 0' ('exit ' + $rc)
Test-Task 'AutoSave' $autoSaveXml 'save --hidden' 'PT10M' -intervalIso $intervalIso

$autoRestoreXml = Join-Path $env:TEMP 'ScreenMemory-AutoRestore.xml'
$rc = Invoke-DryRun 'AutoRestore' $autoRestoreXml
Check ($rc -eq 0) 'AutoRestore --dry-run exits 0' ('exit ' + $rc)
Test-Task 'AutoRestore' $autoRestoreXml 'restore --hidden' 'PT0S' -logon -delayIso $delayIso

if ($Register) {
    Write-Host '[check] -Register: the Task Scheduler itself must accept the XML'
    $tmpName = 'ScreenMemory XmlCheck AutoSave'
    $rc = Invoke-Schtasks ('/Create /TN "' + $tmpName + '" /XML "' + $autoSaveXml + '" /F')
    Check ($rc -eq 0) 'scheduler accepted the AutoSave XML' ('schtasks exit ' + $rc)
    if ($rc -eq 0) {
        $del = Invoke-Schtasks ('/Delete /TN "' + $tmpName + '" /F')
        Check ($del -eq 0) 'temporary AutoSave task removed' ('schtasks exit ' + $del)
    }

    # Logon triggers need elevation, so the copy is registered with a time trigger:
    # principal, settings and the action stay exactly as the exe built them.
    $patchedPath = Join-Path $env:TEMP 'ScreenMemory-AutoRestore-TimeTrigger.xml'
    $text = [System.IO.File]::ReadAllText($autoRestoreXml, [System.Text.Encoding]::Unicode)
    $when = (Get-Date).AddHours(1).ToString('yyyy-MM-ddTHH:mm:ss')
    $time = '<TimeTrigger><StartBoundary>' + $when + '</StartBoundary><Enabled>true</Enabled></TimeTrigger>'
    $patched = [regex]::Replace($text, '(?s)<LogonTrigger>.*?</LogonTrigger>', $time)
    Check ($patched -ne $text) 'AutoRestore copy really lost the logon trigger'
    [System.IO.File]::WriteAllText($patchedPath, $patched, [System.Text.Encoding]::Unicode)
    $tmpName = 'ScreenMemory XmlCheck AutoRestore'
    $rc = Invoke-Schtasks ('/Create /TN "' + $tmpName + '" /XML "' + $patchedPath + '" /F')
    Check ($rc -eq 0) 'scheduler accepted the AutoRestore XML (logon trigger swapped for a time trigger)' ('schtasks exit ' + $rc)
    if ($rc -eq 0) {
        $del = Invoke-Schtasks ('/Delete /TN "' + $tmpName + '" /F')
        Check ($del -eq 0) 'temporary AutoRestore task removed' ('schtasks exit ' + $del)
    }
}

if ($problems -eq 0) {
    Write-Host '[check] PASS'
    exit 0
}
Write-Host ("[check] FAIL: {0} problem(s)" -f $problems)
exit 1
