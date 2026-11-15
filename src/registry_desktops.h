#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Виртуальные рабочие столы и pinned-приложения из реестра (см. PLAN.md §9.1).
//
// Источник (проверено на этой машине, Win10 22H2 19045.6456):
//   HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops
//     VirtualDesktopIDs      REG_BINARY, по 16 байт на десктоп — порядок + GUID
//     Desktops\{GUID}\Name   REG_SZ, имени может не быть
//     PinnedApps             значения: имя значения = путь к exe, AUMID
//                            или "{ИзвестнаяПапка}\относительный\путь.exe"
//
// GUID — главный идентификатор десктопа (стабилен при переименовании),
// имя — человекочитаемая метка (может отсутствовать и повторяться),
// индекс — порядок из VirtualDesktopIDs (последний резерв при сопоставлении).
namespace sm {

struct DesktopInfo {
    GUID guid{};        // идентификатор десктопа
    std::wstring name;  // имя из реестра; пусто, если имени нет
    int index = 0;      // 0 — самый левый в Task View
};

struct PinnedApp {
    std::wstring raw;       // имя значения из реестра, как есть
    std::wstring kind;      // "путь" | "aumid" | "known"
    std::wstring resolved;  // для "known" — путь после подстановки известной папки
    DWORD type = 0;         // REG_SZ / REG_DWORD / ...
    DWORD data = 0;         // значение для REG_DWORD, иначе 0
};

// Порядок и GUID десктопов. false — не удалось прочитать VirtualDesktopIDs.
bool ReadDesktops(std::vector<DesktopInfo>* out, std::wstring* error);

// Имя десктопа по GUID (пустая строка, если имени нет или ключа нет).
std::wstring ReadDesktopName(const GUID& guid);

// Pinned-приложения: набор значений из ключа PinnedApps.
bool ReadPinnedApps(std::vector<PinnedApp>* out, std::wstring* error);

// Текущий десктоп по реестру (значение CurrentVirtualDesktop).
// На Win10 22H2 этого значения нет — тогда false, и текущий десктоп определяется через COM.
bool ReadCurrentDesktopFromRegistry(GUID* out);

std::wstring GuidToString(const GUID& guid);          // "{XXXXXXXX-XXXX-...}", верхний регистр
bool ParseGuid(const std::wstring& text, GUID* out);  // принимает "{...}" и "..." без скобок
bool IsNullGuid(const GUID& guid);

// Индекс десктопа в списке; -1, если такого GUID нет.
int FindDesktopIndex(const std::vector<DesktopInfo>& desktops, const GUID& guid);

// Классификация имени значения из PinnedApps и подстановка известной папки:
//   "known" — "{6D809377-6AF0-444B-8957-A3773F02200E}\OpenConnect-GUI\x.exe"
//   "aumid" — "Microsoft.VisualStudioCode", "Telegram.TelegramDesktop.2cb78e44..."
//   "путь"  — обычный путь к exe
void ExplainPinnedName(const std::wstring& raw, std::wstring* kind, std::wstring* resolved);

// Совпадает ли pinned-запись с приложением. Используется на этапе 5 при разборе слепка.
bool PinnedMatches(const PinnedApp& pinned, const std::wstring& exePath, const std::wstring& aumid);

}  // namespace sm
