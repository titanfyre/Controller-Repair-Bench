#pragma once
#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <algorithm>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <map>
namespace Ui
{
inline int Logical(HWND window, int pixels)
{
    const auto dpi = GetDpiForWindow(window);
    return MulDiv(pixels, 96, static_cast<int>(dpi ? dpi : 96));
}
inline int Pixel(HWND window, int logical)
{
    const auto dpi = GetDpiForWindow(window);
    return MulDiv(logical, static_cast<int>(dpi ? dpi : 96), 96);
}
struct Canvas
{
    HDC dc;
    int saved;
    Canvas(HDC context, HWND window) : dc(context), saved(SaveDC(context))
    {
        const int dpi = static_cast<int>(GetDpiForWindow(window));
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc, 96, 96, nullptr);
        SetViewportExtEx(dc, dpi ? dpi : 96, dpi ? dpi : 96, nullptr);
    }
    ~Canvas()
    {
        RestoreDC(dc, saved);
    }
};
inline void Move(HWND child, int x, int y, int width, int height, BOOL repaint)
{
    const auto parent = GetParent(child);
    MoveWindow(child, Pixel(parent, x), Pixel(parent, y), Pixel(parent, width), Pixel(parent, height), repaint);
}
inline constexpr COLORREF Background = RGB(13, 20, 33), Card = RGB(23, 34, 51), Text = RGB(235, 243, 255),
                          Muted = RGB(156, 178, 207), Blue = RGB(48, 117, 248), Accent = RGB(67, 217, 190);
inline HFONT Font(int pixels = 18, int weight = FW_NORMAL)
{
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}
inline HFONT ControlFont(HWND window)
{
    struct Cache
    {
        std::map<UINT, HFONT> fonts;
        ~Cache()
        {
            for (const auto &item : fonts)
                DeleteObject(item.second);
        }
    };
    static Cache cache;
    const auto dpi = GetDpiForWindow(window);
    auto found = cache.fonts.find(dpi);
    if (found != cache.fonts.end())
        return found->second;
    const auto font = Font(Pixel(window, 17));
    cache.fonts.emplace(dpi, font);
    return font;
}
inline void Fonts(HWND window)
{
    const auto font = ControlFont(window);
    for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}
inline std::wstring Wide(const std::string &s)
{
    if (s.empty())
        return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n)
        return L"Invalid UTF-8";
    std::wstring r(static_cast<size_t>(n), L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}
