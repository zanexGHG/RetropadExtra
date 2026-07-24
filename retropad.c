// retropad - a Petzold-style Win32 notepad clone implemented in mostly plain C.
// Keeps the classic menus/accelerators, word wrap, status bar, find/replace,
// font picker, and basic file load/save with BOM detection.
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdlib.h>
#include "resource.h"
#include "file_io.h"

#define APP_TITLE      L"retropad"
#define UNTITLED_NAME  L"Untitled"
#define MAX_PATH_BUFFER 1024
#define DEFAULT_WIDTH  640
#define DEFAULT_HEIGHT 480

typedef struct Theme {
    COLORREF fg;
    COLORREF bg;
    COLORREF statusBg;
    COLORREF statusText;
} Theme;

#define THEME_CUSTOM 2

static const Theme g_themes[] = {
    { RGB(0, 0, 0),       RGB(255, 255, 255), RGB(240, 240, 240), RGB(0, 0, 0)       },
    { RGB(220, 220, 220), RGB(30, 30, 30),    RGB(45, 45, 48),    RGB(220, 220, 220) },
};

typedef struct AppState {
    HWND hwndMain;
    HWND hwndEdit;
    HWND hwndStatus;
    HFONT hFont;
    int theme;
    Theme colors;
    Theme custom;
    HBRUSH hThemeBrush;
    WCHAR statusText[128];
    WCHAR currentPath[MAX_PATH_BUFFER];
    BOOL wordWrap;
    BOOL statusVisible;
    BOOL statusBeforeWrap;
    BOOL modified;
    TextEncoding encoding;
    FINDREPLACEW find;
    HWND hFindDlg;
    HWND hReplaceDlg;
    UINT findFlags;
    WCHAR findText[128];
    WCHAR replaceText[128];
} AppState;

static AppState g_app = {0};
static HINSTANCE g_hInst = NULL;
static UINT g_findMsg = 0;

typedef enum { APPMODE_DEFAULT, APPMODE_ALLOWDARK, APPMODE_FORCEDARK, APPMODE_FORCELIGHT, APPMODE_MAX } PreferredAppMode;
typedef PreferredAppMode (WINAPI *SetPreferredAppModeFn)(PreferredAppMode);
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
typedef void (WINAPI *FlushMenuThemesFn)(void);
static AllowDarkModeForWindowFn g_allowDarkModeForWindow = NULL;
static FlushMenuThemesFn g_flushMenuThemes = NULL;

static void InitDarkMode(void) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    SetPreferredAppModeFn setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_allowDarkModeForWindow = (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    g_flushMenuThemes = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(APPMODE_ALLOWDARK);
}

static BOOL IsDarkColor(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000 < 128;
}

static COLORREF Lighten(COLORREF c, int amt) {
    return RGB(min(255, GetRValue(c) + amt), min(255, GetGValue(c) + amt), min(255, GetBValue(c) + amt));
}

// Undocumented window messages feature used by Windows to draw themed menu bars and individual menu-bar items. (Thanks to a random stackoverflow post over 12 years ago or smth xD)
#define WM_UAHDRAWMENU     0x0091
#define WM_UAHDRAWMENUITEM 0x0092

typedef union { DWORD rgsizeBar[4]; DWORD rgsizePopup[8]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

static RECT MenuBarRect(HWND hwnd) {
    MENUBARINFO mbi = { sizeof(mbi) };
    GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
    RECT rcWin;
    GetWindowRect(hwnd, &rcWin);
    RECT rc = mbi.rcBar;
    OffsetRect(&rc, -rcWin.left, -rcWin.top);
    return rc;
}

// Hide the light resize grip in dark mode (cuz it triggers tf out of me to have a little light quare on the bottom right)
static LRESULT CALLBACK StatusSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    LRESULT r = DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_PAINT && IsDarkColor(g_app.colors.bg)) {
        RECT rc;
        GetClientRect(h, &rc);
        rc.left = rc.right - GetSystemMetrics(SM_CXVSCROLL) - 4;
        HDC hdc = GetDC(h);
        HBRUSH b = CreateSolidBrush(g_app.colors.statusBg);
        FillRect(hdc, &rc, b);
        DeleteObject(b);
        ReleaseDC(h, hdc);
    }
    return r;
}

static void UpdateTitle(HWND hwnd);
static void CreateEditControl(HWND hwnd);
static void UpdateLayout(HWND hwnd);
static BOOL PromptSaveChanges(HWND hwnd);
static BOOL DoFileOpen(HWND hwnd);
static BOOL DoFileSave(HWND hwnd, BOOL saveAs);
static void DoFileNew(HWND hwnd);
static void SetWordWrap(HWND hwnd, BOOL enabled);
static void ToggleStatusBar(HWND hwnd, BOOL visible);
static void UpdateStatusBar(HWND hwnd);
static void ShowFindDialog(HWND hwnd);
static void ShowReplaceDialog(HWND hwnd);
static BOOL DoFindNext(BOOL reverse);
static void DoSelectFont(HWND hwnd);
static void InsertTimeDate(HWND hwnd);
static void SetTheme(int theme);
static void ApplyColors(void);
static void ApplyWindowTheme(void);
static BOOL PickColor(HWND owner, COLORREF *color);
static INT_PTR CALLBACK CustomizeDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void HandleFindReplace(LPFINDREPLACE lpfr);
static void LoadSettings(void);
static void SaveSettings(void);
static BOOL LoadDocumentFromPath(HWND hwnd, LPCWSTR path);
static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam);

