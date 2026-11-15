#pragma once

#include <windows.h>

#include <string>

// Сведения о процессе окна (PLAN.md §7 п.5): путь exe, имя файла, командная
// строка. Плюс AppUserModelID окна — он нужен и для UWP, и для сопоставления
// с PinnedApps (в реестре там вместо пути бывает AUMID).
namespace sm {

struct ProcessInfo {
    std::wstring exePath;  // QueryFullProcessImageNameW — полный путь
    std::wstring exeName;  // только имя файла: «explorer.exe»
    std::wstring cmdline;  // NtQueryInformationProcess(ProcessCommandLineInformation)
    DWORD error = 0;       // почему exePath пуст (код WinAPI); 0 — путь прочитан
};

// Опрос процесса с кэшем на время запуска: у одного приложения обычно несколько
// окон, а OpenProcess на каждое — лишние обращения к системе.
const ProcessInfo& ProcessInfoFor(DWORD pid);
void ClearProcessCache();

// Опрос без кэша. false — процесс не открылся или путь не прочитан (см. error).
bool ReadProcessInfo(DWORD pid, ProcessInfo* out);

// AppUserModelID окна (PKEY_AppUserModel_ID). У обычных Win32-приложений его
// обычно нет; у UWP рамка ApplicationFrameWindow отдаёт AUMID приложения.
std::wstring WindowAppUserModelId(HWND hwnd);

// «C:\Program Files\App\app.exe» -> «app.exe».
std::wstring FileNameOf(const std::wstring& path);

}  // namespace sm
