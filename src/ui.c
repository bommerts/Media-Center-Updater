#include "ui.h"
#include <uxtheme.h>

/* ---- GDI+ (flat C interface) for smooth shapes. Text is drawn with GDI. ---- */

typedef struct GpGraphics GpGraphics;
typedef struct GpBrush GpBrush;
typedef struct GpPen GpPen;
typedef struct GpPath GpPath;
typedef struct GpPointF { float x, y; } GpPointF;
typedef struct GdiplusStartupInput { UINT32 version; void *debug_callback; BOOL no_thread; BOOL no_codecs; } GdiplusStartupInput;

int WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateFromHDC(HDC dc, GpGraphics **graphics);
int WINAPI GdipDeleteGraphics(GpGraphics *graphics);
int WINAPI GdipSetSmoothingMode(GpGraphics *graphics, int mode);
int WINAPI GdipSetPixelOffsetMode(GpGraphics *graphics, int mode);
int WINAPI GdipCreateSolidFill(DWORD color, GpBrush **brush);
int WINAPI GdipDeleteBrush(GpBrush *brush);
int WINAPI GdipCreatePen1(DWORD color, float width, int unit, GpPen **pen);
int WINAPI GdipDeletePen(GpPen *pen);
int WINAPI GdipSetPenStartCap(GpPen *pen, int cap);
int WINAPI GdipSetPenEndCap(GpPen *pen, int cap);
int WINAPI GdipSetPenLineJoin(GpPen *pen, int join);
int WINAPI GdipCreatePath(int fill_mode, GpPath **path);
int WINAPI GdipDeletePath(GpPath *path);
int WINAPI GdipAddPathArc(GpPath *path, float x, float y, float width, float height, float start, float sweep);
int WINAPI GdipClosePathFigure(GpPath *path);
int WINAPI GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path);
int WINAPI GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path);
int WINAPI GdipFillEllipse(GpGraphics *graphics, GpBrush *brush, float x, float y, float width, float height);
int WINAPI GdipDrawEllipse(GpGraphics *graphics, GpPen *pen, float x, float y, float width, float height);
int WINAPI GdipDrawArc(GpGraphics *graphics, GpPen *pen, float x, float y, float width, float height, float start, float sweep);
int WINAPI GdipDrawLines(GpGraphics *graphics, GpPen *pen, const GpPointF *points, int count);
int WINAPI GdipFillPolygon(GpGraphics *graphics, GpBrush *brush, const GpPointF *points, int count, int fill_mode);
int WINAPI GdipDrawPolygon(GpGraphics *graphics, GpPen *pen, const GpPointF *points, int count);

enum { SMOOTHING_ANTIALIAS = 4, PIXEL_OFFSET_HALF = 4, UNIT_PIXEL = 2, CAP_ROUND = 2, JOIN_ROUND = 2 };

/* ---- State ---- */

/* Each control remembers what it is and how it is drawn in its user data. */
#define TAG_MASK 0xF0000000
#define TAG_LABEL 0x10000000
#define TAG_BUTTON 0x20000000
#define TAG_EDIT 0x30000000
#define TAG_COMBO 0x40000000
#define TAG_SPINNER 0x50000000
#define FLAG_SELECTED 0x08000000
#define DATA(tag, font, detail, surface) ((LONG_PTR)((tag) | ((font) << 16) | ((detail) << 8) | (surface)))
#define DATA_FONT(data) ((FontId)(((data) >> 16) & 0xFF))
#define DATA_DETAIL(data) ((int)(((data) >> 8) & 0xFF))
#define DATA_SURFACE(data) ((Surface)((data) & 0xFF))

static UINT dpi = 96;
static Theme theme;
static HFONT fonts[FONT_COUNT];
static int line_heights[FONT_COUNT];
static HBRUSH surface_brushes[SURFACE_COUNT], field_brush;
static ULONG_PTR gdiplus_token;
static HWND tooltips;

typedef int (WINAPI *SetPreferredAppModeFn)(int mode);
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND window, BOOL allow);
typedef void (WINAPI *FlushMenuThemesFn)(void);
static SetPreferredAppModeFn set_preferred_app_mode;
static AllowDarkModeForWindowFn allow_dark_mode_for_window;
static FlushMenuThemesFn flush_menu_themes;