static BOOL GetEditText(HWND hwndEdit, WCHAR **bufferOut, int *lengthOut) {
    int length = GetWindowTextLengthW(hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 1) * sizeof(WCHAR));
    if (!buffer) return FALSE;
    GetWindowTextW(hwndEdit, buffer, length + 1);
    if (lengthOut) *lengthOut = length;
    *bufferOut = buffer;
    return TRUE;
}

static BOOL FindInEdit(HWND hwndEdit, const WCHAR *needle, BOOL matchCase, BOOL searchDown, DWORD startPos, DWORD *outStart, DWORD *outEnd) {
    if (!needle || needle[0] == L'\0') return FALSE;

    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(hwndEdit, &text, &len)) return FALSE;

    size_t needleLen = wcslen(needle);
    WCHAR *haystack = text;
    WCHAR *needleBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
    if (!needleBuf) {
        HeapFree(GetProcessHeap(), 0, text);
        return FALSE;
    }
    StringCchCopyW(needleBuf, needleLen + 1, needle);

    if (!matchCase) {
        CharLowerBuffW(haystack, len);
        CharLowerBuffW(needleBuf, (DWORD)needleLen);
    }

    if (startPos > (DWORD)len) startPos = (DWORD)len;

    WCHAR *found = NULL;
    if (searchDown) {
        found = wcsstr(haystack + startPos, needleBuf);
        if (!found && startPos > 0) {
            found = wcsstr(haystack, needleBuf);
        }
    } else {
        WCHAR *p = haystack;
        while ((p = wcsstr(p, needleBuf)) != NULL) {
            DWORD idx = (DWORD)(p - haystack);
            if (idx < startPos) {
                found = p;
                p++;
            } else {
                break;
            }
        }
        if (!found && startPos < (DWORD)len) {
            p = haystack + startPos;
            while ((p = wcsstr(p, needleBuf)) != NULL) {
                found = p;
                p++;
            }
        }
    }

    BOOL result = FALSE;
    if (found) {
        DWORD pos = (DWORD)(found - haystack);
        *outStart = pos;
        *outEnd = pos + (DWORD)needleLen;
        result = TRUE;
    }

    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    return result;
}

static int ReplaceAllOccurrences(HWND hwndEdit, const WCHAR *needle, const WCHAR *replacement, BOOL matchCase) {
    if (!needle || needle[0] == L'\0') return 0;

    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(hwndEdit, &text, &len)) return 0;

    size_t needleLen = wcslen(needle);
    size_t replLen = replacement ? wcslen(replacement) : 0;

    WCHAR *searchBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    WCHAR *needleBuf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (needleLen + 1) * sizeof(WCHAR));
    if (!searchBuf || !needleBuf) {
        HeapFree(GetProcessHeap(), 0, text);
        if (searchBuf) HeapFree(GetProcessHeap(), 0, searchBuf);
        if (needleBuf) HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }
    StringCchCopyW(searchBuf, len + 1, text);
    StringCchCopyW(needleBuf, needleLen + 1, needle);

    if (!matchCase) {
        CharLowerBuffW(searchBuf, len);
        CharLowerBuffW(needleBuf, (DWORD)needleLen);
    }

    int count = 0;
    WCHAR *p = searchBuf;
    while ((p = wcsstr(p, needleBuf)) != NULL) {
        count++;
        p += needleLen;
    }
    if (count == 0) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }

    size_t newLen = (size_t)len - (size_t)count * needleLen + (size_t)count * replLen;
    WCHAR *result = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (newLen + 1) * sizeof(WCHAR));
    if (!result) {
        HeapFree(GetProcessHeap(), 0, text);
        HeapFree(GetProcessHeap(), 0, searchBuf);
        HeapFree(GetProcessHeap(), 0, needleBuf);
        return 0;
    }

    WCHAR *dst = result;
    WCHAR *searchCur = searchBuf;
    WCHAR *origCur = text;
    while ((p = wcsstr(searchCur, needleBuf)) != NULL) {
        size_t delta = (size_t)(p - searchCur);
        CopyMemory(dst, origCur, delta * sizeof(WCHAR));
        dst += delta;
        origCur += delta;
        searchCur += delta;

        if (replLen) {
            CopyMemory(dst, replacement, replLen * sizeof(WCHAR));
            dst += replLen;
        }
        origCur += needleLen;
        searchCur += needleLen;
    }
    size_t tail = wcslen(origCur);
    CopyMemory(dst, origCur, tail * sizeof(WCHAR));
    dst += tail;
    *dst = L'\0';

    SetWindowTextW(hwndEdit, result);
    HeapFree(GetProcessHeap(), 0, text);
    HeapFree(GetProcessHeap(), 0, searchBuf);
    HeapFree(GetProcessHeap(), 0, needleBuf);
    HeapFree(GetProcessHeap(), 0, result);
    SendMessageW(hwndEdit, EM_SETMODIFY, TRUE, 0);
    g_app.modified = TRUE;
    UpdateTitle(g_app.hwndMain);
    return count;
}

