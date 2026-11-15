#include "browser_url.h"

#include <UIAutomation.h>
#include <cwctype>
#include <string>

#include "log.h"

// CLSID/IID берём из заголовков SDK, но сами функции UIA не вызываем — только
// CoCreateInstance, поэтому достаточно uiautomationcore.lib (она есть в любом SDK).
#pragma comment(lib, "uiautomationcore.lib")

namespace sm {
namespace {

bool SameText(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring Lower(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return out;
}

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

// UIA создаётся один раз на процесс; неудача запоминается, чтобы не пытаться
// снова на каждом окне.
IUIAutomation* g_uia = nullptr;
bool g_uiaUnavailable = false;

IUIAutomation* Uia() {
    if (g_uia != nullptr) {
        return g_uia;
    }
    if (g_uiaUnavailable) {
        return nullptr;
    }
    IUIAutomation* uia = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&uia));
    if (FAILED(hr) || uia == nullptr) {
        g_uiaUnavailable = true;
        Trace(L"UIA: CoCreateInstance(CUIAutomation) не удался (%s) — адреса страниц не читаются",
              HrText(hr).c_str());
        return nullptr;
    }
    // Чужое приложение может не отвечать на UIA: ограничиваем ожидание, иначе save
    // «зависает» на одном окне.
    IUIAutomation2* uia2 = nullptr;
    if (SUCCEEDED(uia->QueryInterface(IID_PPV_ARGS(&uia2))) && uia2 != nullptr) {
        uia2->put_ConnectionTimeout(2000);
        uia2->put_TransactionTimeout(3000);
        uia2->Release();
    }
    g_uia = uia;
    return g_uia;
}

// Похоже на адрес? Отсекает служебные Edit-контролы («Номер страницы» = «9»,
// «Масштаб» = «100%») и пустые строки.
bool LooksLikeUrl(const std::wstring& value) {
    const std::wstring lower = Lower(Trim(value));
    if (lower.size() < 4) {
        return false;
    }
    static const wchar_t* const kSchemes[] = {L"http://",  L"https://", L"file://",
                                              L"chrome://", L"edge://",  L"about:",
                                              L"view-source:", L"ftp://", L"mailto:"};
    for (const wchar_t* scheme : kSchemes) {
        if (lower.rfind(scheme, 0) == 0) {
            return true;
        }
    }
    if (lower.find(L' ') != std::wstring::npos) {
        return false;  // адрес не содержит пробелов
    }
    // Chrome часто показывает адрес без схемы: «google.com/search?q=…», «E:/…/file.pdf».
    if (lower.find(L'.') != std::wstring::npos) {
        return true;
    }
    if (lower.size() >= 3 && iswalpha(lower[0]) != 0 && lower[1] == L':' &&
        (lower[2] == L'/' || lower[2] == L'\\')) {
        return true;  // путь на диске
    }
    return false;
}

// Chrome (и родственные) показывают адрес без схемы — для запуска её надо вернуть.
// Локальные файлы («E:/…pdf») и служебные адреса оставляем как есть.
std::wstring NormalizeUrl(const std::wstring& value) {
    const std::wstring text = Trim(value);
    const std::wstring lower = Lower(text);
    if (lower.find(L"://") != std::wstring::npos || lower.rfind(L"about:", 0) == 0 ||
        lower.rfind(L"chrome:", 0) == 0 || lower.rfind(L"edge:", 0) == 0 ||
        lower.rfind(L"view-source:", 0) == 0 || lower.rfind(L"mailto:", 0) == 0) {
        return text;
    }
    if (lower.size() >= 3 && iswalpha(lower[0]) != 0 && lower[1] == L':' &&
        (lower[2] == L'/' || lower[2] == L'\\')) {
        return text;  // локальный файл: Chrome откроет его и так
    }
    return L"https://" + text;
}


std::wstring ValueOf(IUIAutomationElement* element, bool* ok) {
    *ok = false;
    std::wstring out;
    IUIAutomationValuePattern* pattern = nullptr;
    if (element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern)) == S_OK &&
        pattern != nullptr) {
        BSTR value = nullptr;
        if (pattern->get_CurrentValue(&value) == S_OK && value != nullptr) {
            out.assign(value, SysStringLen(value));
            *ok = true;
        }
        if (value != nullptr) {
            SysFreeString(value);
        }
        pattern->Release();
        if (*ok) {
            return out;
        }
    }
    // Значение может приходить только через LegacyIAccessiblePattern: его отдаёт
    // UIA-мост к MSAA, а через него работает доступность Gecko (Firefox).
    IUIAutomationLegacyIAccessiblePattern* legacy = nullptr;
    if (element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy)) ==
            S_OK &&
        legacy != nullptr) {
        BSTR value = nullptr;
        if (legacy->get_CurrentValue(&value) == S_OK && value != nullptr) {
            out.assign(value, SysStringLen(value));
            *ok = true;
        }
        if (value != nullptr) {
            SysFreeString(value);
        }
        legacy->Release();
    }
    return out;
}

