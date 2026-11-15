#pragma once

#include <windows.h>

#include <string>

// Консольный вывод и лог-файл.
//
// Правила:
//  * Info/Warn/Error видны всегда: в консоль и (если включён --verbose) в файл-лог.
//  * Trace виден только при --verbose (консоль + файл).
//  * Консоль пишется через WriteConsoleW (кириллица без плясок с кодовыми страницами);
//    если stdout перенаправлен в файл/канал — пишем UTF-8 без BOM.
//  * Лог-файл: T:\ScreenMemory\screenmemory.log (UTF-8 с BOM, append). Если диска T: нет —
//    рядом с exe, чтобы утилита оставалась переносимой.
namespace sm {

enum class Level {
    Info,
    Warn,
    Error,
    Trace,
};

// Открывает лог-файл, если verbose = true. Безопасно вызывать один раз в начале main.
void InitLog(bool verbose);
void ShutdownLog();
bool IsVerbose();

void Logf(Level level, const wchar_t* fmt, ...);

// Короткие обёртки: ими и пользуемся в остальном коде.
void Info(const wchar_t* fmt, ...);
void Warn(const wchar_t* fmt, ...);
void Error(const wchar_t* fmt, ...);
void Trace(const wchar_t* fmt, ...);

// Пути (вычисляются один раз, кэшируются).
const std::wstring& ExePath();   // полный путь к ScreenMemory.exe
const std::wstring& ExeDir();    // каталог exe, без завершающего слэша
const std::wstring& LogFilePath();

// dir + "\" + name (без хитростей с путями вида "C:" — они нам не нужны).
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
// Тексты системных ошибок — чтобы в отчёте и логе было «5: Отказано в доступе»,
// а не голое число. Для HRESULT из Win32-диапазона (0x8007xxxx) печатается и текст системы.
std::wstring Win32ErrorText(DWORD code);
std::wstring HrText(HRESULT hr);



}  // namespace sm