static void UpdateTitle(HWND hwnd) {
    WCHAR name[MAX_PATH_BUFFER];
    if (g_app.currentPath[0]) {
        WCHAR *fileName = wcsrchr(g_app.currentPath, L'\\');
        fileName = fileName ? fileName + 1 : g_app.currentPath;
        StringCchCopyW(name, MAX_PATH_BUFFER, fileName);
    } else {
        StringCchCopyW(name, MAX_PATH_BUFFER, UNTITLED_NAME);
    }

    WCHAR title[MAX_PATH_BUFFER + 32];
    StringCchPrintfW(title, ARRAYSIZE(title), L"%s%s - %s", (g_app.modified ? L"*" : L""), name, APP_TITLE);
    SetWindowTextW(hwnd, title);
}

static void ApplyFontToEdit(HWND hwndEdit, HFONT font) {
    SendMessageW(hwndEdit, WM_SETFONT, (WPARAM)font, TRUE);
}

static void CreateEditControl(HWND hwnd) {
    if (g_app.hwndEdit) {
        DestroyWindow(g_app.hwndEdit);
    }

    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    if (!g_app.wordWrap) {
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    }

    g_app.hwndEdit = CreateWindowExW(0, L"EDIT", NULL, style, 0, 0, 0, 0, hwnd, (HMENU)1, g_hInst, NULL);
    if (g_app.hwndEdit && g_app.hFont) {
        ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
    }
    SendMessageW(g_app.hwndEdit, EM_SETLIMITTEXT, 0, 0); // allow large files
    if (g_app.hThemeBrush) ApplyWindowTheme();
    UpdateLayout(hwnd);
}

static void ToggleStatusBar(HWND hwnd, BOOL visible) {
    g_app.statusVisible = visible;
    if (visible) {
        if (!g_app.hwndStatus) {
            g_app.hwndStatus = CreateStatusWindowW(WS_CHILD, L"", hwnd, 2);
            SetWindowSubclass(g_app.hwndStatus, StatusSubclass, 1, 0);
        }
        ShowWindow(g_app.hwndStatus, SW_SHOW);
    } else if (g_app.hwndStatus) {
        ShowWindow(g_app.hwndStatus, SW_HIDE);
    }
    UpdateLayout(hwnd);
    UpdateStatusBar(hwnd);
}

static void UpdateLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int statusHeight = 0;
    if (g_app.statusVisible && g_app.hwndStatus) {
        SendMessageW(g_app.hwndStatus, WM_SIZE, 0, 0);
        RECT sbrc;
        GetWindowRect(g_app.hwndStatus, &sbrc);
        statusHeight = sbrc.bottom - sbrc.top;
        MoveWindow(g_app.hwndStatus, 0, rc.bottom - statusHeight, rc.right, statusHeight, TRUE);
    }

    if (g_app.hwndEdit) {
        MoveWindow(g_app.hwndEdit, 0, 0, rc.right, rc.bottom - statusHeight, TRUE);
    }
}

static BOOL PromptSaveChanges(HWND hwnd) {
    if (!g_app.modified) return TRUE;

    WCHAR prompt[MAX_PATH_BUFFER + 64];
    const WCHAR *name = g_app.currentPath[0] ? g_app.currentPath : UNTITLED_NAME;
    StringCchPrintfW(prompt, ARRAYSIZE(prompt), L"Do you want to save changes to %s?", name);
    int res = MessageBoxW(hwnd, prompt, APP_TITLE, MB_ICONQUESTION | MB_YESNOCANCEL);
    if (res == IDYES) {
        return DoFileSave(hwnd, FALSE);
    }
    return res == IDNO;
}

static BOOL LoadDocumentFromPath(HWND hwnd, LPCWSTR path) {
    WCHAR *text = NULL;
    TextEncoding enc = ENC_UTF8;
    if (!LoadTextFile(hwnd, path, &text, NULL, &enc)) {
        return FALSE;
    }

    SetWindowTextW(g_app.hwndEdit, text);
    HeapFree(GetProcessHeap(), 0, text);
    StringCchCopyW(g_app.currentPath, ARRAYSIZE(g_app.currentPath), path);
    g_app.encoding = enc;
    SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
    g_app.modified = FALSE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
    return TRUE;
}

static BOOL DoFileOpen(HWND hwnd) {
    if (!PromptSaveChanges(hwnd)) return FALSE;

    WCHAR path[MAX_PATH_BUFFER] = L"";
    if (!OpenFileDialog(hwnd, path, ARRAYSIZE(path))) {
        return FALSE;
    }
    return LoadDocumentFromPath(hwnd, path);
}

static BOOL DoFileSave(HWND hwnd, BOOL saveAs) {
    WCHAR path[MAX_PATH_BUFFER];
    if (saveAs || g_app.currentPath[0] == L'\0') {
        path[0] = L'\0';
        if (g_app.currentPath[0]) {
            StringCchCopyW(path, ARRAYSIZE(path), g_app.currentPath);
        }
        if (!SaveFileDialog(hwnd, path, ARRAYSIZE(path))) {
            return FALSE;
        }
        StringCchCopyW(g_app.currentPath, ARRAYSIZE(g_app.currentPath), path);
    } else {
        StringCchCopyW(path, ARRAYSIZE(path), g_app.currentPath);
    }

    int len = GetWindowTextLengthW(g_app.hwndEdit);
    WCHAR *buffer = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
    if (!buffer) return FALSE;
    GetWindowTextW(g_app.hwndEdit, buffer, len + 1);

    BOOL ok = SaveTextFile(hwnd, path, buffer, len, g_app.encoding);
    HeapFree(GetProcessHeap(), 0, buffer);
    if (ok) {
        SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
        g_app.modified = FALSE;
        UpdateTitle(hwnd);
    }
    return ok;
}