static LRESULT CALLBACK spinner_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

int ui_px(int value) { return MulDiv(value, (int)dpi, 96); }
const Theme *ui_theme(void) { return &theme; }
HFONT ui_font(FontId id) { return fonts[id]; }
int ui_line_height(FontId id) { return line_heights[id]; }

void ui_init(void)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);
    GdiplusStartupInput input = { 1, NULL, FALSE, FALSE };
    GdiplusStartup(&gdiplus_token, &input, NULL);

    WNDCLASSW spinner = { 0 };
    spinner.lpfnWndProc = spinner_proc;
    spinner.hInstance = GetModuleHandleW(NULL);
    spinner.lpszClassName = L"McuSpinner";
    spinner.cbWndExtra = sizeof(LONG_PTR) * 2;
    RegisterClassW(&spinner);

    /* Windows has no public way to give classic controls, menus and scroll bars their dark appearance.
       These uxtheme entry points have been stable since Windows 10 version 1903, the oldest version this
       app supports is newer, and the app still works (with light menus and scroll bars) without them. */
    typedef LONG (WINAPI *GetVersionFn)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    GetVersionFn get_version = ntdll ? (GetVersionFn)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    OSVERSIONINFOW version = { sizeof(version) };
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme && get_version && get_version(&version) == 0 && version.dwBuildNumber >= 18362)
    {
        allow_dark_mode_for_window = (AllowDarkModeForWindowFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
        set_preferred_app_mode = (SetPreferredAppModeFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
        flush_menu_themes = (FlushMenuThemesFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    }
    ui_set_dpi(96);
    ui_set_theme(true);
}

void ui_shutdown(void)
{
    for (int i = 0; i < FONT_COUNT; i++) if (fonts[i]) DeleteObject(fonts[i]);
    for (int i = 0; i < SURFACE_COUNT; i++) if (surface_brushes[i]) DeleteObject(surface_brushes[i]);
    if (field_brush) DeleteObject(field_brush);
    if (gdiplus_token) GdiplusShutdown(gdiplus_token);
}

void ui_set_dpi(UINT value)
{
    static const struct { int size; int weight; } specs[FONT_COUNT] =
        { { 14, 400 }, { 14, 600 }, { 12, 400 }, { 12, 600 }, { 17, 600 }, { 20, 600 }, { 26, 600 } };
    dpi = value ? value : 96;
    HDC dc = GetDC(NULL);
    for (int i = 0; i < FONT_COUNT; i++)
    {
        if (fonts[i]) DeleteObject(fonts[i]);
        fonts[i] = CreateFontW(-ui_px(specs[i].size), 0, 0, 0, specs[i].weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        TEXTMETRICW metrics;
        HGDIOBJ previous = SelectObject(dc, fonts[i]);
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        line_heights[i] = metrics.tmHeight;
    }
    ReleaseDC(NULL, dc);
    if (tooltips) SendMessageW(tooltips, TTM_SETMAXTIPWIDTH, 0, ui_px(340));
}

bool ui_system_uses_dark(void)
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
}

void ui_set_theme(bool dark)
{
    theme.dark = dark;
    if (dark)
    {
        theme.surface[SURFACE_PAGE] = RGB(32, 32, 32);
        theme.surface[SURFACE_CARD] = RGB(43, 43, 43);
        theme.surface[SURFACE_INFO] = RGB(39, 39, 39);
        theme.surface[SURFACE_SUCCESS] = RGB(57, 61, 27);
        theme.surface[SURFACE_WARNING] = RGB(67, 53, 25);
        theme.surface[SURFACE_ERROR] = RGB(68, 39, 38);
        theme.severity[NOTICE_INFO] = RGB(96, 205, 255);
        theme.severity[NOTICE_SUCCESS] = RGB(108, 203, 95);
        theme.severity[NOTICE_WARNING] = RGB(252, 225, 0);
        theme.severity[NOTICE_ERROR] = RGB(255, 153, 164);
        theme.card_stroke = RGB(58, 58, 58);
        theme.text = RGB(255, 255, 255);
        theme.text_secondary = RGB(200, 200, 200);
        theme.text_disabled = RGB(120, 120, 120);
        theme.accent = RGB(96, 205, 255);
        theme.accent_hot = RGB(90, 189, 233);
        theme.accent_text = RGB(0, 0, 0);
        theme.control = RGB(55, 55, 55);
        theme.control_hot = RGB(62, 62, 62);
        theme.control_pressed = RGB(48, 48, 48);
        theme.control_stroke = RGB(72, 72, 72);
        theme.field = RGB(31, 31, 31);
        theme.field_stroke = RGB(96, 96, 96);
    }
    else
    {
        theme.surface[SURFACE_PAGE] = RGB(243, 243, 243);
        theme.surface[SURFACE_CARD] = RGB(251, 251, 251);
        theme.surface[SURFACE_INFO] = RGB(249, 249, 249);
        theme.surface[SURFACE_SUCCESS] = RGB(223, 246, 221);
        theme.surface[SURFACE_WARNING] = RGB(255, 244, 206);
        theme.surface[SURFACE_ERROR] = RGB(253, 231, 233);
        theme.severity[NOTICE_INFO] = RGB(0, 95, 184);
        theme.severity[NOTICE_SUCCESS] = RGB(15, 123, 15);
        theme.severity[NOTICE_WARNING] = RGB(157, 93, 0);
        theme.severity[NOTICE_ERROR] = RGB(196, 43, 28);
        theme.card_stroke = RGB(226, 226, 226);
        theme.text = RGB(26, 26, 26);
        theme.text_secondary = RGB(96, 96, 96);
        theme.text_disabled = RGB(160, 160, 160);
        theme.accent = RGB(0, 95, 184);
        theme.accent_hot = RGB(26, 111, 191);
        theme.accent_text = RGB(255, 255, 255);
        theme.control = RGB(253, 253, 253);
        theme.control_hot = RGB(246, 246, 246);
        theme.control_pressed = RGB(240, 240, 240);
        theme.control_stroke = RGB(208, 208, 208);
        theme.field = RGB(255, 255, 255);
        theme.field_stroke = RGB(160, 160, 160);
    }
    for (int i = 0; i < SURFACE_COUNT; i++)
    {
        if (surface_brushes[i]) DeleteObject(surface_brushes[i]);
        surface_brushes[i] = CreateSolidBrush(theme.surface[i]);
    }
    if (field_brush) DeleteObject(field_brush);
    field_brush = CreateSolidBrush(theme.field);
    if (set_preferred_app_mode) set_preferred_app_mode(dark ? 2 : 3);
    if (flush_menu_themes) flush_menu_themes();
}

void ui_theme_scrollbars(HWND window)
{
    if (allow_dark_mode_for_window) allow_dark_mode_for_window(window, theme.dark);
    SetWindowTheme(window, theme.dark && allow_dark_mode_for_window ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

/* ---- Controls ---- */

static HWND create_control(const wchar_t *class_name, const wchar_t *text, DWORD style, HWND parent, int id, FontId font, LONG_PTR data)
{
    HWND control = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SetWindowLongPtrW(control, GWLP_USERDATA, data);
    SendMessageW(control, WM_SETFONT, (WPARAM)fonts[font], FALSE);
    return control;
}

HWND ui_label(HWND parent, const wchar_t *text, FontId font, TextRole role, Surface surface)
{
    return create_control(L"STATIC", text, SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, parent, 0, font, DATA(TAG_LABEL, font, role, surface));
}

HWND ui_button(HWND parent, int id, const wchar_t *text, ButtonKind kind, Surface surface)
{
    DWORD style = WS_TABSTOP | (kind == BUTTON_SWITCH || kind == BUTTON_CHECK ? BS_AUTOCHECKBOX : BS_PUSHBUTTON);
    FontId font = kind == BUTTON_TAB ? FONT_STRONG : FONT_BODY;
    return create_control(L"BUTTON", text, style, parent, id, font, DATA(TAG_BUTTON, font, kind, surface));
}

HWND ui_edit(HWND parent, int id, bool password, const wchar_t *placeholder)
{
    HWND edit = create_control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0), parent, id,
        FONT_BODY, DATA(TAG_EDIT, FONT_BODY, 0, SURFACE_CARD));
    SendMessageW(edit, EM_SETLIMITTEXT, URLN - 1, 0);
    if (placeholder) SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)placeholder);
    return edit;
}

