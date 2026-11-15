#pragma once

#include <windows.h>

#include <string>

// Папка окна Проводника (PLAN.md §9.2): именно папка, а не заголовок — главный
// ключ для восстановления, потому что заголовки у окон Проводника повторяются.
namespace sm {

// CabinetWClass / ExploreWClass — окна Проводника (у них есть папка).
bool IsExplorerWindowClass(const std::wstring& cls);

// Путь папки конкретного окна Проводника. Для виртуальных папок (Быстрый доступ,
// Корзина, «Этот компьютер») файлового пути не существует, поэтому возвращается
// «parsing name» с префиксом `shell:` — такой путь потом запускается как
// explorer.exe "shell:...". false — папку определить не удалось.
//
// `title` — заголовок окна: для окон на неактивных виртуальных столах папка
// восстанавливается по нему (см. §9.2 PLAN.md), остальным он не нужен.
//
// Требует инициализированного COM в режиме STA (IShellWindows — apartment-threaded).
bool GetExplorerFolder(HWND hwnd, const std::wstring& title, std::wstring* folder);

}  // namespace sm