static void DoFileNew(HWND hwnd) {
    if (!PromptSaveChanges(hwnd)) return;
    SetWindowTextW(g_app.hwndEdit, L"");
    g_app.currentPath[0] = L'\0';
    g_app.encoding = ENC_UTF8;
    SendMessageW(g_app.hwndEdit, EM_SETMODIFY, FALSE, 0);
    g_app.modified = FALSE;
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
}

static void SetWordWrap(HWND hwnd, BOOL enabled) {
    if (g_app.wordWrap == enabled) return;
    g_app.wordWrap = enabled;
    HWND edit = g_app.hwndEdit;
    WCHAR *text = NULL;
    int len = 0;
    if (!GetEditText(edit, &text, &len)) {
        return;
    }
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);

    CreateEditControl(hwnd);
    SetWindowTextW(g_app.hwndEdit, text);
    SendMessageW(g_app.hwndEdit, EM_SETSEL, start, end);
    HeapFree(GetProcessHeap(), 0, text);

    if (enabled) {
        g_app.statusBeforeWrap = g_app.statusVisible;
        ToggleStatusBar(hwnd, FALSE);
        EnableMenuItem(GetMenu(hwnd), IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_GRAYED);
    } else {
        ToggleStatusBar(hwnd, g_app.statusBeforeWrap);
        EnableMenuItem(GetMenu(hwnd), IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
        EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_ENABLED);
    }
    UpdateTitle(hwnd);
    UpdateStatusBar(hwnd);
}

static void UpdateStatusBar(HWND hwnd) {
    if (!g_app.statusVisible || !g_app.hwndStatus) return;
    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    int line = (int)SendMessageW(g_app.hwndEdit, EM_LINEFROMCHAR, selStart, 0) + 1;
    int col = (int)(selStart - SendMessageW(g_app.hwndEdit, EM_LINEINDEX, line - 1, 0)) + 1;
    int lines = (int)SendMessageW(g_app.hwndEdit, EM_GETLINECOUNT, 0, 0);

    StringCchPrintfW(g_app.statusText, ARRAYSIZE(g_app.statusText), L"Ln %d, Col %d    Lines: %d", line, col, lines);
    SendMessageW(g_app.hwndStatus, SB_SETTEXT, SBT_OWNERDRAW, (LPARAM)g_app.statusText);
}

static void ShowFindDialog(HWND hwnd) {
    if (g_app.hFindDlg) {
        SetForegroundWindow(g_app.hFindDlg);
        return;
    }

    ZeroMemory(&g_app.find, sizeof(g_app.find));
    g_app.find.lStructSize = sizeof(FINDREPLACEW);
    g_app.find.hwndOwner = hwnd;
    g_app.find.lpstrFindWhat = g_app.findText;
    g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
    g_app.find.Flags = g_app.findFlags;

    g_app.hFindDlg = FindTextW(&g_app.find);
}

static void ShowReplaceDialog(HWND hwnd) {
    if (g_app.hReplaceDlg) {
        SetForegroundWindow(g_app.hReplaceDlg);
        return;
    }

    ZeroMemory(&g_app.find, sizeof(g_app.find));
    g_app.find.lStructSize = sizeof(FINDREPLACEW);
    g_app.find.hwndOwner = hwnd;
    g_app.find.lpstrFindWhat = g_app.findText;
    g_app.find.lpstrReplaceWith = g_app.replaceText;
    g_app.find.wFindWhatLen = ARRAYSIZE(g_app.findText);
    g_app.find.wReplaceWithLen = ARRAYSIZE(g_app.replaceText);
    g_app.find.Flags = g_app.findFlags;

    g_app.hReplaceDlg = ReplaceTextW(&g_app.find);
}

static BOOL DoFindNext(BOOL reverse) {
    if (g_app.findText[0] == L'\0') {
        ShowFindDialog(g_app.hwndMain);
        return FALSE;
    }

    DWORD start = 0, end = 0;
    SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    BOOL matchCase = (g_app.findFlags & FR_MATCHCASE) != 0;
    BOOL down = (g_app.findFlags & FR_DOWN) != 0;
    if (reverse) down = !down;
    DWORD searchStart = down ? end : start;
    DWORD outStart = 0, outEnd = 0;
    if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, searchStart, &outStart, &outEnd)) {
        SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
        SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
        return TRUE;
    }
    MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
    return FALSE;
}