static void theme_combo(HWND combo)
{
    COMBOBOXINFO info = { sizeof(info) };
    if (allow_dark_mode_for_window) allow_dark_mode_for_window(combo, theme.dark);
    SetWindowTheme(combo, theme.dark && allow_dark_mode_for_window ? L"DarkMode_CFD" : L"CFD", NULL);
    if (GetComboBoxInfo(combo, &info) && info.hwndList) ui_theme_scrollbars(info.hwndList);
    SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, ui_px(24));
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, ui_px(26));
}

HWND ui_combo(HWND parent, int id)
{
    HWND combo = create_control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS, parent, id,
        FONT_BODY, DATA(TAG_COMBO, FONT_BODY, 0, SURFACE_CARD));
    theme_combo(combo);
    return combo;
}

HWND ui_spinner(HWND parent, Surface surface)
{
    HWND spinner = create_control(L"McuSpinner", L"", 0, parent, 0, FONT_BODY, DATA(TAG_SPINNER, 0, 0, surface));
    ShowWindow(spinner, SW_HIDE);
    return spinner;
}

void ui_spinner_set(HWND spinner, bool active)
{
    bool was_active = GetWindowLongPtrW(spinner, sizeof(LONG_PTR)) != 0;
    if (was_active == active) return;
    SetWindowLongPtrW(spinner, sizeof(LONG_PTR), active);
    if (active) SetTimer(spinner, 1, 30, NULL);
    else KillTimer(spinner, 1);
    ShowWindow(spinner, active ? SW_SHOWNA : SW_HIDE);
}