std::wstring NameOf(IUIAutomationElement* element) {
    BSTR name = nullptr;
    std::wstring out;
    if (element->get_CurrentName(&name) == S_OK && name != nullptr) {
        out.assign(name, SysStringLen(name));
    }
    if (name != nullptr) {
        SysFreeString(name);
    }
    return out;
}

std::wstring AutomationIdOf(IUIAutomationElement* element) {
    BSTR id = nullptr;
    std::wstring out;
    if (element->get_CurrentAutomationId(&id) == S_OK && id != nullptr) {
        out.assign(id, SysStringLen(id));
    }
    if (id != nullptr) {
        SysFreeString(id);
    }
    return out;
}

// Адресная строка по имени контрола: «Адресная строка и строка поиска» (Chrome/Edge
// на русском), «Address and search bar» (англ.), «Найдите в Google или введите
// адрес» (Firefox), «Панель адреса».
bool IsAddressBarName(const std::wstring& name) {
    const std::wstring lower = Lower(name);
    return lower.find(L"адресн") != std::wstring::npos ||
           lower.find(L"адреса") != std::wstring::npos ||
           lower.find(L"адрес") != std::wstring::npos ||
           lower.find(L"address") != std::wstring::npos ||
           lower.find(L"location") != std::wstring::npos ||
           lower.find(L"url") != std::wstring::npos;
}

// Второй признак, не зависящий от локали и от того, Edit это или ComboBox:
// у Firefox поле адреса — ComboBox с AutomationId «urlbar-input», у Chromium —
// «omnibox»/«addressEditBox». Имя у Firefox («Найдите в Google или введите
// адрес») знает про «адрес», но полагаться только на текст интерфейса нельзя.
bool IsAddressBarId(const std::wstring& automationId) {
    if (automationId.empty()) {
        return false;
    }
    const std::wstring lower = Lower(automationId);
    return lower.find(L"urlbar") != std::wstring::npos ||
           lower.find(L"omnibox") != std::wstring::npos ||
           lower.find(L"address") != std::wstring::npos;
}

}  // namespace

bool IsBrowserExe(const std::wstring& exeName) {
    static const wchar_t* const kBrowserExes[] = {
        L"chrome.exe",  L"msedge.exe", L"firefox.exe", L"opera.exe",  L"brave.exe",
        L"vivaldi.exe", L"yandex.exe", L"iexplore.exe", L"browser.exe",
    };
    for (const wchar_t* exe : kBrowserExes) {
        if (SameText(exeName, exe)) {
            return true;
        }
    }
    return false;
}

