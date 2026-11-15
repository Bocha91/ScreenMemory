#pragma once

#include <windows.h>

#include <string>

// Автозапуск (PLAN.md §11, CONTEXT п. 65): две задачи Планировщика заданий —
// AutoSave (каждые N минут) и AutoRestore (при входе в систему с задержкой).
//
// Задачи регистрируются через schtasks.exe с XML-описанием: так не зависит от локали
// системы, пути с пробелами и кириллицей переживаются, а задержка входа задаётся точно.
// Регистрация задачи «при входе в систему» требует прав администратора (проверено:
// и schtasks, и COM-интерфейс отвечают «Отказано в доступе» без повышения) — поэтому
// AutoSave/AutoRestore/Uninstall проверяют права и не пытаются обойти UAC.
namespace sm {

// Имена задач — их видно в «Планировщик заданий → Библиотека планировщика заданий».
extern const wchar_t* const kAutoSaveTaskName;     // «ScreenMemory AutoSave»
extern const wchar_t* const kAutoRestoreTaskName;  // «ScreenMemory AutoRestore»

// Значение в HKCU\Software\Microsoft\Windows\CurrentVersion\Run. Своих записей туда
// программа не делает: ключ нужен только Uninstall — убрать следы ручных установок.
extern const wchar_t* const kRunValueName;  // «ScreenMemory»

// Процесс запущен с повышенными правами (UAC).
bool IsElevated();

struct TaskInstallResult {
    bool alreadyExisted = false;  // задача уже была и перезаписана новыми параметрами
    std::wstring schedule;        // расписание человеческим языком — для отчёта
    std::wstring xmlPath;         // где лежит XML задачи (в dryRun остаётся на диске)
};

struct TaskOptions {
    // true — только собрать XML и показать расписание, ничего не регистрировать
    // и не удалять. Права администратора для проверки не нужны.
    bool dryRun = false;
};

// Создать (или обновить) задачу AutoSave: «каждый день, повторять каждые N минут».
// intervalMinutes зажимается в 1…1440. Команда задачи — «<exe> save --hidden».
bool InstallAutoSaveTask(int intervalMinutes, const TaskOptions& options,
                         TaskInstallResult* result, std::wstring* error);

// Создать (или обновить) задачу AutoRestore: «при входе в систему, задержка N секунд».
// delaySeconds зажимается в 0…3600, 0 — без задержки. Команда — «<exe> restore --hidden».
bool InstallAutoRestoreTask(int delaySeconds, const TaskOptions& options,
                            TaskInstallResult* result, std::wstring* error);

// Снять задачу. found — задача есть (в dryRun) либо была и удалена (обычный режим).
bool DeleteTask(const wchar_t* name, const TaskOptions& options, bool* found,
                std::wstring* error);

// Снять значение автозагрузки из HKCU\...\Run. found — значение есть/было.
bool DeleteRunValue(const TaskOptions& options, bool* found, std::wstring* error);

}  // namespace sm