static INT_PTR CALLBACK GoToDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemInt(dlg, IDC_GOTO_EDIT, 1, FALSE);
        HWND edit = GetDlgItem(dlg, IDC_GOTO_EDIT);
        SendMessageW(edit, EM_SETLIMITTEXT, 10, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            BOOL ok = FALSE;
            UINT line = GetDlgItemInt(dlg, IDC_GOTO_EDIT, &ok, FALSE);
            if (!ok || line == 0) {
                MessageBoxW(dlg, L"Enter a valid line number.", APP_TITLE, MB_ICONWARNING);
                return TRUE;
            }
            int maxLine = (int)SendMessageW(g_app.hwndEdit, EM_GETLINECOUNT, 0, 0);
            if ((int)line > maxLine) line = (UINT)maxLine;
            int charIndex = (int)SendMessageW(g_app.hwndEdit, EM_LINEINDEX, line - 1, 0);
            if (charIndex >= 0) {
                SendMessageW(g_app.hwndEdit, EM_SETSEL, charIndex, charIndex);
                SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void DoSelectFont(HWND hwnd) {
    LOGFONTW lf = {0};
    if (g_app.hFont) {
        GetObjectW(g_app.hFont, sizeof(LOGFONTW), &lf);
    } else {
        SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(LOGFONTW), &lf, 0);
    }

    CHOOSEFONTW cf = {0};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

    if (ChooseFontW(&cf)) {
        HFONT newFont = CreateFontIndirectW(&lf);
        if (newFont) {
            if (g_app.hFont) DeleteObject(g_app.hFont);
            g_app.hFont = newFont;
            ApplyFontToEdit(g_app.hwndEdit, g_app.hFont);
            UpdateLayout(hwnd);
        }
    }
}

static void InsertTimeDate(HWND hwnd) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR date[64], time[64], stamp[128];
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, ARRAYSIZE(date));
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, time, ARRAYSIZE(time));
    StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%s %s", time, date);
    SendMessageW(g_app.hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)stamp);
}

static void ApplyWindowTheme(void) {
    BOOL dark = IsDarkColor(g_app.colors.bg);
    const WCHAR *sub = dark ? L"DarkMode_Explorer" : L"Explorer";
    if (g_allowDarkModeForWindow) {
        g_allowDarkModeForWindow(g_app.hwndMain, dark);
        if (g_app.hwndEdit) g_allowDarkModeForWindow(g_app.hwndEdit, dark);
        if (g_app.hwndStatus) g_allowDarkModeForWindow(g_app.hwndStatus, dark);
    }
    DwmSetWindowAttribute(g_app.hwndMain, 20, &dark, sizeof(dark));
    if (g_app.hwndEdit) SetWindowTheme(g_app.hwndEdit, sub, NULL);
    if (g_app.hwndStatus) SetWindowTheme(g_app.hwndStatus, sub, NULL);
    if (g_flushMenuThemes) g_flushMenuThemes();
    DrawMenuBar(g_app.hwndMain);
}

static void ApplyColors(void) {
    if (g_app.hThemeBrush) DeleteObject(g_app.hThemeBrush);
    g_app.hThemeBrush = CreateSolidBrush(g_app.colors.bg);
    if (g_app.hwndStatus) {
        SendMessageW(g_app.hwndStatus, SB_SETBKCOLOR, 0, g_app.colors.statusBg);
        InvalidateRect(g_app.hwndStatus, NULL, TRUE);
    }
    ApplyWindowTheme();
    InvalidateRect(g_app.hwndEdit, NULL, TRUE);
}

static void SetTheme(int theme) {
    g_app.theme = theme;
    g_app.colors = (theme == THEME_CUSTOM) ? g_app.custom : g_themes[theme];
    ApplyColors();
}

static BOOL PickColor(HWND owner, COLORREF *color) {
    static COLORREF custom[16] = {0};
    CHOOSECOLORW cc = {0};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = *color;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return FALSE;
    *color = cc.rgbResult;
    return TRUE;
}

static COLORREF *SwatchColor(Theme *t, int ctrlId) {
    switch (ctrlId) {
    case IDC_CUST_TEXT:       return &t->fg;
    case IDC_CUST_BG:         return &t->bg;
    case IDC_CUST_STATUSBG:   return &t->statusBg;
    case IDC_CUST_STATUSTEXT: return &t->statusText;
    }
    return NULL;
}

static INT_PTR CALLBACK CustomizeDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static Theme t;
    switch (msg) {
    case WM_INITDIALOG:
        t = g_app.colors;
        return TRUE;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        COLORREF *c = SwatchColor(&t, dis->CtlID);
        if (!c) return FALSE;
        HBRUSH b = CreateSolidBrush(*c);
        FillRect(dis->hDC, &dis->rcItem, b);
        DeleteObject(b);
        FrameRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            g_app.custom = t;
            SetTheme(THEME_CUSTOM);
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default: {
            COLORREF *c = SwatchColor(&t, LOWORD(wParam));
            if (c && PickColor(dlg, c)) {
                InvalidateRect((HWND)lParam, NULL, TRUE);
            }
            return TRUE;
        }
        }
    }
    return FALSE;
}