inline std::string Utf8(const std::wstring &s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(static_cast<size_t>(n), ' ');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
inline std::wstring Error(DWORD code)
{
    wchar_t *p = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&p), 0, nullptr);
    std::wstring s = p ? p : L"Unknown Windows error";
    if (p)
        LocalFree(p);
    return L"Windows " + std::to_wstring(code) + L": " + s;
}
inline void TextAt(HDC dc, int x, int y, const std::wstring &s, int width = 1100, COLORREF color = Text,
                   int size = 18, int weight = FW_NORMAL)
{
    HFONT f = Font(size, weight);
    auto old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT r{x, y, x + width, y + size * 4};
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
    SelectObject(dc, old);
    DeleteObject(f);
}
inline void Box(HDC dc, RECT r, COLORREF color = Card, int radius = 16)
{
    auto brush = CreateSolidBrush(color);
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
}
inline HWND Control(HWND parent, const wchar_t *type, const wchar_t *text, int id, DWORD style = 0)
{
    const bool combo = wcscmp(type, L"COMBOBOX") == 0;
    if (combo)
        style |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    HWND h =
        CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 1, 1, parent,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    const HFONT f = ControlFont(parent);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    if (combo)
    {
        SendMessageW(h, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Pixel(parent, 28));
        SendMessageW(h, CB_SETITEMHEIGHT, 0, Pixel(parent, 28));
    }
    return h;
}
inline HWND Button(HWND parent, const wchar_t *text, int id)
{
    return Control(parent, L"BUTTON", text, id, BS_OWNERDRAW);
}
inline void DrawButton(const DRAWITEMSTRUCT &d)
{
    if (d.CtlType == ODT_COMBOBOX)
    {
        Box(d.hDC, d.rcItem, (d.itemState & ODS_SELECTED) ? Blue : Card, 0);
        std::wstring text;
        if (d.itemID != static_cast<UINT>(-1))
        {
            const auto n = SendMessageW(d.hwndItem, CB_GETLBTEXTLEN, d.itemID, 0);
            if (n >= 0 && n < 32768)
            {
                text.resize(static_cast<size_t>(n) + 1);
                SendMessageW(d.hwndItem, CB_GETLBTEXT, d.itemID, reinterpret_cast<LPARAM>(text.data()));
                text.resize(static_cast<size_t>(n));
            }
        }
        auto font = ControlFont(d.hwndItem);
        auto old = SelectObject(d.hDC, font);
        SetBkMode(d.hDC, TRANSPARENT);
        SetTextColor(d.hDC, Text);
        auto r = d.rcItem;
        r.left += Pixel(d.hwndItem, 9);
        DrawTextW(d.hDC, text.c_str(), static_cast<int>(text.size()), &r,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(d.hDC, old);
        return;
    }
    const bool pressed = (d.itemState & ODS_SELECTED) != 0;
    Box(d.hDC, d.rcItem, pressed ? RGB(34, 87, 188) : Blue, 12);
    wchar_t text[128]{};
    GetWindowTextW(d.hwndItem, text, 128);
    auto f = Font(Pixel(d.hwndItem, 17), FW_SEMIBOLD);
    auto old = SelectObject(d.hDC, f);
    SetBkMode(d.hDC, TRANSPARENT);
    SetTextColor(d.hDC, Text);
    RECT r = d.rcItem;
    DrawTextW(d.hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d.hDC, old);
    DeleteObject(f);
    if (d.itemState & ODS_FOCUS)
    {
        InflateRect(&r, -4, -4);
        DrawFocusRect(d.hDC, &r);
    }
}
inline std::filesystem::path ExeDirectory()
{
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), n)).parent_path();
}
inline void Setup(HWND owner)
{
    const auto p = ExeDirectory() / L"Dependencies" / L"InterceptorSetup.exe";
    if (!std::filesystem::is_regular_file(p))
    {
        MessageBoxW(owner,
                    L"Place the matching demo InterceptorSetup.exe in Dependencies beside this application, or "
                    L"download it from the driver repository linked in README.md.",
                    L"Driver installer not bundled", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto code = reinterpret_cast<INT_PTR>(
        ShellExecuteW(owner, L"runas", p.c_str(), nullptr, p.parent_path().c_str(), SW_SHOWNORMAL));
    if (code <= 32)
        MessageBoxW(owner, Error(static_cast<DWORD>(code)).c_str(), L"Could not open installer",
                    MB_OK | MB_ICONERROR);
}
inline bool Save(HWND owner, const wchar_t *extension, std::filesystem::path &out)
{
    wchar_t file[MAX_PATH] = L"Controller-report";
    OPENFILENAMEW d{sizeof(d)};
    d.hwndOwner = owner;
    d.lpstrFile = file;
    d.nMaxFile = MAX_PATH;
    d.lpstrFilter = L"Report files\0*.html;*.csv\0All files\0*.*\0";
    d.lpstrDefExt = extension;
    d.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&d))
        return false;
    out = file;
    return true;
}
inline std::wstring GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}
inline bool Elevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    if (token)
        CloseHandle(token);
    return elevated;
}
inline bool Command(const wchar_t *option)
{
    int n = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &n);
    bool found = false;
    for (int i = 1; i < n; ++i)
        if (wcscmp(args[i], option) == 0)
            found = true;
    if (args)
        LocalFree(args);
    return found;
}
inline bool StartElevated(HWND window)
{
    if (Elevated())
        return true;
    auto exe = ExeDirectory();
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768);
    const auto code =
        reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"runas", path, L"--start", exe.c_str(), SW_SHOWNORMAL));
    if (code > 32)
        PostMessageW(window, WM_CLOSE, 0, 0);
    else
        MessageBoxW(window, L"Capture needs administrator rights to open the signed driver. Nothing was started.",
                    L"Capture not started", MB_OK | MB_ICONINFORMATION);
    return false;
}
inline void Hand(HWND child)
{
    SetClassLongPtrW(child, GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(LoadCursorW(nullptr, IDC_HAND)));
}
inline bool Capture(HWND window, const std::filesystem::path &path)
{
    RECT r{};
    GetClientRect(window, &r);
    if (r.right <= 0 || r.bottom <= 0)
        return false;
    auto dc = GetDC(window);
    auto memory = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap)
    {
        DeleteDC(memory);
        ReleaseDC(window, dc);
        return false;
    }
    auto old = SelectObject(memory, bitmap);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        RECT childRect{};
        GetWindowRect(child, &childRect);
        MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT *>(&childRect), 2);
        const auto saved = SaveDC(memory);
        SetViewportOrgEx(memory, childRect.left, childRect.top, nullptr);
        wchar_t className[64]{};
        GetClassNameW(child, className, 64);
        if (_wcsicmp(className, L"ComboBox") == 0)
        {
            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_COMBOBOX;
            item.hwndItem = child;
            item.hDC = memory;
            item.itemID = static_cast<UINT>(SendMessageW(child, CB_GETCURSEL, 0, 0));
            item.rcItem = {0, 0, childRect.right - childRect.left, Pixel(child, 30)};
            DrawButton(item);
        }
        else
            SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
        RestoreDC(memory, saved);
    }
    GdiFlush();
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(r.right * r.bottom * 4);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(&header), sizeof(header));
    file.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.write(static_cast<const char *>(bits), static_cast<std::streamsize>(r.right) * r.bottom * 4);
    bool ok = static_cast<bool>(file);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, dc);
    return ok;
}
} // namespace Ui
