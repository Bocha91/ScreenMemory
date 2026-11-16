#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Настройки из Config.ini рядом с exe (CONTEXT п. 64).
//
// Файл необязателен: нет файла — работают значения по умолчанию и ничего не печатается.
// Формат тот же, что у layout.txt: строки «ключ=значение», ';' и '#' в начале строки —
// комментарий, секции [имя] допускаются и на разбор не влияют. Кодировка: UTF-8 (с BOM
// и без) либо UTF-16LE с BOM — в путях бывает кириллица («C:\с рабочего стола\...»).
// Значения разворачиваются как переменные среды (%USERPROFILE% и т. п.).
namespace sm {

struct Config {
    std::wstring path;                   // полный путь к Config.ini
    bool exists = false;                 // файл найден и прочитан
    std::wstring preLaunch;              // программа перед restore (пусто — не запускать)
    std::wstring preLaunchArgs;          // её аргументы командной строки
    int preLaunchTimeoutSec = 0;         // ожидание закрытия её окна, 0 — без ограничения
    int autoSaveMinutes = 10;            // ключ AutoSave: интервал задачи AutoSave, минуты
    bool autoSaveSet = false;            // ключ AutoSave был в файле (для отчёта)
    int autoRestoreDelaySec = 30;        // ключ AutoRestoreDelay: задержка входа, секунды
    bool autoRestoreDelaySet = false;    // ключ AutoRestoreDelay был в файле (для отчёта)
    int layoutVersions = 3;              // ключ Layouts: сколько рабочих версий слепка хранить
    bool layoutVersionsSet = false;      // ключ Layouts был в файле (для отчёта)
    // Ключ settle-wait: сколько секунд ждать «успокоения» окон перед фазой B (запуск
    // приложений) и сколько максимум ждать появления окна, которое система открывает сама
    // (CONTEXT п. 69). 0 — не ждать вовсе.
    int settleWaitSec = 10;
    bool settleWaitSet = false;          // ключ settle-wait был в файле (для отчёта)
    std::vector<std::wstring> unknownKeys;  // ключи, которых программа не знает (для --verbose)
};

// false — файл есть, но прочитать его не удалось (error — причина). Отсутствие файла —
// не ошибка: config->exists = false, все значения по умолчанию.
bool LoadConfig(Config* config, std::wstring* error);

}  // namespace sm