static void HandleFindReplace(LPFINDREPLACE lpfr) {
    if (lpfr->Flags & FR_DIALOGTERM) {
        g_app.hFindDlg = NULL;
        g_app.hReplaceDlg = NULL;
        return;
    }

    g_app.findFlags = lpfr->Flags;
    if (lpfr->lpstrFindWhat && lpfr->lpstrFindWhat[0]) {
        StringCchCopyW(g_app.findText, ARRAYSIZE(g_app.findText), lpfr->lpstrFindWhat);
    }
    if (lpfr->lpstrReplaceWith) {
        StringCchCopyW(g_app.replaceText, ARRAYSIZE(g_app.replaceText), lpfr->lpstrReplaceWith);
    }

    BOOL matchCase = (lpfr->Flags & FR_MATCHCASE) != 0;
    BOOL down = (lpfr->Flags & FR_DOWN) != 0;

    if (lpfr->Flags & FR_FINDNEXT) {
        DWORD start = 0, end = 0;
        SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DWORD searchStart = down ? end : start;
        DWORD outStart = 0, outEnd = 0;
        if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, searchStart, &outStart, &outEnd)) {
            SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
            SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
        } else {
            MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
        }
    } else if (lpfr->Flags & FR_REPLACE) {
        DWORD start = 0, end = 0;
        SendMessageW(g_app.hwndEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DWORD outStart = 0, outEnd = 0;
        if (FindInEdit(g_app.hwndEdit, g_app.findText, matchCase, down, start, &outStart, &outEnd)) {
            SendMessageW(g_app.hwndEdit, EM_SETSEL, outStart, outEnd);
            SendMessageW(g_app.hwndEdit, EM_REPLACESEL, TRUE, (LPARAM)g_app.replaceText);
            SendMessageW(g_app.hwndEdit, EM_SCROLLCARET, 0, 0);
            g_app.modified = TRUE;
            UpdateTitle(g_app.hwndMain);
        } else {
            MessageBoxW(g_app.hwndMain, L"Cannot find the text.", APP_TITLE, MB_ICONINFORMATION);
        }
    } else if (lpfr->Flags & FR_REPLACEALL) {
        int replaced = ReplaceAllOccurrences(g_app.hwndEdit, g_app.findText, g_app.replaceText, matchCase);
        WCHAR msg[64];
        StringCchPrintfW(msg, ARRAYSIZE(msg), L"Replaced %d occurrence%s.", replaced, replaced == 1 ? L"" : L"s");
        MessageBoxW(g_app.hwndMain, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
    }
}

static void UpdateMenuStates(HWND hwnd) {
    HMENU menu = GetMenu(hwnd);
    if (!menu) return;

    UINT wrapState = g_app.wordWrap ? MF_CHECKED : MF_UNCHECKED;
    UINT statusState = g_app.statusVisible ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(menu, IDM_FORMAT_WORD_WRAP, MF_BYCOMMAND | wrapState);
    CheckMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | statusState);

    BOOL canGoTo = !g_app.wordWrap;
    EnableMenuItem(menu, IDM_EDIT_GOTO, MF_BYCOMMAND | (canGoTo ? MF_ENABLED : MF_GRAYED));
    if (g_app.wordWrap) {
        EnableMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
    } else {
        EnableMenuItem(menu, IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_ENABLED);
    }

    CheckMenuRadioItem(menu, IDM_THEME_LIGHT, IDM_THEME_CUSTOM, IDM_THEME_LIGHT + g_app.theme, MF_BYCOMMAND);

    BOOL modified = (SendMessageW(g_app.hwndEdit, EM_GETMODIFY, 0, 0) != 0);
    EnableMenuItem(menu, IDM_FILE_SAVE, MF_BYCOMMAND | (modified ? MF_ENABLED : MF_GRAYED));
}

static void HandleCommand(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    switch (LOWORD(wParam)) {
    case IDM_FILE_NEW:
        DoFileNew(hwnd);
        break;
    case IDM_FILE_OPEN:
        DoFileOpen(hwnd);
        break;
    case IDM_FILE_SAVE:
        DoFileSave(hwnd, FALSE);
        break;
    case IDM_FILE_SAVE_AS:
        DoFileSave(hwnd, TRUE);
        break;
    case IDM_FILE_PAGE_SETUP:
    case IDM_FILE_PRINT:
        MessageBoxW(hwnd, L"Printing is not implemented in retropad.", APP_TITLE, MB_ICONINFORMATION);
        break;
    case IDM_FILE_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        break;

    case IDM_EDIT_UNDO:
        SendMessageW(g_app.hwndEdit, EM_UNDO, 0, 0);
        break;
    case IDM_EDIT_CUT:
        SendMessageW(g_app.hwndEdit, WM_CUT, 0, 0);
        break;
    case IDM_EDIT_COPY:
        SendMessageW(g_app.hwndEdit, WM_COPY, 0, 0);
        break;
    case IDM_EDIT_PASTE:
        SendMessageW(g_app.hwndEdit, WM_PASTE, 0, 0);
        break;
    case IDM_EDIT_DELETE:
        SendMessageW(g_app.hwndEdit, WM_CLEAR, 0, 0);
        break;
    case IDM_EDIT_FIND:
        ShowFindDialog(hwnd);
        break;
    case IDM_EDIT_FIND_NEXT:
        DoFindNext(FALSE);
        break;
    case IDM_EDIT_REPLACE:
        ShowReplaceDialog(hwnd);
        break;
    case IDM_EDIT_GOTO:
        if (g_app.wordWrap) {
            MessageBoxW(hwnd, L"Go To is unavailable when Word Wrap is on.", APP_TITLE, MB_ICONINFORMATION);
        } else {
            DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_GOTO), hwnd, GoToDlgProc);
        }
        break;
    case IDM_EDIT_SELECT_ALL:
        SendMessageW(g_app.hwndEdit, EM_SETSEL, 0, -1);
        break;
    case IDM_EDIT_TIME_DATE:
        InsertTimeDate(hwnd);
        break;

    case IDM_FORMAT_WORD_WRAP:
        SetWordWrap(hwnd, !g_app.wordWrap);
        break;
    case IDM_FORMAT_FONT:
        DoSelectFont(hwnd);
        break;

    case IDM_VIEW_STATUS_BAR:
        ToggleStatusBar(hwnd, !g_app.statusVisible);
        break;

    case IDM_THEME_LIGHT:
    case IDM_THEME_DARK:
        SetTheme(LOWORD(wParam) - IDM_THEME_LIGHT);
        break;
    case IDM_THEME_CUSTOM:
        DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_CUSTOMIZE), hwnd, CustomizeDlgProc);
        break;

    case IDM_HELP_VIEW_HELP:
        MessageBoxW(hwnd, L"No help file is available for retropad.", APP_TITLE, MB_ICONINFORMATION);
        break;
    case IDM_HELP_ABOUT:
        DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_ABOUT), hwnd, AboutDlgProc);
        break;
    }
}