void ui_set_surface(HWND control, Surface surface)
{
    LONG_PTR data = GetWindowLongPtrW(control, GWLP_USERDATA);
    if (DATA_SURFACE(data) == surface) return;
    SetWindowLongPtrW(control, GWLP_USERDATA, (data & ~(LONG_PTR)0xFF) | surface);
    InvalidateRect(control, NULL, TRUE);
}

void ui_set_selected(HWND tab, bool selected)
{
    LONG_PTR data = GetWindowLongPtrW(tab, GWLP_USERDATA);
    SetWindowLongPtrW(tab, GWLP_USERDATA, selected ? data | FLAG_SELECTED : data & ~(LONG_PTR)FLAG_SELECTED);
    InvalidateRect(tab, NULL, TRUE);
}

void ui_set_text(HWND control, const wchar_t *text)
{
    int length = GetWindowTextLengthW(control);
    wchar_t *current = xalloc(((size_t)length + 2) * sizeof(wchar_t));
    GetWindowTextW(control, current, length + 1);
    if (wcscmp(current, text) != 0) SetWindowTextW(control, text);
    free(current);
}

void ui_add_tooltip(HWND control, const wchar_t *text)
{
    if (!tooltips)
    {
        tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, GetAncestor(control, GA_ROOT), NULL, GetModuleHandleW(NULL), NULL);
        SendMessageW(tooltips, TTM_SETMAXTIPWIDTH, 0, ui_px(340));
        SendMessageW(tooltips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    }
    TOOLINFOW tool = { sizeof(tool) };
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = GetParent(control);
    tool.uId = (UINT_PTR)control;
    tool.lpszText = (wchar_t *)text;
    SendMessageW(tooltips, TTM_ADDTOOLW, 0, (LPARAM)&tool);
}

static BOOL CALLBACK refresh_control(HWND control, LPARAM unused)
{
    LONG_PTR data = GetWindowLongPtrW(control, GWLP_USERDATA);
    (void)unused;
    switch (data & TAG_MASK)
    {
    case TAG_LABEL: case TAG_BUTTON: case TAG_EDIT:
        SendMessageW(control, WM_SETFONT, (WPARAM)fonts[DATA_FONT(data)], FALSE);
        break;
    case TAG_COMBO:
        SendMessageW(control, WM_SETFONT, (WPARAM)fonts[FONT_BODY], FALSE);
        theme_combo(control);
        break;
    }
    return TRUE;
}

