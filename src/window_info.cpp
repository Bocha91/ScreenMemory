#include "window_info.h"

#include <dwmapi.h>

#include <cstdio>

#include <cwchar>

#include "process_info.h"
#include "shell_explorer.h"

namespace {

// Заголовок чужого окна читаем через WM_GETTEXT с тайм-аутом: GetWindowTextW
// на зависшем приложении может подвесить и нас (PLAN.md §7 п.5).
std::wstring ReadTitle(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return std::wstring();
    }

    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    const LRESULT got = SendMessageTimeoutW(hwnd, WM_GETTEXT,
                                            static_cast<WPARAM>(text.size()),
                                            reinterpret_cast<LPARAM>(&text[0]),
                                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied);
    if (got == 0 && copied == 0) {
        return std::wstring();  // окно не ответило за 100 мс — считаем, что заголовка нет
    }
    text.resize(wcsnlen(text.c_str(), text.size()));
    return text;
}

std::wstring ReadClassName(HWND hwnd) {
    wchar_t buf[256] = L"";
    GetClassNameW(hwnd, buf, _countof(buf));
    return buf;
}

// У UWP-приложений заголовок часто лежит не на рамке ApplicationFrameWindow, а на
// дочернем Windows.UI.Core.CoreWindow. Если у рамки заголовка нет — берём из ребёнка.
std::wstring ReadUwpChildTitle(HWND hwnd) {
    const HWND child =
        FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
    if (child == nullptr) {
        return std::wstring();
    }
    return ReadTitle(child);
}

std::wstring FormatRect(const RECT& rect) {
    wchar_t buf[64] = L"";
    swprintf_s(buf, L"%ld,%ld %ldx%ld", static_cast<long>(rect.left),
               static_cast<long>(rect.top), static_cast<long>(rect.right - rect.left),
               static_cast<long>(rect.bottom - rect.top));
    return buf;
}

}  // namespace

namespace sm {

bool GetWindowInfo(HWND hwnd, WindowInfo* out) {
    if (out == nullptr) {
        return false;
    }
    *out = WindowInfo{};
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) {
        return false;
    }

    out->hwnd = hwnd;
    out->visible = IsWindowVisible(hwnd) != FALSE;
    out->owned = GetWindow(hwnd, GW_OWNER) != nullptr;
    out->style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    out->exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    out->cls = ReadClassName(hwnd);
    out->title = ReadTitle(hwnd);
    if (out->title.empty() && out->cls == L"ApplicationFrameWindow") {
        out->title = ReadUwpChildTitle(hwnd);
    }
    GetWindowThreadProcessId(hwnd, &out->pid);

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) != FALSE) {
        out->placement = placement;
        out->placementOk = true;
    }

    out->rectOk = GetWindowRect(hwnd, &out->rect) != FALSE;

    RECT frame{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        out->frame = frame;
        out->frameOk = true;
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        out->cloaked = cloaked;
    }
    return true;
}

bool GetWindowDetails(WindowInfo* info) {
    if (info == nullptr || info->hwnd == nullptr) {
        return false;
    }

    const ProcessInfo& process = ProcessInfoFor(info->pid);
    info->exePath = process.exePath;
    info->exeName = process.exeName;
    info->cmdline = process.cmdline;
    info->exeError = process.error;
    info->aumid = WindowAppUserModelId(info->hwnd);

    // Папка есть только у окон Проводника; для остальных IShellWindows не нужен.
    info->folder.clear();
    if (IsExplorerWindowClass(info->cls)) {
        GetExplorerFolder(info->hwnd, info->title, &info->folder);
    }
    return true;
}

const wchar_t* CloakedText(DWORD cloaked) {
    switch (cloaked & (CloakedApp | CloakedShell | CloakedInherited)) {
        case CloakedNone:
            return L"—";
        case CloakedApp:
            return L"app";
        case CloakedShell:
            return L"shell";
        case CloakedInherited:
            return L"inh";
        case CloakedApp | CloakedShell:
            return L"app+shell";
        case CloakedApp | CloakedInherited:
            return L"app+inh";
        case CloakedShell | CloakedInherited:
            return L"shell+inh";
        default:
            return L"app+shell+inh";
    }
}

const wchar_t* ShowStateText(int showCmd) {
    switch (showCmd) {
        case SW_SHOWMAXIMIZED:
            return L"max";
        case SW_SHOWMINIMIZED:
            return L"min";
        case SW_SHOWNORMAL:
            return L"normal";
        default:
            return L"?";
    }
}

const wchar_t* ShowStateText(const WindowInfo& info) {
    return info.placementOk ? ShowStateText(info.placement.showCmd) : L"?";
}

std::wstring RectText(const RECT& rect) {
    return FormatRect(rect);
}

std::wstring RectText(const WindowInfo& info) {
    if (info.frameOk) {
        return FormatRect(info.frame);
    }
    if (info.rectOk) {
        return FormatRect(info.rect);
    }
    return L"—";
}

std::wstring NormalRectText(const WindowInfo& info) {
    if (!info.placementOk) {
        return std::wstring();
    }
    return FormatRect(info.placement.rcNormalPosition);
}

}  // namespace sm