static INT_PTR CALLBACK AboutDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(dlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void GetIniPath(WCHAR *out, size_t cch) {
    GetModuleFileNameW(NULL, out, (DWORD)cch);
    WCHAR *slash = wcsrchr(out, L'\\');
    if (slash) StringCchCopyW(slash + 1, cch - (size_t)(slash + 1 - out), L"retropad.ini");
}

static int IniGetInt(LPCWSTR path, LPCWSTR key, int def) {
    WCHAR buf[64], defs[32];
    StringCchPrintfW(defs, ARRAYSIZE(defs), L"%d", def);
    GetPrivateProfileStringW(L"retropad", key, defs, buf, ARRAYSIZE(buf), path);
    return _wtoi(buf);
}

static void IniSetInt(LPCWSTR path, LPCWSTR key, int val) {
    WCHAR buf[32];
    StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", val);
    WritePrivateProfileStringW(L"retropad", key, buf, path);
}

static void LoadSettings(void) {
    WCHAR path[MAX_PATH_BUFFER];
    GetIniPath(path, ARRAYSIZE(path));

    g_app.theme = IniGetInt(path, L"Theme", 0);
    if (g_app.theme < 0 || g_app.theme > THEME_CUSTOM) g_app.theme = 0;
    g_app.custom.fg = (COLORREF)IniGetInt(path, L"CustomFg", (int)g_themes[0].fg);
    g_app.custom.bg = (COLORREF)IniGetInt(path, L"CustomBg", (int)g_themes[0].bg);
    g_app.custom.statusBg = (COLORREF)IniGetInt(path, L"CustomStatusBg", (int)g_themes[0].statusBg);
    g_app.custom.statusText = (COLORREF)IniGetInt(path, L"CustomStatusText", (int)g_themes[0].statusText);
    g_app.wordWrap = IniGetInt(path, L"WordWrap", 0) != 0;
    g_app.statusVisible = IniGetInt(path, L"StatusBar", 1) != 0;

    WCHAR face[LF_FACESIZE];
    GetPrivateProfileStringW(L"retropad", L"FontFace", L"", face, ARRAYSIZE(face), path);
    if (face[0]) {
        LOGFONTW lf = {0};
        lf.lfHeight = IniGetInt(path, L"FontHeight", -12);
        lf.lfWeight = IniGetInt(path, L"FontWeight", FW_NORMAL);
        lf.lfItalic = (BYTE)IniGetInt(path, L"FontItalic", 0);
        lf.lfCharSet = DEFAULT_CHARSET;
        StringCchCopyW(lf.lfFaceName, LF_FACESIZE, face);
        g_app.hFont = CreateFontIndirectW(&lf);
    }
}

static void SaveSettings(void) {
    WCHAR path[MAX_PATH_BUFFER];
    GetIniPath(path, ARRAYSIZE(path));

    IniSetInt(path, L"Theme", g_app.theme);
    IniSetInt(path, L"CustomFg", (int)g_app.custom.fg);
    IniSetInt(path, L"CustomBg", (int)g_app.custom.bg);
    IniSetInt(path, L"CustomStatusBg", (int)g_app.custom.statusBg);
    IniSetInt(path, L"CustomStatusText", (int)g_app.custom.statusText);
    IniSetInt(path, L"WordWrap", g_app.wordWrap);
    IniSetInt(path, L"StatusBar", g_app.statusVisible);

    if (g_app.hFont) {
        LOGFONTW lf = {0};
        GetObjectW(g_app.hFont, sizeof(lf), &lf);
        WritePrivateProfileStringW(L"retropad", L"FontFace", lf.lfFaceName, path);
        IniSetInt(path, L"FontHeight", lf.lfHeight);
        IniSetInt(path, L"FontWeight", lf.lfWeight);
        IniSetInt(path, L"FontItalic", lf.lfItalic);
    }
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_findMsg) {
        HandleFindReplace((LPFINDREPLACE)lParam);
        return 0;
    }

    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        CreateEditControl(hwnd);
        ToggleStatusBar(hwnd, g_app.statusVisible);
        SetTheme(g_app.theme);
        if (g_app.wordWrap) {
            EnableMenuItem(GetMenu(hwnd), IDM_VIEW_STATUS_BAR, MF_BYCOMMAND | MF_GRAYED);
            EnableMenuItem(GetMenu(hwnd), IDM_EDIT_GOTO, MF_BYCOMMAND | MF_GRAYED);
        }
        UpdateTitle(hwnd);
        UpdateStatusBar(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_SETFOCUS:
        if (g_app.hwndEdit) SetFocus(g_app.hwndEdit);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wParam, g_app.colors.fg);
        SetBkColor((HDC)wParam, g_app.colors.bg);
        return (LRESULT)g_app.hThemeBrush;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
        if (dis->hwndItem != g_app.hwndStatus) break;
        HBRUSH b = CreateSolidBrush(g_app.colors.statusBg);
        FillRect(dis->hDC, &dis->rcItem, b);
        DeleteObject(b);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, g_app.colors.statusText);
        RECT rc = dis->rcItem;
        rc.left += 4;
        DrawTextW(dis->hDC, (LPCWSTR)dis->itemData, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        return TRUE;
    }
    case WM_UAHDRAWMENU: {
        if (!IsDarkColor(g_app.colors.bg)) break;
        UAHMENU *pum = (UAHMENU *)lParam;
        RECT rc = MenuBarRect(hwnd);
        HBRUSH b = CreateSolidBrush(g_app.colors.statusBg);
        FillRect(pum->hdc, &rc, b);
        DeleteObject(b);
        return TRUE;
    }
    case WM_UAHDRAWMENUITEM: {
        if (!IsDarkColor(g_app.colors.bg)) break;
        UAHDRAWMENUITEM *pmi = (UAHDRAWMENUITEM *)lParam;
        WCHAR text[128] = L"";
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text) - 1;
        GetMenuItemInfoW(pmi->um.hmenu, pmi->umi.iPosition, TRUE, &mii);

        BOOL hot = (pmi->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        HBRUSH b = CreateSolidBrush(hot ? Lighten(g_app.colors.statusBg, 24) : g_app.colors.statusBg);
        FillRect(pmi->um.hdc, &pmi->dis.rcItem, b);
        DeleteObject(b);
        SetBkMode(pmi->um.hdc, TRANSPARENT);
        SetTextColor(pmi->um.hdc, g_app.colors.statusText);
        DrawTextW(pmi->um.hdc, text, -1, &pmi->dis.rcItem, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return TRUE;
    }
    case WM_NCACTIVATE:
    case WM_NCPAINT: {
        LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (IsDarkColor(g_app.colors.bg)) {
            RECT rc = MenuBarRect(hwnd);
            rc.top = rc.bottom;
            rc.bottom += 1;
            HDC hdc = GetWindowDC(hwnd);
            HBRUSH b = CreateSolidBrush(g_app.colors.statusBg);
            FillRect(hdc, &rc, b);
            DeleteObject(b);
            ReleaseDC(hwnd, hdc);
        }
        return r;
    }
    case WM_SIZE:
        UpdateLayout(hwnd);
        UpdateStatusBar(hwnd);
        return 0;
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        WCHAR path[MAX_PATH_BUFFER];
        if (DragQueryFileW(hDrop, 0, path, ARRAYSIZE(path))) {
            if (PromptSaveChanges(hwnd)) {
                LoadDocumentFromPath(hwnd, path);
            }
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && (HWND)lParam == g_app.hwndEdit) {
            g_app.modified = (SendMessageW(g_app.hwndEdit, EM_GETMODIFY, 0, 0) != 0);
            UpdateTitle(hwnd);
            UpdateStatusBar(hwnd);
            return 0;
        } else if (HIWORD(wParam) == EN_UPDATE && (HWND)lParam == g_app.hwndEdit) {
            UpdateStatusBar(hwnd);
            return 0;
        }
        HandleCommand(hwnd, wParam, lParam);
        return 0;
    case WM_INITMENUPOPUP:
        UpdateMenuStates(hwnd);
        return 0;
    case WM_CLOSE:
        if (PromptSaveChanges(hwnd)) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        SaveSettings();
        if (g_app.hThemeBrush) DeleteObject(g_app.hThemeBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    g_hInst = hInstance;
    g_findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    InitDarkMode();
    g_app.wordWrap = FALSE;
    g_app.statusVisible = TRUE;
    g_app.encoding = ENC_UTF8;
    g_app.findFlags = FR_DOWN;
    g_app.theme = 0;
    g_app.colors = g_themes[0];
    g_app.custom = g_themes[0];
    LoadSettings();
    g_app.statusBeforeWrap = g_app.statusVisible;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_RETROPAD));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RETROPAD_WINDOW";
    wc.lpszMenuName = MAKEINTRESOURCE(IDC_RETROPAD);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class.", APP_TITLE, MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, DEFAULT_WIDTH, DEFAULT_HEIGHT,
                                NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.", APP_TITLE, MB_ICONERROR);
        return 0;
    }

    g_app.hwndMain = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    HACCEL accel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCE(IDC_RETROPAD));

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!accel || !TranslateAcceleratorW(hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}