std::wstring ReadBrowserUrl(HWND hwnd) {
    if (hwnd == nullptr) {
        return std::wstring();
    }
    IUIAutomation* uia = Uia();
    if (uia == nullptr) {
        return std::wstring();
    }
    IUIAutomationElement* window = nullptr;
    if (FAILED(uia->ElementFromHandle(hwnd, &window)) || window == nullptr) {
        Trace(L"UIA: окно %p не отдалось (адрес страницы не прочитан)",
              static_cast<void*>(hwnd));
        return std::wstring();
    }

    // Адресная строка бывает двух видов: Edit с ValuePattern (Chrome, Edge) и
    // ComboBox, у которого значение лежит на самом ComboBox — так делает Firefox
    // (проверено зондом на Firefox 160: ControlType ComboBox, имя «Найдите в Google
    // или введите адрес», ClassName «urlbar-input textbox-input», AutomationId
    // «urlbar-input»). Поэтому условие — Edit ИЛИ ComboBox.
    VARIANT kind{};
    kind.vt = VT_I4;
    kind.lVal = UIA_EditControlTypeId;
    IUIAutomationCondition* editCondition = nullptr;
    IUIAutomationCondition* comboCondition = nullptr;
    IUIAutomationCondition* condition = nullptr;
    if (uia->CreatePropertyCondition(UIA_ControlTypePropertyId, kind, &editCondition) == S_OK &&
        editCondition != nullptr) {
        kind.lVal = UIA_ComboBoxControlTypeId;
        if (uia->CreatePropertyCondition(UIA_ControlTypePropertyId, kind, &comboCondition) ==
                S_OK &&
            comboCondition != nullptr) {
            if (uia->CreateOrCondition(editCondition, comboCondition, &condition) != S_OK) {
                condition = nullptr;
            }
        }
        if (condition == nullptr) {
            // OR не собрался — ищем хотя бы по Edit (Chrome).
            editCondition->AddRef();
            condition = editCondition;
        }
    }

    std::wstring result;
    std::wstring fallback;
    int fallbackCount = 0;
    IUIAutomationElementArray* fields = nullptr;
    if (condition != nullptr &&
        window->FindAll(TreeScope_Descendants, condition, &fields) == S_OK && fields != nullptr) {
        int length = 0;
        fields->get_Length(&length);
        for (int i = 0; i < length && result.empty(); ++i) {
            IUIAutomationElement* field = nullptr;
            if (fields->GetElement(i, &field) != S_OK || field == nullptr) {
                continue;
            }
            bool hasValue = false;
            const std::wstring value = ValueOf(field, &hasValue);
            if (hasValue && LooksLikeUrl(value)) {
                if (IsAddressBarName(NameOf(field)) || IsAddressBarId(AutomationIdOf(field))) {
                    result = NormalizeUrl(value);
                } else if (fallbackCount == 0) {
                    fallback = NormalizeUrl(value);
                    ++fallbackCount;
                } else {
                    ++fallbackCount;
                }
            }
            field->Release();
        }
        fields->Release();
    }
    if (condition != nullptr) {
        condition->Release();
    }
    if (editCondition != nullptr) {
        editCondition->Release();
    }
    if (comboCondition != nullptr) {
        comboCondition->Release();
    }
    window->Release();

    if (result.empty() && fallbackCount == 1) {
        // Адресная строка не опознана ни по имени, ни по AutomationId (незнакомая
        // локаль), но значение, похожее на адрес, на окне ровно одно — значит это
        // она. Если кандидатов несколько, не угадываем: неверный адрес хуже пустого.
        result = fallback;
    }

    if (result.empty()) {
        Trace(L"UIA: у окна %p адресная строка не найдена", static_cast<void*>(hwnd));
    }
    return result;
}

bool SamePage(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    // Ключ адреса: без завершающих «/», схема и хост в нижнем регистре, путь и query
    // не трогаем — там регистр значим («/Water2/» и «/water2/» на части серверов
    // разные страницы).
    auto key = [](const std::wstring& value) {
        std::wstring text = NormalizeUrl(value);
        while (text.size() > 1 && text.back() == L'/') {
            text.pop_back();
        }
        const size_t schemeEnd = text.find(L"://");
        if (schemeEnd == std::wstring::npos) {
            return text;
        }
        size_t head = text.find(L'/', schemeEnd + 3);
        if (head == std::wstring::npos) {
            head = text.size();
        }
        for (size_t i = 0; i < head; ++i) {
            text[i] = static_cast<wchar_t>(towlower(text[i]));
        }
        return text;
    };
    return key(a) == key(b);
}

}  // namespace sm