void ui_refresh_controls(HWND root)
{
    EnumChildWindows(root, refresh_control, 0);
    if (tooltips) SetWindowTheme(tooltips, theme.dark && allow_dark_mode_for_window ? L"DarkMode_Explorer" : L"Explorer", NULL);
    RedrawWindow(root, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static wchar_t *control_text(HWND control, int *length)
{
    *length = GetWindowTextLengthW(control);
    wchar_t *text = xalloc(((size_t)*length + 2) * sizeof(wchar_t));
    *length = GetWindowTextW(control, text, *length + 1);
    return text;
}

int ui_text_height(HWND control, int width)
{
    int length;
    wchar_t *text = control_text(control, &length);
    RECT rect = { 0, 0, width > 1 ? width : 1, 0 };
    if (length)
    {
        HDC dc = GetDC(control);
        HGDIOBJ previous = SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
        DrawTextW(dc, text, length, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, previous);
        ReleaseDC(control, dc);
    }
    free(text);
    return rect.bottom;
}

SIZE ui_text_size(HWND control)
{
    int length;
    wchar_t *text = control_text(control, &length);
    RECT rect = { 0, 0, 0, 0 };
    HDC dc = GetDC(control);
    HGDIOBJ previous = SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    DrawTextW(dc, length ? text : L" ", length ? length : 1, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
    free(text);
    SIZE size = { length ? rect.right : 0, rect.bottom };
    return size;
}

/* ---- Shapes ---- */

static DWORD argb(COLORREF color)
{
    return 0xFF000000 | ((DWORD)GetRValue(color) << 16) | ((DWORD)GetGValue(color) << 8) | GetBValue(color);
}

static GpGraphics *begin_shapes(HDC dc)
{
    GpGraphics *graphics = NULL;
    if (GdipCreateFromHDC(dc, &graphics) != 0) return NULL;
    GdipSetSmoothingMode(graphics, SMOOTHING_ANTIALIAS);
    GdipSetPixelOffsetMode(graphics, PIXEL_OFFSET_HALF);
    return graphics;
}

static GpPen *round_pen(COLORREF color, float width)
{
    GpPen *pen = NULL;
    GdipCreatePen1(argb(color), width, UNIT_PIXEL, &pen);
    GdipSetPenStartCap(pen, CAP_ROUND);
    GdipSetPenEndCap(pen, CAP_ROUND);
    GdipSetPenLineJoin(pen, JOIN_ROUND);
    return pen;
}

static GpPath *round_path(float x, float y, float width, float height, float radius)
{
    GpPath *path = NULL;
    float d = radius * 2;
    if (d > width) d = width;
    if (d > height) d = height;
    if (d < 1) d = 1;
    GdipCreatePath(0, &path);
    GdipAddPathArc(path, x, y, d, d, 180, 90);
    GdipAddPathArc(path, x + width - d, y, d, d, 270, 90);
    GdipAddPathArc(path, x + width - d, y + height - d, d, d, 0, 90);
    GdipAddPathArc(path, x, y + height - d, d, d, 90, 90);
    GdipClosePathFigure(path);
    return path;
}

void ui_round_rect(HDC dc, const RECT *rect, int radius, COLORREF fill, COLORREF stroke, bool has_stroke)
{
    GpGraphics *graphics = begin_shapes(dc);
    if (!graphics) return;
    float inset = has_stroke ? 0.5f : 0;
    GpPath *path = round_path(rect->left + inset, rect->top + inset, rect->right - rect->left - inset * 2,
        rect->bottom - rect->top - inset * 2, (float)radius);
    GpBrush *brush = NULL;
    GdipCreateSolidFill(argb(fill), &brush);
    GdipFillPath(graphics, brush, path);
    GdipDeleteBrush(brush);
    if (has_stroke)
    {
        GpPen *pen = NULL;
        GdipCreatePen1(argb(stroke), 1, UNIT_PIXEL, &pen);
        GdipDrawPath(graphics, pen, path);
        GdipDeletePen(pen);
    }
    GdipDeletePath(path);
    GdipDeleteGraphics(graphics);
}

void ui_circle(HDC dc, int x, int y, int diameter, COLORREF color)
{
    GpGraphics *graphics = begin_shapes(dc);
    GpBrush *brush = NULL;
    if (!graphics) return;
    GdipCreateSolidFill(argb(color), &brush);
    GdipFillEllipse(graphics, brush, (float)x, (float)y, (float)diameter, (float)diameter);
    GdipDeleteBrush(brush);
    GdipDeleteGraphics(graphics);
}

static void draw_lines(GpGraphics *graphics, GpPen *pen, float x, float y, float scale, const float *points, int count)
{
    GpPointF scaled[8];
    for (int i = 0; i < count && i < 8; i++)
    {
        scaled[i].x = x + points[i * 2] * scale;
        scaled[i].y = y + points[i * 2 + 1] * scale;
    }
    GdipDrawLines(graphics, pen, scaled, count);
}

void ui_severity_icon(HDC dc, int x, int y, int size, Severity severity)
{
    static const float info_line[] = { .5f, .46f, .5f, .72f }, info_dot[] = { .5f, .29f, .5f, .30f };
    static const float warning_line[] = { .5f, .27f, .5f, .55f }, warning_dot[] = { .5f, .72f, .5f, .73f };
    static const float check[] = { .29f, .52f, .44f, .67f, .72f, .36f };
    static const float cross_a[] = { .34f, .34f, .66f, .66f }, cross_b[] = { .66f, .34f, .34f, .66f };
    GpGraphics *graphics = begin_shapes(dc);
    if (!graphics) return;
    GpBrush *brush = NULL;
    GdipCreateSolidFill(argb(theme.severity[severity]), &brush);
    GdipFillEllipse(graphics, brush, (float)x, (float)y, (float)size, (float)size);
    GdipDeleteBrush(brush);
    GpPen *pen = round_pen(theme.dark ? RGB(0, 0, 0) : RGB(255, 255, 255), size * 0.11f);
    float fx = (float)x, fy = (float)y, scale = (float)size;
    switch (severity)
    {
    case NOTICE_INFO: draw_lines(graphics, pen, fx, fy, scale, info_line, 2); draw_lines(graphics, pen, fx, fy, scale, info_dot, 2); break;
    case NOTICE_WARNING: draw_lines(graphics, pen, fx, fy, scale, warning_line, 2); draw_lines(graphics, pen, fx, fy, scale, warning_dot, 2); break;
    case NOTICE_SUCCESS: draw_lines(graphics, pen, fx, fy, scale, check, 3); break;
    default: draw_lines(graphics, pen, fx, fy, scale, cross_a, 2); draw_lines(graphics, pen, fx, fy, scale, cross_b, 2);
    }
    GdipDeletePen(pen);
    GdipDeleteGraphics(graphics);
}

/* The app logo, drawn as shapes on a 100x100 grid (same design as Assets\Logo.svg). */
void ui_logo(HDC dc, int x, int y, int size)
{
    static const float shaft[] = { 74, 84, 74, 64 }, head[] = { 66, 72, 74, 64, 82, 72 };
    GpGraphics *graphics = begin_shapes(dc);
    if (!graphics) return;
    float fx = (float)x, fy = (float)y, scale = size / 100.0f;
    GpBrush *brush = NULL;
    GpPen *pen = NULL;
    GpPath *path = round_path(fx, fy, (float)size, (float)size, 22 * scale);
    GdipCreateSolidFill(argb(RGB(0x1E, 0x29, 0x3B)), &brush);
    GdipFillPath(graphics, brush, path);
    GdipDeleteBrush(brush);
    GdipDeletePath(path);

    GpPointF triangle[3] = { { fx + 30 * scale, fy + 24 * scale }, { fx + 30 * scale, fy + 72 * scale }, { fx + 70 * scale, fy + 48 * scale } };
    GdipCreateSolidFill(argb(RGB(0xF5, 0x9E, 0x0B)), &brush);
    GdipFillPolygon(graphics, brush, triangle, 3, 0);
    GdipDeleteBrush(brush);
    pen = round_pen(RGB(0xF5, 0x9E, 0x0B), 8 * scale);
    GdipDrawPolygon(graphics, pen, triangle, 3);
    GdipDeletePen(pen);

    GdipCreateSolidFill(argb(RGB(0x22, 0xC5, 0x5E)), &brush);
    GdipFillEllipse(graphics, brush, fx + 54 * scale, fy + 54 * scale, 40 * scale, 40 * scale);
    GdipDeleteBrush(brush);
    pen = round_pen(RGB(0x1E, 0x29, 0x3B), 5 * scale);
    GdipDrawEllipse(graphics, pen, fx + 54 * scale, fy + 54 * scale, 40 * scale, 40 * scale);
    GdipDeletePen(pen);

    pen = round_pen(RGB(255, 255, 255), 5 * scale);
    draw_lines(graphics, pen, fx, fy, scale, shaft, 2);
    draw_lines(graphics, pen, fx, fy, scale, head, 3);
    GdipDeletePen(pen);
    GdipDeleteGraphics(graphics);
}

/* ---- Messages from controls ---- */

LRESULT ui_on_ctlcolor(UINT message, HDC dc, HWND control)
{
    LONG_PTR data = GetWindowLongPtrW(control, GWLP_USERDATA);
    LONG_PTR tag = data & TAG_MASK;
    if (message == WM_CTLCOLORSTATIC && tag == TAG_LABEL)
    {
        Surface surface = DATA_SURFACE(data);
        SetTextColor(dc, DATA_DETAIL(data) == TEXT_SECONDARY ? theme.text_secondary : theme.text);
        SetBkColor(dc, theme.surface[surface]);
        return (LRESULT)surface_brushes[surface];
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || tag == TAG_EDIT)
    {
        /* A disabled edit box asks with WM_CTLCOLORSTATIC, and Windows greys its text by itself. */
        SetTextColor(dc, theme.text);
        SetBkColor(dc, theme.field);
        return (LRESULT)field_brush;
    }
    SetTextColor(dc, theme.text);
    SetBkColor(dc, theme.surface[SURFACE_PAGE]);
    return (LRESULT)surface_brushes[SURFACE_PAGE];
}

static void draw_button(HWND button, HDC target, UINT state, LONG_PTR data)
{
    static const float check[] = { .26f, .52f, .42f, .68f, .75f, .32f };
    RECT rect;
    wchar_t text[256];
    GetClientRect(button, &rect);
    int width = rect.right, height = rect.bottom;
    if (width <= 0 || height <= 0) return;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HGDIOBJ old_font = SelectObject(dc, fonts[DATA_FONT(data)]);
    ButtonKind kind = (ButtonKind)DATA_DETAIL(data);
    bool disabled = (state & CDIS_DISABLED) || !IsWindowEnabled(button);
    bool hot = (state & CDIS_HOT) && !disabled, pressed = (state & CDIS_SELECTED) && !disabled;
    bool checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool focused = (state & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    COLORREF text_color = disabled ? theme.text_disabled : theme.text;
    GetWindowTextW(button, text, COUNTOF(text));
    FillRect(dc, &rect, surface_brushes[DATA_SURFACE(data)]);
    SetBkMode(dc, TRANSPARENT);

    switch (kind)
    {
    case BUTTON_STANDARD:
        ui_round_rect(dc, &rect, ui_px(4), pressed ? theme.control_pressed : hot ? theme.control_hot : theme.control, theme.control_stroke, true);
        SetTextColor(dc, text_color);
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
    case BUTTON_ACCENT:
        if (disabled) ui_round_rect(dc, &rect, ui_px(4), theme.control, theme.control_stroke, true);
        else ui_round_rect(dc, &rect, ui_px(4), pressed || hot ? theme.accent_hot : theme.accent, 0, false);
        SetTextColor(dc, disabled ? theme.text_disabled : theme.accent_text);
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
    case BUTTON_SWITCH:
    {
        RECT track = { 0, (height - ui_px(20)) / 2, ui_px(40), (height - ui_px(20)) / 2 + ui_px(20) };
        COLORREF on_color = disabled ? theme.text_disabled : hot ? theme.accent_hot : theme.accent;
        COLORREF off_color = disabled ? theme.text_disabled : theme.text_secondary;
        int knob = ui_px(hot || pressed ? 14 : 12);
        if (checked) ui_round_rect(dc, &track, ui_px(10), on_color, 0, false);
        else ui_round_rect(dc, &track, ui_px(10), theme.surface[DATA_SURFACE(data)], off_color, true);
        ui_circle(dc, checked ? track.right - ui_px(10) - knob / 2 : track.left + ui_px(10) - knob / 2,
            track.top + (ui_px(20) - knob) / 2, knob, checked ? theme.accent_text : off_color);
        RECT label = { track.right + ui_px(12), 0, width, height };
        SetTextColor(dc, text_color);
        DrawTextW(dc, checked ? L"On" : L"Off", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
    }
    case BUTTON_CHECK:
    {
        int size = ui_px(20);
        RECT box = { 0, (height - size) / 2, size, (height - size) / 2 + size };
        if (checked)
        {
            ui_round_rect(dc, &box, ui_px(4), disabled ? theme.text_disabled : hot ? theme.accent_hot : theme.accent, 0, false);
            GpGraphics *graphics = begin_shapes(dc);
            if (graphics)
            {
                GpPen *pen = round_pen(theme.accent_text, size * 0.1f);
                draw_lines(graphics, pen, (float)box.left, (float)box.top, (float)size, check, 3);
                GdipDeletePen(pen);
                GdipDeleteGraphics(graphics);
            }
        }
        else ui_round_rect(dc, &box, ui_px(4), hot ? theme.control_hot : theme.field, disabled ? theme.text_disabled : theme.text_secondary, true);
        RECT label = { box.right + ui_px(8), 0, width, height };
        SetTextColor(dc, text_color);
        DrawTextW(dc, text, -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        break;
    }
    case BUTTON_TAB:
    {
        bool selected = (data & FLAG_SELECTED) != 0;
        SIZE extent;
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &extent);
        SetTextColor(dc, selected || hot ? theme.text : theme.text_secondary);
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (selected)
        {
            RECT bar = { (width - extent.cx) / 2, height - ui_px(4), (width + extent.cx) / 2, height - ui_px(1) };
            ui_round_rect(dc, &bar, ui_px(2), theme.accent, 0, false);
        }
        break;
    }
    }
    if (focused)
    {
        GpGraphics *graphics = begin_shapes(dc);
        if (graphics)
        {
            GpPath *path = round_path(1, 1, (float)width - 2, (float)height - 2, (float)ui_px(5));
            GpPen *pen = round_pen(theme.text, (float)ui_px(2) > 2 ? 2.0f : (float)ui_px(2));
            GdipDrawPath(graphics, pen, path);
            GdipDeletePen(pen);
            GdipDeletePath(path);
            GdipDeleteGraphics(graphics);
        }
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

bool ui_on_notify(const NMHDR *header, LRESULT *result)
{
    if (header->code != NM_CUSTOMDRAW) return false;
    LONG_PTR data = GetWindowLongPtrW(header->hwndFrom, GWLP_USERDATA);
    if ((data & TAG_MASK) != TAG_BUTTON) return false;
    const NMCUSTOMDRAW *draw = (const NMCUSTOMDRAW *)header;
    if (draw->dwDrawStage != CDDS_PREERASE && draw->dwDrawStage != CDDS_PREPAINT) return false;
    /* The whole button is drawn here, so Windows draws nothing of its own. */
    draw_button(header->hwndFrom, draw->hdc, draw->uItemState, data);
    *result = CDRF_SKIPDEFAULT;
    return true;
}

static LRESULT CALLBACK spinner_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_TIMER:
        SetWindowLongPtrW(window, 0, (GetWindowLongPtrW(window, 0) + 14) % 360);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint;
        RECT rect;
        HDC target = BeginPaint(window, &paint);
        GetClientRect(window, &rect);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, rect.right, rect.bottom);
        HGDIOBJ previous = SelectObject(dc, bitmap);
        FillRect(dc, &rect, surface_brushes[DATA_SURFACE(GetWindowLongPtrW(window, GWLP_USERDATA))]);
        GpGraphics *graphics = begin_shapes(dc);
        if (graphics)
        {
            float stroke = rect.right * 0.14f;
            GpPen *pen = round_pen(theme.accent, stroke);
            GdipDrawArc(graphics, pen, stroke, stroke, rect.right - stroke * 2, rect.bottom - stroke * 2,
                (float)GetWindowLongPtrW(window, 0), 260);
            GdipDeletePen(pen);
            GdipDeleteGraphics(graphics);
        }
        BitBlt(target, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
