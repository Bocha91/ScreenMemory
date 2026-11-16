<#
    tests\check_rights.ps1 - проверка разделения прав при восстановлении
    (0.9.3, PLAN.md §9.3).

    Вход: вывод "restore --dry-run" (UTF-8) на слепке, где единственное окно
    записано с elevated=1, и коды возврата. Обычный (не повышенный) restore
    не может поднять такое приложение без UAC, поэтому запись обязана уйти в
    admin.log, а отчёт - сказать, что делать.

    Проверяется:
      * код возврата совпал с ожидаемым;
      * в отчёте есть строка пропуска "окно было открыто от администратора";
      * в отчёте есть подсказка "окон от администратора не запущено: 1";
      * запись НЕ попала в "запустил бы": в плане только обычная запись
        (иначе UAC спрашивали бы на каждое приложение).

    Если сам тест запущен из повышенного процесса (администраторская консоль),
    проверка неприменима: такой restore поднимает elevated=1 своим токеном.
    Тогда печатается skip и код 0.

    ВАЖНО: файл сохранён в UTF-8 с BOM. Windows PowerShell 5.1 без BOM читает
    .ps1 как ANSI и русские шаблоны ниже ломаются.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$LogPath,
    [int]$ExpectedExit = 0,
    [int]$ActualExit = 0
)

$ErrorActionPreference = 'Stop'
$problems = 0

if (-not (Test-Path -LiteralPath $LogPath)) {
    Write-Host ("[rights] FAIL log not found: {0}" -f $LogPath)
    exit 1
}

$text = [System.IO.File]::ReadAllText($LogPath, [System.Text.Encoding]::UTF8)

if ($text.Contains('правами администратора')) {
    Write-Host '[rights] skip elevated run: права разделяет сам restore'
    exit 0
}

if ($ActualExit -eq $ExpectedExit) {
    Write-Host ("[rights] ok   exit code {0}" -f $ActualExit)
} else {
    Write-Host ("[rights] FAIL exit code {0}, expected {1}" -f $ActualExit, $ExpectedExit)
    $problems++
}

$checks = @(
    @{ Needle = 'окно было открыто от администратора'; Text = 'elevated window is reported as skipped' },
    @{ Needle = 'окон от администратора не запущено: 1'; Text = 'hint about elevated restore is printed' }
)
foreach ($check in $checks) {
    if ($text.Contains($check.Needle)) {
        Write-Host ("[rights] ok   {0}" -f $check.Text)
    } else {
        Write-Host ("[rights] FAIL {0}" -f $check.Text)
        $problems++
    }
}

$plainLaunches = [regex]::Matches($text, 'запустил бы').Count
if ($plainLaunches -eq 1) {
    Write-Host '[rights] ok   only the plain window is planned for launch'
} else {
    Write-Host ("[rights] FAIL planned launches: {0}, expected 1 (plain window only)" -f $plainLaunches)
    $problems++
}

if ($problems -eq 0) {
    Write-Host '[rights] PASS'
    exit 0
}
Write-Host ("[rights] FAIL, problems: {0}" -f $problems)
exit 1
