/* The application's window: the Updates and Settings tabs, and everything the user can do from them. */
#include "app.h"
#include "discovery.h"
#include "integration.h"
#include "selfupdate.h"
#include "system.h"
#include "ui.h"
#include "version.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#define WIDE2(text) L##text
#define WIDE(text) WIDE2(text)
#define VERSION_TEXT WIDE(APP_VERSION)
#define S(value) ui_px(value)
#define ACTIVITY_ROWS 30

enum
{
    WM_APP_RESULT = WM_APP + 1, WM_APP_ACTIVITY, WM_APP_BUSY, WM_APP_SUMMARY, WM_APP_RUN_DONE, WM_APP_DISCOVERY,
    WM_APP_PULSARR_KEY, WM_APP_UPDATE_CHECKED, WM_APP_UPDATE_DOWNLOADED, WM_APP_TRAY, WM_APP_SHOW, WM_APP_LAYOUT
};
enum { TIMER_SCHEDULE = 1, TIMER_INSTALL = 2 };
enum
{
    ID_CHECK = 100, ID_INSTALL, ID_TAB_UPDATES, ID_TAB_SETTINGS, ID_OPEN_LOGS,
    ID_SCHEDULE_TOGGLE, ID_SCHEDULE_MODE, ID_INTERVAL, ID_INTERVAL_DOWN, ID_INTERVAL_UP, ID_HOUR, ID_MINUTE, ID_SCHEDULE_CHECK_NOW,
    ID_PULSARR_KEY, ID_PULSARR_KEY_SHOW, ID_FIND_PULSARR_KEY,
    ID_THEME, ID_TRAY, ID_START_MINIMIZED, ID_STARTUP,
    ID_APP_UPDATE_CHECK_TOGGLE, ID_APP_UPDATE_INSTALL_TOGGLE, ID_APP_UPDATE_CHECK, ID_APP_UPDATE_INSTALL,
    ID_RADARR_URL, ID_RADARR_KEY, ID_RADARR_KEY_SHOW, ID_SONARR_URL, ID_SONARR_KEY, ID_SONARR_KEY_SHOW,
    ID_FIND_KEYS, ID_SAVE_CONNECTIONS, ID_ELEVATE, ID_EXIT,
    ID_TRAY_OPEN, ID_TRAY_CHECK, ID_TRAY_EXIT,
    ID_MONITOR = 200, ID_AUTO_INSTALL = 210, ID_USAGE = 220
};

/* Things painted behind the controls of a tab: cards, notices, status dots and text box frames. */
typedef enum { DECOR_CARD, DECOR_INFO, DECOR_DOT, DECOR_FIELD } DecorKind;
typedef struct Decor { DecorKind kind; RECT rect; int detail; HWND control; } Decor;

typedef struct Panel
{
    HWND window;
    int scroll, content_height;
    Decor decor[160], previous[160];
    int decor_count, previous_count;
} Panel;

typedef struct InfoBar { HWND title, message, action; Severity severity; bool open; } InfoBar;

typedef struct ProgramRow
{
    ProgramSettings *settings;
    bool editable;
    UpdateState state;
    wchar_t installed[VERN], available[VERN], message[MSGN];
    HWND name, description, state_text, version, message_text, monitor_header, monitor, auto_header, auto_install;
    HWND usage, usage_description;
} ProgramRow;

typedef struct ActivityRow { HWND time, application, message; } ActivityRow;

static struct
{
    HINSTANCE instance;
    HWND window;
    HICON icon_large, icon_small;
    AppSettings *settings;
    SettingsStore *store;
    Coordinator *coordinator;
    TrayIcon tray;
    UINT taskbar_created;
    bool initializing, exiting, dialog_open, retrieving_keys, hidden_to_tray, layout_pending;
    /* From the last setup check: Pulsarr is installed for automatic updates and turns away requests without a key. */
    bool pulsarr_needs_key;
    int tab;

    /* Updates to this app itself. */
    bool has_app_update, app_update_busy, app_update_checked, install_in_tray;
    SelfUpdateInfo app_update;
    int64_t next_app_update_attempt;
    wchar_t installer_path[PATHN];

    /* Header and footer. */
    HWND title, subtitle, busy_ring, check, install, tabs[2], next_run_text, background_text;
    Panel panels[2];

    /* Updates tab. */
    InfoBar summary;
    HWND apps_title, monitored_text, activity_title, open_logs, empty_activity;
    ProgramRow rows[APP_COUNT];
    ActivityRow activity_rows[ACTIVITY_ROWS];
    ActivityEntry activity[ACTIVITY_ROWS];
    int activity_count;

    /* Settings tab. */
    HWND settings_title, settings_intro;
    HWND schedule_title, schedule_description, schedule_toggle, schedule_mode_label, schedule_mode, interval_label, interval,
        interval_down, interval_up, time_label, hour, minute, schedule_detail, schedule_check_now, schedule_note;
    HWND usage_title, usage_description, pulsarr_key_title, pulsarr_key_description, pulsarr_key, pulsarr_key_show,
        find_pulsarr_key, pulsarr_key_status;
    HWND appearance_title, theme_label, theme, tray_label, tray_toggle, tray_note, start_minimized, startup;
    HWND app_update_title, app_update_description, app_update_check_label, app_update_check_toggle, app_update_check_note,
        app_update_install_label, app_update_install_toggle, app_update_install_note, app_update_check, app_update_install,
        app_update_ring, app_update_status;
    InfoBar app_update_unavailable;
    HWND connections_title, connections_intro, connections_help, radarr_url_label, radarr_url, radarr_key_label, radarr_key,
        radarr_key_show, radarr_key_status, sonarr_url_label, sonarr_url, sonarr_key_label, sonarr_key, sonarr_key_show,
        sonarr_key_status, find_keys, save_connections, key_search_ring, key_search_status;
    HWND pulsarr_title, pulsarr_description;
    InfoBar pulsarr_setup, administrator;
    HWND elevate, settings_status, exit_button, version_text;
} W;

static void request_layout(void);
static void set_busy(bool busy);
static void refresh_app_update_controls(void);
static void refresh_schedule_text(void);
static void run_updates(bool install);
static void install_app_update(bool automatic);

/* ---- Small helpers ---- */

static bool busy(void) { return coordinator_is_busy(W.coordinator); }
static bool is_checked(HWND button) { return SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void set_checked(HWND button, bool checked) { SendMessageW(button, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0); }
static bool is_shown(HWND control) { return (GetWindowLongW(control, GWL_STYLE) & WS_VISIBLE) != 0; }
static int maximum(int a, int b) { return a > b ? a : b; }

static void show(HWND control, bool visible)
{
    if (is_shown(control) == visible) return;
    ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    request_layout();
}

static void enable(HWND control, bool enabled)
{
    if ((IsWindowEnabled(control) != 0) != enabled) EnableWindow(control, enabled);
}

/* Sets a label's text and lays the tab out again, since wrapped text may need a different height. */
static void set_text(HWND control, const wchar_t *text)
{
    ui_set_text(control, text);
    request_layout();
}

static void get_text(HWND control, wchar_t *dst, int cap, bool trim)
{
    dst[0] = 0;
    GetWindowTextW(control, dst, cap);
    if (trim) str_trim(dst);
}

static ProgramSettings *program(ApplicationId id) { return settings_program(W.settings, id); }

static void format_time(int64_t ticks, wchar_t *dst, size_t cap)
{
    time_format_local(ticks, L"MMM d", L" at ", L"h:mm tt", dst, cap);
}

static void start_thread(LPTHREAD_START_ROUTINE routine, void *parameter)
{
    HANDLE thread = CreateThread(NULL, 0, routine, parameter, 0, NULL);
    if (thread) CloseHandle(thread);
}

static void info_set(InfoBar *bar, const wchar_t *title, const wchar_t *message, Severity severity)
{
    bar->severity = severity;
    ui_set_surface(bar->title, (Surface)(SURFACE_INFO + severity));
    ui_set_surface(bar->message, (Surface)(SURFACE_INFO + severity));
    if (bar->action) ui_set_surface(bar->action, (Surface)(SURFACE_INFO + severity));
    set_text(bar->title, title);
    set_text(bar->message, message);
}

static void info_open(InfoBar *bar, bool open)
{
    bar->open = open;
    show(bar->title, open);
    show(bar->message, open);
    if (bar->action) show(bar->action, open);
    request_layout();
}

static void show_summary(const wchar_t *title, const wchar_t *message, Severity severity)
{
    info_set(&W.summary, title, message, severity);
}

static void show_notice(const wchar_t *title, const wchar_t *message)
{
    if (W.dialog_open) return;
    W.dialog_open = true;
    TaskDialog(W.window, NULL, L"Media Center Updater", title, message, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, NULL);
    W.dialog_open = false;
}

/* ---- Program rows ---- */

static const wchar_t *state_label(const ProgramRow *row)
{
    if (!row->settings->enabled) return L"Monitoring off";
    switch (row->state)
    {
    case STATE_CHECKING: return L"Checking for updates";
    case STATE_UP_TO_DATE: return L"Up to date";
    case STATE_UPDATE_AVAILABLE: return L"Update available";
    case STATE_INSTALLING: return L"Installing update";
    case STATE_UPDATED: return L"Updated successfully";
    case STATE_NOT_INSTALLED: return L"Not detected";
    case STATE_NEEDS_SETUP: return L"Setup needed";
    case STATE_ERROR: return L"Needs attention";
    case STATE_RESTART_REQUIRED: return L"Restart required";
    default: return L"Not checked";
    }
}

static COLORREF state_color(const ProgramRow *row)
{
    if (!row->settings->enabled) return RGB(128, 128, 128);
    switch (row->state)
    {
    case STATE_UP_TO_DATE: case STATE_UPDATED: return RGB(36, 157, 100);
    case STATE_ERROR: return RGB(219, 75, 75);
    case STATE_NEEDS_SETUP: case STATE_RESTART_REQUIRED: case STATE_UPDATE_AVAILABLE: return RGB(210, 140, 31);
    case STATE_CHECKING: case STATE_INSTALLING: return RGB(67, 129, 244);
    default: return RGB(128, 128, 128);
    }
}

static void refresh_row(ProgramRow *row)
{
    wchar_t version[VERN * 2 + 48];
    str_fmt(version, COUNTOF(version), L"Installed: %s   \x2022   Latest: %s", row->installed, row->available);
    set_text(row->state_text, state_label(row));
    set_text(row->version, version);
    set_text(row->message_text, row->message);
    enable(row->monitor, row->editable);
    enable(row->auto_install, row->settings->enabled && row->editable);
    enable(row->usage, row->editable);
}

static void apply_result(const UpdateResult *result)
{
    ProgramRow *row = &W.rows[result->id];
    row->state = result->state;
    if (!str_blank(result->installed_version)) str_copy(row->installed, COUNTOF(row->installed), result->installed_version);
    else if (result->state == STATE_NOT_INSTALLED) str_copy(row->installed, COUNTOF(row->installed), L"Not detected");
    if (!str_blank(result->available_version)) str_copy(row->available, COUNTOF(row->available), result->available_version);
    str_copy(row->message, COUNTOF(row->message), result->message);
    refresh_row(row);
}

static void refresh_activity(void)
{
    wchar_t time[80];
    for (int i = 0; i < ACTIVITY_ROWS; i++)
    {
        ActivityRow *row = &W.activity_rows[i];
        bool used = i < W.activity_count;
        if (used)
        {
            time_format_local(W.activity[i].time.ticks, L"MMM d", L", ", L"h:mm tt", time, COUNTOF(time));
            ui_set_text(row->time, time);
            ui_set_text(row->application, W.activity[i].application);
            ui_set_text(row->message, W.activity[i].message);
        }
        show(row->time, used);
        show(row->application, used);
        show(row->message, used);
    }
    show(W.empty_activity, W.activity_count == 0);
    request_layout();
}

static void add_activity(const ActivityEntry *entry)
{
    int keep = W.activity_count < ACTIVITY_ROWS ? W.activity_count : ACTIVITY_ROWS - 1;
    memmove(&W.activity[1], &W.activity[0], sizeof(ActivityEntry) * (size_t)keep);
    W.activity[0] = *entry;
    W.activity_count = keep + 1;
    refresh_activity();
}

static void log_app_activity(const wchar_t *message, bool error)
{
    ActivityEntry *entry = xalloc(sizeof(ActivityEntry));
    entry->time = dto_now();
    str_copy(entry->application, COUNTOF(entry->application), L"Media Center Updater");
    str_copy(entry->message, COUNTOF(entry->message), message);
    entry->is_error = error;
    store_log(W.store, entry);
    add_activity(entry);
    free(entry);
}

/* ---- Layout ---- */

static void place(Panel *panel, HWND control, int x, int y, int width, int height)
{
    RECT current;
    POINT origin = { 0, 0 };
    GetWindowRect(control, &current);
    ClientToScreen(GetParent(control), &origin);
    y -= panel ? panel->scroll : 0;
    if (current.left - origin.x == x && current.top - origin.y == y && current.right - current.left == width &&
        current.bottom - current.top == height)
        return;
    /* Windows would otherwise move the control's picture along with it. Several controls move in one pass,
       so a picture can already have another control's drawn over it; each one is redrawn instead. */
    SetWindowPos(control, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

/* A drop-down list decides its own height; the height passed here only bounds its list. */
static void place_combo(Panel *panel, HWND combo, int x, int y, int width)
{
    RECT current;
    POINT origin = { 0, 0 };
    GetWindowRect(combo, &current);
    ClientToScreen(GetParent(combo), &origin);
    y += (S(32) - (current.bottom - current.top)) / 2;
    if (current.left - origin.x == x && current.top - origin.y == y - panel->scroll && current.right - current.left == width) return;
    SetWindowPos(combo, NULL, x, y - panel->scroll, width, S(400), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

/* Places a label wrapped to a width. Returns the height used, which is 0 for a hidden or empty label. */
static int place_text(Panel *panel, HWND label, int x, int y, int width)
{
    if (!is_shown(label)) return 0;
    int height = ui_text_height(label, width);
    place(panel, label, x, y, width, height);
    return height;
}

static int add_decor(Panel *panel, DecorKind kind, int x, int y, int width, int height, int detail, HWND control)
{
    if (panel->decor_count >= (int)COUNTOF(panel->decor)) return 0;
    Decor *decor = &panel->decor[panel->decor_count];
    memset(decor, 0, sizeof(*decor));
    decor->kind = kind;
    SetRect(&decor->rect, x, y, x + width, y + height);
    decor->detail = detail;
    decor->control = control;
    return panel->decor_count++;
}

/* A text box: a frame painted by the tab, with the borderless edit control inside it. */
static void place_field(Panel *panel, HWND edit, int x, int y, int width)
{
    int line = ui_line_height(FONT_BODY);
    add_decor(panel, DECOR_FIELD, x, y, width, S(32), 0, edit);
    place(panel, edit, x + S(10), y + (S(32) - line) / 2, width - S(20), line);
}

static int button_width(HWND button) { return ui_text_size(button).cx + S(24); }

static int layout_info(Panel *panel, InfoBar *bar, int x, int y, int width)
{
    if (!bar->open) return 0;
    int pad = S(13), text_x = x + S(16) + S(18) + S(12);
    int action_width = bar->action ? button_width(bar->action) : 0;
    int text_width = x + width - S(16) - text_x - (action_width ? action_width + S(16) : 0);
    int title_height = ui_text_height(bar->title, text_width), message_height = ui_text_height(bar->message, text_width);
    int height = pad + title_height + (title_height && message_height ? S(2) : 0) + message_height + pad;
    if (action_width && height < S(32) + S(16)) height = S(32) + S(16);
    add_decor(panel, DECOR_INFO, x, y, width, height, bar->severity, NULL);
    place(panel, bar->title, text_x, y + pad, text_width, title_height);
    place(panel, bar->message, text_x, y + pad + title_height + (title_height ? S(2) : 0), text_width, message_height);
    if (action_width) place(panel, bar->action, x + width - S(16) - action_width, y + (height - S(32)) / 2, action_width, S(32));
    return height;
}

static int layout_program(Panel *panel, ProgramRow *row, int x, int y, int width)
{
    int pad_x = S(16), pad_y = S(13), gap = S(20), c1 = S(240), c3 = S(122), c4 = S(140);
    int c2 = maximum(width - pad_x * 2 - c1 - c3 - c4 - gap * 3, S(120));
    int x1 = x + pad_x, x2 = x1 + c1 + gap, x3 = x2 + c2 + gap, x4 = x3 + c3 + gap;
    int name_height = ui_text_height(row->name, c1), description_height = ui_text_height(row->description, c1);
    int h1 = name_height + S(5) + description_height;
    int dot = S(7), state_height = ui_line_height(FONT_STRONG);
    int version_height = ui_text_height(row->version, c2), message_height = ui_text_height(row->message_text, c2);
    int h2 = state_height + S(6) + version_height + (message_height ? S(6) + message_height : 0);
    int header_height = ui_line_height(FONT_BODY), h3 = header_height + S(6) + S(24);
    int content = maximum(h1, maximum(h2, h3)), top;
    add_decor(panel, DECOR_CARD, x, y, width, content + pad_y * 2, 0, NULL);

    top = y + pad_y + (content - h1) / 2;
    place(panel, row->name, x1, top, c1, name_height);
    place(panel, row->description, x1, top + name_height + S(5), c1, description_height);

    top = y + pad_y + (content - h2) / 2;
    add_decor(panel, DECOR_DOT, x2, top + (state_height - dot) / 2 + S(1), dot, dot, (int)state_color(row), NULL);
    place(panel, row->state_text, x2 + dot + S(8), top, c2 - dot - S(8), state_height);
    place(panel, row->version, x2, top + state_height + S(6), c2, version_height);
    place(panel, row->message_text, x2, top + state_height + S(6) + version_height + S(6), c2, message_height);

    top = y + pad_y + (content - h3) / 2;
    place(panel, row->monitor_header, x3, top, c3, header_height);
    place(panel, row->monitor, x3, top + header_height + S(6), S(96), S(24));
    place(panel, row->auto_header, x4, top, c4, header_height);
    place(panel, row->auto_install, x4, top + header_height + S(6), S(96), S(24));
    return content + pad_y * 2;
}

static void layout_updates(Panel *panel, int width)
{
    int margin = S(32), w = maximum(width - margin * 2, S(700)), x = margin, y = S(8);
    int title_height = ui_line_height(FONT_TITLE), button_height = S(32);
    y += layout_info(panel, &W.summary, x, y, w) + S(18);

    SIZE monitored = ui_text_size(W.monitored_text);
    place(panel, W.apps_title, x, y, w - monitored.cx - S(20), title_height);
    place(panel, W.monitored_text, x + w - monitored.cx, y + (title_height - monitored.cy) / 2, monitored.cx, monitored.cy);
    y += title_height + S(16);
    for (int i = 0; i < APP_COUNT; i++) y += layout_program(panel, &W.rows[i], x, y, w) + S(10);
    y += S(8);

    int logs_width = button_width(W.open_logs);
    place(panel, W.activity_title, x, y + (button_height - title_height) / 2, w - logs_width - S(20), title_height);
    place(panel, W.open_logs, x + w - logs_width, y, logs_width, button_height);
    y += button_height + S(16);

    int card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL), cx = x + S(16), cw = w - S(32);
    y += S(16);
    if (W.activity_count == 0) y += place_text(panel, W.empty_activity, cx, y, cw);
    for (int i = 0; i < W.activity_count; i++)
    {
        ActivityRow *row = &W.activity_rows[i];
        int c1 = S(120), c2 = S(140), gap = S(16), c3 = cw - c1 - c2 - gap * 2;
        int h1 = ui_text_height(row->time, c1), h2 = ui_text_height(row->application, c2), h3 = ui_text_height(row->message, c3);
        y += S(5);
        place(panel, row->time, cx, y, c1, h1);
        place(panel, row->application, cx + c1 + gap, y, c2, h2);
        place(panel, row->message, cx + c1 + c2 + gap * 2, y, c3, h3);
        y += maximum(h1, maximum(h2, h3)) + S(5);
    }
    y += S(16);
    panel->decor[card].rect.bottom = y;
    panel->content_height = y + S(24);
}

static void layout_settings(Panel *panel, int width)
{
    int margin = S(32), w = maximum(width - margin * 2, S(700));
    if (w > S(1000)) w = S(1000);
    int x = maximum((width - w) / 2, margin), y = S(8), pad = S(22), cx = x + pad, cw = w - pad * 2;
    int button_height = S(32), header_height = ui_line_height(FONT_BODY), switch_width = S(96), switch_height = S(24);
    int check_height = S(24), card, h, bx;

    y += place_text(panel, W.settings_title, x, y, w) + S(12);
    y += place_text(panel, W.settings_intro, x, y, w) + S(18);

    /* Scheduled updates. */
    card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL);
    y += pad;
    h = place_text(panel, W.schedule_title, cx, y, cw - S(130));
    h += S(5) + place_text(panel, W.schedule_description, cx, y + h + S(5), cw - S(130));
    place(panel, W.schedule_toggle, cx + cw - switch_width, y + (h - switch_height) / 2, switch_width, switch_height);
    y += h + S(16);
    place(panel, W.schedule_mode_label, cx, y, S(195), header_height);
    place_combo(panel, W.schedule_mode, cx, y + header_height + S(6), S(195));
    bx = cx + S(195) + S(20);
    if (is_shown(W.interval))
    {
        place(panel, W.interval_label, bx, y, S(220), header_height);
        place_field(panel, W.interval, bx, y + header_height + S(6), S(100));
        place(panel, W.interval_down, bx + S(106), y + header_height + S(6), S(36), S(32));
        place(panel, W.interval_up, bx + S(148), y + header_height + S(6), S(36), S(32));
    }
    else
    {
        place(panel, W.time_label, bx, y, S(220), header_height);
        place_combo(panel, W.hour, bx, y + header_height + S(6), S(110));
        place_combo(panel, W.minute, bx + S(118), y + header_height + S(6), S(80));
    }
    y += header_height + S(6) + S(32) + S(16);
    bx = button_width(W.schedule_check_now);
    h = maximum(ui_text_height(W.schedule_detail, cw - bx - S(12)), button_height);
    place(panel, W.schedule_detail, cx, y + (h - ui_text_height(W.schedule_detail, cw - bx - S(12))) / 2, cw - bx - S(12),
        ui_text_height(W.schedule_detail, cw - bx - S(12)));
    place(panel, W.schedule_check_now, cx + cw - bx, y + (h - button_height) / 2, bx, button_height);
    y += h + S(16);
    y += place_text(panel, W.schedule_note, cx, y, cw) + pad;
    panel->decor[card].rect.bottom = y;
    y += S(18);

    /* Wait while apps are in use. */
    card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL);
    y += pad;
    y += place_text(panel, W.usage_title, cx, y, cw) + S(5);
    y += place_text(panel, W.usage_description, cx, y, cw) + S(16);
    for (int i = 0; i < APP_COUNT; i++)
    {
        place(panel, W.rows[i].usage, cx, y, cw, check_height);
        y += check_height + S(2);
        y += place_text(panel, W.rows[i].usage_description, cx + S(28), y, cw - S(28)) + S(8);
    }
    if (is_shown(W.pulsarr_key))
    {
        int key_width = cw < S(560) ? cw : S(560), find_width = button_width(W.find_pulsarr_key);
        int show_width = button_width(W.pulsarr_key_show);
        y += S(8);
        y += place_text(panel, W.pulsarr_key_title, cx, y, key_width) + S(6);
        y += place_text(panel, W.pulsarr_key_description, cx, y, key_width) + S(6);
        place_field(panel, W.pulsarr_key, cx, y, key_width - find_width - show_width - S(16));
        place(panel, W.pulsarr_key_show, cx + key_width - find_width - show_width - S(8), y, show_width, button_height);
        place(panel, W.find_pulsarr_key, cx + key_width - find_width, y, find_width, button_height);
        y += button_height;
        h = ui_text_height(W.pulsarr_key_status, key_width);
        if (h) y += S(6) + place_text(panel, W.pulsarr_key_status, cx, y + S(6), key_width);
    }
    else y -= S(8);
    y += pad;
    panel->decor[card].rect.bottom = y;
    y += S(18);

    /* Appearance and background behavior. */
    card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL);
    y += pad;
    y += place_text(panel, W.appearance_title, cx, y, cw) + S(16);
    {
        int x2 = cx + S(230) + S(36), w2 = cw - S(230) - S(36), right = y;
        place(panel, W.theme_label, cx, y, S(210), header_height);
        place_combo(panel, W.theme, cx, y + header_height + S(6), S(210));
        place(panel, W.tray_label, x2, right, w2, header_height);
        right += header_height + S(6);
        place(panel, W.tray_toggle, x2, right, switch_width, switch_height);
        right += switch_height + S(8);
        right += place_text(panel, W.tray_note, x2, right, w2) + S(8);
        place(panel, W.start_minimized, x2, right, w2, check_height);
        right += check_height + S(8);
        place(panel, W.startup, x2, right, w2, check_height);
        right += check_height;
        y = maximum(right, y + header_height + S(6) + S(32));
    }
    y += pad;
    panel->decor[card].rect.bottom = y;
    y += S(18);

    /* Auto-update. */
    card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL);
    y += pad;
    y += place_text(panel, W.app_update_title, cx, y, cw) + S(5);
    y += place_text(panel, W.app_update_description, cx, y, cw) + S(16);
    h = layout_info(panel, &W.app_update_unavailable, cx, y, cw);
    if (h) y += h + S(16);
    {
        int column = (cw - S(36)) / 2, x2 = cx + column + S(36), left = y, right = y;
        place(panel, W.app_update_check_label, cx, left, column, header_height);
        left += header_height + S(6);
        place(panel, W.app_update_check_toggle, cx, left, switch_width, switch_height);
        left += switch_height + S(4);
        left += place_text(panel, W.app_update_check_note, cx, left, column);
        place(panel, W.app_update_install_label, x2, right, column, header_height);
        right += header_height + S(6);
        place(panel, W.app_update_install_toggle, x2, right, switch_width, switch_height);
        right += switch_height + S(4);
        right += place_text(panel, W.app_update_install_note, x2, right, column);
        y = maximum(left, right) + S(16);
    }
    bx = cx;
    place(panel, W.app_update_check, bx, y, button_width(W.app_update_check), button_height);
    bx += button_width(W.app_update_check) + S(12);
    if (is_shown(W.app_update_install))
    {
        place(panel, W.app_update_install, bx, y, button_width(W.app_update_install), button_height);
        bx += button_width(W.app_update_install) + S(12);
    }
    place(panel, W.app_update_ring, bx, y + (button_height - S(22)) / 2, S(22), S(22));
    y += button_height;
    h = ui_text_height(W.app_update_status, cw);
    if (h) y += S(16) + place_text(panel, W.app_update_status, cx, y + S(16), cw);
    y += pad;
    panel->decor[card].rect.bottom = y;
    y += S(18);

    /* Radarr and Sonarr connections. */
    card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL);
    y += pad;
    y += place_text(panel, W.connections_title, cx, y, cw) + S(16);
    y += place_text(panel, W.connections_intro, cx, y, cw) + S(16);
    y += place_text(panel, W.connections_help, cx, y, cw) + S(16);
    {
        int column = (cw - S(20)) / 2, x2 = cx + column + S(20), show_width = button_width(W.radarr_key_show);
        HWND labels[2][2] = { { W.radarr_url_label, W.radarr_key_label }, { W.sonarr_url_label, W.sonarr_key_label } };
        HWND boxes[2][3] = { { W.radarr_url, W.radarr_key, W.radarr_key_show }, { W.sonarr_url, W.sonarr_key, W.sonarr_key_show } };
        HWND status[2] = { W.radarr_key_status, W.sonarr_key_status };
        for (int i = 0; i < 2; i++)
        {
            place(panel, labels[i][0], cx, y, column, header_height);
            place(panel, labels[i][1], x2, y, column, header_height);
            y += header_height + S(6);
            place_field(panel, boxes[i][0], cx, y, column);
            place_field(panel, boxes[i][1], x2, y, column - show_width - S(8));
            place(panel, boxes[i][2], x2 + column - show_width, y, show_width, button_height);
            y += S(32) + S(12);
            h = place_text(panel, status[i], cx, y, cw);
            if (h) y += h + S(16);
        }
    }
    bx = cx;
    place(panel, W.find_keys, bx, y, button_width(W.find_keys), button_height);
    bx += button_width(W.find_keys) + S(12);
    place(panel, W.save_connections, bx, y, button_width(W.save_connections), button_height);
    bx += button_width(W.save_connections) + S(12);
    place(panel, W.key_search_ring, bx, y + (button_height - S(22)) / 2, S(22), S(22));
    y += button_height;
    h = ui_text_height(W.key_search_status, cw);
    if (h) y += S(16) + place_text(panel, W.key_search_status, cx, y + S(16), cw);
    y += pad;
    panel->decor[card].rect.bottom = y;
    y += S(18);

    /* Pulsarr installation. */
    card = add_decor(panel, DECOR_CARD, x, y, w, 0, 0, NULL);
    y += pad;
    y += place_text(panel, W.pulsarr_title, cx, y, cw) + S(16);
    y += place_text(panel, W.pulsarr_description, cx, y, cw) + S(16);
    y += layout_info(panel, &W.pulsarr_setup, cx, y, cw) + pad;
    panel->decor[card].rect.bottom = y;
    y += S(18);

    h = layout_info(panel, &W.administrator, x, y, w);
    if (h) y += h + S(18);
    h = place_text(panel, W.settings_status, x, y, w);
    if (h) y += h + S(18);
    place(panel, W.exit_button, x, y, button_width(W.exit_button), button_height);
    y += button_height + S(18);
    y += place_text(panel, W.version_text, x, y, w);
    panel->content_height = y + S(24);
}

static void update_scroll_bar(Panel *panel)
{
    RECT client;
    GetClientRect(panel->window, &client);
    SCROLLINFO info = { sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0,
        maximum(panel->content_height - 1, 0), (UINT)client.bottom, panel->scroll, 0 };
    SetScrollInfo(panel->window, SB_VERT, &info, TRUE);
}

static void layout_panel(int index)
{
    Panel *panel = &W.panels[index];
    RECT client;
    GetClientRect(panel->window, &client);
    if (client.right <= 0) return;
    int scroll_before = panel->scroll;
    for (int pass = 0; pass < 2; pass++)
    {
        panel->decor_count = 0;
        if (index == 0) layout_updates(panel, client.right);
        else layout_settings(panel, client.right);
        int limit = maximum(panel->content_height - client.bottom, 0);
        if (panel->scroll <= limit) break;
        panel->scroll = limit;      /* the content became shorter than the scroll position */
    }
    /* The cards are drawn at their place minus the scroll position, so a changed position moves them too. */
    if (panel->scroll != scroll_before || panel->decor_count != panel->previous_count ||
        memcmp(panel->decor, panel->previous, sizeof(Decor) * (size_t)panel->decor_count) != 0)
    {
        memcpy(panel->previous, panel->decor, sizeof(Decor) * (size_t)panel->decor_count);
        panel->previous_count = panel->decor_count;
        InvalidateRect(panel->window, NULL, TRUE);
    }
    update_scroll_bar(panel);
}

static void layout_window(void)
{
    RECT client;
    GetClientRect(W.window, &client);
    int margin = S(32), logo = S(52), top = S(28);
    int title_height = ui_line_height(FONT_HEADER), subtitle_height = ui_line_height(FONT_BODY);
    int text_x = margin + logo + S(16), text_top = top + (logo - title_height - S(3) - subtitle_height) / 2;
    int install_width = button_width(W.install), check_width = button_width(W.check), button_height = S(32);
    int buttons_y = top + (logo - button_height) / 2, right = client.right - margin;
    place(NULL, W.install, right - install_width, buttons_y, install_width, button_height);
    place(NULL, W.check, right - install_width - S(10) - check_width, buttons_y, check_width, button_height);
    place(NULL, W.busy_ring, right - install_width - S(10) - check_width - S(10) - S(22), buttons_y + (button_height - S(22)) / 2, S(22), S(22));
    int text_width = maximum(right - install_width - check_width - S(70) - text_x, S(100));
    place(NULL, W.title, text_x, text_top, text_width, title_height);
    place(NULL, W.subtitle, text_x, text_top + title_height + S(3), text_width, subtitle_height);

    int tabs_y = top + logo + S(14), tab_x = margin, tab_height = S(40);
    for (int i = 0; i < 2; i++)
    {
        int tab_width = ui_text_size(W.tabs[i]).cx + S(24);
        place(NULL, W.tabs[i], tab_x, tabs_y, tab_width, tab_height);
        tab_x += tab_width + S(8);
    }
    int footer_height = S(12) * 2 + ui_line_height(FONT_CAPTION), panel_top = tabs_y + tab_height + S(6);
    int panel_height = maximum(client.bottom - footer_height - panel_top, 0);
    /* The tabs only change size, so their pictures stay where they are and can be kept. */
    for (int i = 0; i < 2; i++)
        SetWindowPos(W.panels[i].window, NULL, 0, panel_top, client.right, panel_height, SWP_NOZORDER | SWP_NOACTIVATE);
    SIZE background = ui_text_size(W.background_text);
    int footer_y = client.bottom - footer_height + S(12);
    place(NULL, W.next_run_text, margin, footer_y, maximum(client.right - margin * 2 - background.cx - S(20), S(50)), ui_line_height(FONT_CAPTION));
    place(NULL, W.background_text, client.right - margin - background.cx, footer_y, background.cx, background.cy);
}

static void layout_all(void)
{
    W.layout_pending = false;
    layout_window();
    layout_panel(0);
    layout_panel(1);
}

static void request_layout(void)
{
    if (W.layout_pending || !W.window) return;
    W.layout_pending = true;
    PostMessageW(W.window, WM_APP_LAYOUT, 0, 0);
}

static void scroll_panel(Panel *panel, int position)
{
    RECT client;
    GetClientRect(panel->window, &client);
    int limit = maximum(panel->content_height - client.bottom, 0);
    if (position > limit) position = limit;
    if (position < 0) position = 0;
    int delta = panel->scroll - position;
    if (!delta) return;
    panel->scroll = position;
    ScrollWindowEx(panel->window, 0, delta, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    update_scroll_bar(panel);
    UpdateWindow(panel->window);
}

/* Scrolls the tab so the control with the keyboard focus can be seen. */
static void ensure_visible(HWND control)
{
    for (int i = 0; i < 2; i++)
    {
        Panel *panel = &W.panels[i];
        if (!control || GetParent(control) != panel->window) continue;
        RECT rect, client;
        GetWindowRect(control, &rect);
        MapWindowPoints(NULL, panel->window, (POINT *)&rect, 2);
        GetClientRect(panel->window, &client);
        if (rect.top < S(16)) scroll_panel(panel, panel->scroll + rect.top - S(16));
        else if (rect.bottom > client.bottom - S(16)) scroll_panel(panel, panel->scroll + rect.bottom - client.bottom + S(16));
    }
}

static void paint_panel(Panel *panel)
{
    PAINTSTRUCT paint;
    RECT client;
    const Theme *theme = ui_theme();
    HDC target = BeginPaint(panel->window, &paint);
    GetClientRect(panel->window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    HBRUSH page = CreateSolidBrush(theme->surface[SURFACE_PAGE]);
    FillRect(dc, &client, page);
    DeleteObject(page);
    for (int i = 0; i < panel->decor_count; i++)
    {
        const Decor *decor = &panel->decor[i];
        RECT rect = decor->rect;
        OffsetRect(&rect, 0, -panel->scroll);
        if (rect.bottom < paint.rcPaint.top || rect.top > paint.rcPaint.bottom) continue;
        switch (decor->kind)
        {
        case DECOR_CARD:
            ui_round_rect(dc, &rect, S(8), theme->surface[SURFACE_CARD], theme->card_stroke, true);
            break;
        case DECOR_INFO:
            ui_round_rect(dc, &rect, S(6), theme->surface[SURFACE_INFO + decor->detail], theme->card_stroke, true);
            ui_severity_icon(dc, rect.left + S(16), rect.top + S(13) + (ui_line_height(FONT_STRONG) - S(18)) / 2, S(18), (Severity)decor->detail);
            break;
        case DECOR_DOT:
            ui_circle(dc, rect.left, rect.top, rect.right - rect.left, (COLORREF)decor->detail);
            break;
        case DECOR_FIELD:
            ui_round_rect(dc, &rect, S(4), theme->field,
                GetFocus() == decor->control ? theme->accent : IsWindowEnabled(decor->control) ? theme->field_stroke : theme->control_stroke, true);
            break;
        }
    }
    BitBlt(target, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left,
        paint.rcPaint.bottom - paint.rcPaint.top, dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(panel->window, &paint);
}

static LRESULT CALLBACK panel_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    Panel *panel = &W.panels[GetWindowLongPtrW(window, GWLP_USERDATA)];
    switch (message)
    {
    case WM_SIZE:
        if (panel->window) layout_panel((int)(panel - W.panels));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_panel(panel);
        return 0;
    case WM_VSCROLL:
    {
        RECT client;
        SCROLLINFO info = { sizeof(info), SIF_TRACKPOS };
        GetClientRect(window, &client);
        GetScrollInfo(window, SB_VERT, &info);
        switch (LOWORD(wparam))
        {
        case SB_LINEUP: scroll_panel(panel, panel->scroll - S(48)); break;
        case SB_LINEDOWN: scroll_panel(panel, panel->scroll + S(48)); break;
        case SB_PAGEUP: scroll_panel(panel, panel->scroll - client.bottom); break;
        case SB_PAGEDOWN: scroll_panel(panel, panel->scroll + client.bottom); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: scroll_panel(panel, info.nTrackPos); break;
        case SB_TOP: scroll_panel(panel, 0); break;
        case SB_BOTTOM: scroll_panel(panel, panel->content_height); break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL || lines > 20) lines = 10;
        scroll_panel(panel, panel->scroll - MulDiv(GET_WHEEL_DELTA_WPARAM(wparam), (int)lines * S(28), WHEEL_DELTA));
        return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
        return ui_on_ctlcolor(message, (HDC)wparam, (HWND)lparam);
    case WM_COMMAND: case WM_NOTIFY:
        return SendMessageW(GetParent(window), message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* ---- Settings ---- */

static bool read_interval(int *value)
{
    wchar_t text[16];
    get_text(W.interval, text, COUNTOF(text), true);
    if (!text[0]) return false;
    int result = 0;
    for (const wchar_t *p = text; *p; p++)
    {
        if (*p < L'0' || *p > L'9' || result > 100000) return false;
        result = result * 10 + (*p - L'0');
    }
    *value = result < 1 ? 1 : result > 168 ? 168 : result;
    return true;
}

static void refresh_schedule_text(void)
{
    wchar_t text[200], when[120];
    if (!W.settings->schedule_enabled) str_copy(text, COUNTOF(text), L"Schedule is off. You can check for updates at any time.");
    else if (W.settings->next_run.has)
    {
        time_format_local(W.settings->next_run.ticks, L"ddd, MMM d", L" at ", L"h:mm tt", when, COUNTOF(when));
        str_fmt(text, COUNTOF(text), L"Next scheduled check: %s", when);
    }
    else str_copy(text, COUNTOF(text), L"Scheduled checks are enabled.");
    set_text(W.schedule_detail, text);
    set_text(W.next_run_text, busy() ? L"Update check in progress" : text);
}

/* The key is only used to ask Pulsarr whether it is busy before an automatic install.
   A saved key keeps the panel visible so it can be changed or cleared. */
static void refresh_pulsarr_key_panel(void)
{
    const ProgramSettings *pulsarr = program(APP_PULSARR);
    bool visible = pulsarr->enabled && pulsarr->auto_install && pulsarr->wait_while_in_use &&
        (W.pulsarr_needs_key || pulsarr->api_key[0]);
    show(W.pulsarr_key_title, visible);
    show(W.pulsarr_key_description, visible);
    show(W.pulsarr_key, visible);
    show(W.pulsarr_key_show, visible);
    show(W.find_pulsarr_key, visible);
    show(W.pulsarr_key_status, visible);
}

static void refresh_preferences(void)
{
    wchar_t text[80];
    bool daily = SendMessageW(W.schedule_mode, CB_GETCURSEL, 0, 0) == 1;
    bool schedule = is_checked(W.schedule_toggle) && !busy();
    int count = 0;
    enable(W.schedule_mode, schedule);
    enable(W.interval, schedule);
    enable(W.interval_down, schedule);
    enable(W.interval_up, schedule);
    enable(W.hour, schedule);
    enable(W.minute, schedule);
    show(W.interval_label, !daily);
    show(W.interval, !daily);
    show(W.interval_down, !daily);
    show(W.interval_up, !daily);
    show(W.time_label, daily);
    show(W.hour, daily);
    show(W.minute, daily);
    enable(W.start_minimized, is_checked(W.tray_toggle));
    set_text(W.background_text, is_checked(W.tray_toggle) ? L"Close to tray is on" : L"Closing the window exits the app");
    for (int i = 0; i < APP_COUNT; i++)
    {
        if (W.rows[i].settings->enabled) count++;
        refresh_row(&W.rows[i]);
    }
    str_fmt(text, COUNTOF(text), L"%d of %d monitored", count, APP_COUNT);
    set_text(W.monitored_text, text);
    refresh_pulsarr_key_panel();
    refresh_schedule_text();
    refresh_app_update_controls();
    InvalidateRect(W.panels[1].window, NULL, FALSE);
}

static bool save_settings(bool save_connections)
{
    static const wchar_t *const themes[] = { L"Dark", L"Light", L"System" };
    if (W.initializing) return true;
    AppSettings *settings = W.settings;
    wchar_t error[MSGN], text[MSGN];
    int interval = 0;
    bool has_interval = read_interval(&interval);
    bool schedule = is_checked(W.schedule_toggle);
    ScheduleMode mode = SendMessageW(W.schedule_mode, CB_GETCURSEL, 0, 0) == 1 ? SCHEDULE_DAILY : SCHEDULE_INTERVAL;
    int hour = (int)SendMessageW(W.hour, CB_GETCURSEL, 0, 0), minute = (int)SendMessageW(W.minute, CB_GETCURSEL, 0, 0);
    int theme = (int)SendMessageW(W.theme, CB_GETCURSEL, 0, 0);
    if (hour < 0) hour = settings->daily_hour;
    if (minute < 0) minute = settings->daily_minute;
    bool schedule_changed = settings->schedule_enabled != schedule || settings->schedule_mode != mode ||
        (has_interval && settings->interval_hours != interval) || settings->daily_hour != hour || settings->daily_minute != minute;

    str_copy(settings->theme, COUNTOF(settings->theme), themes[theme < 0 || theme > 2 ? 0 : theme]);
    settings->schedule_enabled = schedule;
    settings->schedule_mode = mode;
    if (has_interval) settings->interval_hours = interval;
    settings->daily_hour = hour;
    settings->daily_minute = minute;
    settings->minimize_to_tray = is_checked(W.tray_toggle);
    settings->start_minimized = is_checked(W.start_minimized);
    settings->app_update_check = is_checked(W.app_update_check_toggle);
    settings->app_update_install = is_checked(W.app_update_install_toggle);
    if (save_connections)
    {
        get_text(W.radarr_url, program(APP_RADARR)->server_url, URLN, true);
        get_text(W.radarr_key, program(APP_RADARR)->api_key, KEYN, true);
        get_text(W.sonarr_url, program(APP_SONARR)->server_url, URLN, true);
        get_text(W.sonarr_key, program(APP_SONARR)->api_key, KEYN, true);
    }
    bool ok = !schedule_changed || coordinator_recalculate_schedule(W.coordinator, error, COUNTOF(error));
    ok = ok && store_save(W.store, settings, error, COUNTOF(error));
    if (ok) set_text(W.settings_status, L"Preferences saved.");
    else
    {
        str_fmt(text, COUNTOF(text), L"Could not save preferences: %s", error);
        set_text(W.settings_status, text);
        show_summary(L"Preferences could not be saved", error, NOTICE_ERROR);
    }
    return ok;
}

static void preference_changed(void)
{
    if (W.initializing) return;
    save_settings(false);
    refresh_preferences();
}

static void apply_theme(void)
{
    bool dark = wcscmp(W.settings->theme, L"Light") == 0 ? false
        : wcscmp(W.settings->theme, L"System") == 0 ? ui_system_uses_dark() : true;
    BOOL value = dark;
    ui_set_theme(dark);
    DwmSetWindowAttribute(W.window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value));
    for (int i = 0; i < 2; i++) ui_theme_scrollbars(W.panels[i].window);
    ui_refresh_controls(W.window);
}

static void set_busy(bool is_busy)
{
    bool connection_controls = !is_busy && !W.retrieving_keys;
    bool schedule = !is_busy && W.settings->schedule_enabled;
    enable(W.check, connection_controls);
    enable(W.install, connection_controls);
    enable(W.schedule_check_now, connection_controls);
    ui_spinner_set(W.busy_ring, is_busy);
    enable(W.save_connections, connection_controls);
    enable(W.find_keys, connection_controls);
    enable(W.radarr_url, connection_controls);
    enable(W.radarr_key, connection_controls);
    enable(W.sonarr_url, connection_controls);
    enable(W.sonarr_key, connection_controls);
    enable(W.pulsarr_key, connection_controls);
    enable(W.find_pulsarr_key, connection_controls);
    enable(W.schedule_toggle, !is_busy);
    enable(W.schedule_mode, schedule);
    enable(W.interval, schedule);
    enable(W.interval_down, schedule);
    enable(W.interval_up, schedule);
    enable(W.hour, schedule);
    enable(W.minute, schedule);
    enable(W.elevate, connection_controls);
    enable(W.startup, !is_busy);
    for (int i = 0; i < APP_COUNT; i++)
    {
        W.rows[i].editable = !is_busy;
        refresh_row(&W.rows[i]);
    }
    refresh_schedule_text();
    refresh_app_update_controls();
    InvalidateRect(W.panels[1].window, NULL, FALSE);
}

/* ---- Running checks and installs ---- */

typedef struct RunJob { RunSnapshot snapshot; bool install, scheduled; } RunJob;

static void post_copy(UINT message, WPARAM wparam, const void *data, size_t size)
{
    void *copy = xalloc(size);
    memcpy(copy, data, size);
    if (!PostMessageW(W.window, message, wparam, (LPARAM)copy)) free(copy);
}

static void on_result(void *context, const UpdateResult *result) { post_copy(WM_APP_RESULT, 0, result, sizeof(*result)); }
static void on_activity(void *context, const ActivityEntry *entry) { post_copy(WM_APP_ACTIVITY, 0, entry, sizeof(*entry)); }
static void on_busy(void *context, bool is_busy) { PostMessageW(W.window, WM_APP_BUSY, is_busy, 0); }
static void on_summary(void *context, const wchar_t *summary, bool finished)
{
    post_copy(WM_APP_SUMMARY, finished, summary, (wcslen(summary) + 1) * sizeof(wchar_t));
}

static DWORD WINAPI run_thread(LPVOID parameter)
{
    RunJob *job = parameter;
    int waiting = coordinator_execute(W.coordinator, &job->snapshot, job->install);
    PostMessageW(W.window, WM_APP_RUN_DONE, (WPARAM)waiting, job->scheduled);
    free(job);
    return 0;
}

static void start_run(bool install, bool scheduled)
{
    RunJob *job = xalloc(sizeof(RunJob));
    job->install = install;
    job->scheduled = scheduled;
    if (!coordinator_begin(W.coordinator, &job->snapshot))
    {
        free(job);
        return;
    }
    set_busy(true);
    start_thread(run_thread, job);
}

static void run_updates(bool install)
{
    if (busy() || W.retrieving_keys || W.exiting) return;
    if (W.app_update_busy)
    {
        show_summary(L"Please wait", L"Media Center Updater is checking for or installing a new version of itself. Try again in a moment.", NOTICE_INFO);
        return;
    }
    if (!save_settings(true)) return;
    bool any = false;
    for (int i = 0; i < W.settings->program_count; i++) any = any || W.settings->programs[i].enabled;
    if (!any)
    {
        show_summary(L"Choose an application", L"Turn on Monitor for at least one app before checking for updates.", NOTICE_WARNING);
        return;
    }
    start_run(install, false);
}

/* ---- Finding API keys and checking Pulsarr's setup ---- */

typedef struct DiscoveryJob
{
    bool replace, has_radarr, has_sonarr, has_pulsarr_key, pulsarr_needs_key;
    wchar_t radarr_url[URLN], sonarr_url[URLN], radarr_key[KEYN], sonarr_key[KEYN], pulsarr_key[KEYN];
    KeyDiscovery radarr, sonarr, pulsarr_found;
    PulsarrSetup pulsarr;
} DiscoveryJob;

static DWORD WINAPI discovery_thread(LPVOID parameter)
{
    DiscoveryJob *job = parameter;
    check_pulsarr_setup(&job->pulsarr);
    job->pulsarr_needs_key = job->pulsarr.supported && pulsarr_requires_api_key(NULL);
    job->has_radarr = job->replace || str_blank(job->radarr_key);
    if (job->has_radarr) discover_api_key(APP_RADARR, job->radarr_url, NULL, &job->radarr);
    job->has_sonarr = job->replace || str_blank(job->sonarr_key);
    if (job->has_sonarr) discover_api_key(APP_SONARR, job->sonarr_url, NULL, &job->sonarr);
    job->has_pulsarr_key = job->pulsarr_needs_key && (job->replace || str_blank(job->pulsarr_key));
    if (job->has_pulsarr_key) discover_pulsarr_api_key(NULL, &job->pulsarr_found);
    if (!PostMessageW(W.window, WM_APP_DISCOVERY, 0, (LPARAM)job)) free(job);
    return 0;
}

static void refresh_detected_setup(bool replace_existing_keys)
{
    if (W.retrieving_keys || busy() || W.exiting) return;
    W.retrieving_keys = true;
    set_busy(busy());
    ui_spinner_set(W.key_search_ring, true);
    set_text(W.key_search_status, L"Looking in this PC's application settings...");
    DiscoveryJob *job = xalloc(sizeof(DiscoveryJob));
    job->replace = replace_existing_keys;
    get_text(W.radarr_url, job->radarr_url, URLN, true);
    get_text(W.sonarr_url, job->sonarr_url, URLN, true);
    get_text(W.radarr_key, job->radarr_key, KEYN, false);
    get_text(W.sonarr_key, job->sonarr_key, KEYN, false);
    get_text(W.pulsarr_key, job->pulsarr_key, KEYN, false);
    start_thread(discovery_thread, job);
}

static bool apply_pulsarr_key(const KeyDiscovery *result, const wchar_t *original_key)
{
    wchar_t current[KEYN];
    get_text(W.pulsarr_key, current, KEYN, false);
    if (wcscmp(current, original_key) != 0)
    {
        set_text(W.pulsarr_key_status, L"Your latest change was kept. Choose Find key again to replace it.");
        return false;
    }
    set_text(W.pulsarr_key_status, result->message);
    if (!result->found) return false;
    /* Set the saved key first so the box's change notification sees nothing new to save. */
    str_copy(program(APP_PULSARR)->api_key, KEYN, result->api_key);
    SetWindowTextW(W.pulsarr_key, result->api_key);
    return true;
}

static bool apply_discovered_key(ApplicationId id, const KeyDiscovery *result, HWND address_box, HWND key_box, HWND status,
    const wchar_t *original_address, const wchar_t *original_key)
{
    const wchar_t *name = app_name(id);
    wchar_t address[URLN], key[KEYN], text[MSGN + 64];
    get_text(address_box, address, URLN, true);
    get_text(key_box, key, KEYN, false);
    if (wcscmp(address, original_address) != 0 || wcscmp(key, original_key) != 0)
    {
        str_fmt(text, COUNTOF(text), L"%s: Your latest changes were kept. Choose Find API keys again to search with the new details.", name);
        set_text(status, text);
        return false;
    }
    if (!result)
    {
        str_fmt(text, COUNTOF(text), L"%s: A saved key is already filled in. Find API keys again can refresh it from the app's settings.", name);
        set_text(status, text);
        return false;
    }
    str_fmt(text, COUNTOF(text), L"%s: %s", name, result->message);
    set_text(status, text);
    if (!result->found) return false;
    SetWindowTextW(key_box, result->api_key);
    str_copy(program(id)->server_url, URLN, original_address);
    str_copy(program(id)->api_key, KEYN, result->api_key);
    return true;
}

static void discovery_finished(DiscoveryJob *job)
{
    wchar_t error[MSGN], text[160];
    int filled = 0;
    bool saved = true;
    if (apply_discovered_key(APP_RADARR, job->has_radarr ? &job->radarr : NULL, W.radarr_url, W.radarr_key, W.radarr_key_status,
            job->radarr_url, job->radarr_key))
        filled++;
    if (apply_discovered_key(APP_SONARR, job->has_sonarr ? &job->sonarr : NULL, W.sonarr_url, W.sonarr_key, W.sonarr_key_status,
            job->sonarr_url, job->sonarr_key))
        filled++;
    if (job->has_pulsarr_key && apply_pulsarr_key(&job->pulsarr_found, job->pulsarr_key)) filled++;
    info_set(&W.pulsarr_setup, job->pulsarr.supported ? L"This PC meets Pulsarr's requirement" : L"Pulsarr setup needs attention",
        job->pulsarr.message, job->pulsarr.supported ? NOTICE_SUCCESS : NOTICE_WARNING);
    W.pulsarr_needs_key = job->pulsarr_needs_key;
    refresh_pulsarr_key_panel();

    /* Save only the retrieved connections, not other edits still waiting in the form. */
    if (filled > 0) saved = store_save(W.store, W.settings, error, COUNTOF(error));
    if (!saved)
        set_text(W.key_search_status, L"The setup check or saving could not finish. Any keys already shown have been kept. Try Find API keys again, or use Save connections.");
    else if (filled == 1) set_text(W.key_search_status, L"One API key was found, filled in, and saved securely. See each app's result.");
    else if (filled > 1)
    {
        str_fmt(text, COUNTOF(text), L"%d API keys were found, filled in, and saved securely.", filled);
        set_text(W.key_search_status, text);
    }
    else
        set_text(W.key_search_status, job->replace
            ? L"Search finished. No replacement keys were found. Any keys already entered have been kept."
            : L"Setup check finished. Any saved or manually entered keys have been kept. Use Find API keys again to refresh them.");
    W.retrieving_keys = false;
    ui_spinner_set(W.key_search_ring, false);
    set_busy(busy());
}

typedef struct PulsarrKeyJob { wchar_t original_key[KEYN]; KeyDiscovery found; } PulsarrKeyJob;

static DWORD WINAPI pulsarr_key_thread(LPVOID parameter)
{
    PulsarrKeyJob *job = parameter;
    discover_pulsarr_api_key(NULL, &job->found);
    if (!PostMessageW(W.window, WM_APP_PULSARR_KEY, 0, (LPARAM)job)) free(job);
    return 0;
}

static void find_pulsarr_key(void)
{
    if (W.retrieving_keys || busy() || W.exiting) return;
    W.retrieving_keys = true;
    set_busy(busy());
    set_text(W.pulsarr_key_status, L"Looking in Pulsarr's database on this PC...");
    PulsarrKeyJob *job = xalloc(sizeof(PulsarrKeyJob));
    get_text(W.pulsarr_key, job->original_key, KEYN, false);
    start_thread(pulsarr_key_thread, job);
}

static void pulsarr_key_finished(PulsarrKeyJob *job)
{
    wchar_t error[MSGN];
    /* Never show database details or key values. */
    if (apply_pulsarr_key(&job->found, job->original_key) && !store_save(W.store, W.settings, error, COUNTOF(error)))
        set_text(W.pulsarr_key_status, L"The key could not be saved. Try Find key again.");
    W.retrieving_keys = false;
    set_busy(busy());
}

/* ---- Updates to this app itself ---- */

static void refresh_app_update_controls(void)
{
    bool installed = self_update_is_installed_copy();
    wchar_t text[VERN + 32];
    enable(W.app_update_check_toggle, installed);
    enable(W.app_update_install_toggle, installed && is_checked(W.app_update_check_toggle));
    enable(W.app_update_check, !W.app_update_busy);
    ui_spinner_set(W.app_update_ring, W.app_update_busy);
    show(W.app_update_install, W.has_app_update);
    if (!installed) str_copy(text, COUNTOF(text), L"Open download page");
    else if (W.has_app_update) str_fmt(text, COUNTOF(text), L"Install version %s", W.app_update.version);
    else str_copy(text, COUNTOF(text), L"Install update");
    set_text(W.app_update_install, text);
    enable(W.app_update_install, !W.app_update_busy && (!installed || (!busy() && !W.retrieving_keys)));
}

typedef struct UpdateCheckJob { bool automatic; SelfUpdateCheck result; SelfUpdateInfo info; wchar_t error[MSGN]; } UpdateCheckJob;

static DWORD WINAPI update_check_thread(LPVOID parameter)
{
    UpdateCheckJob *job = parameter;
    job->result = self_update_check(NULL, NULL, VERSION_TEXT, &job->info, job->error, COUNTOF(job->error));
    if (!PostMessageW(W.window, WM_APP_UPDATE_CHECKED, 0, (LPARAM)job)) free(job);
    return 0;
}

static void check_app_update(bool automatic)
{
    if (W.app_update_busy || W.exiting) return;
    W.app_update_busy = true;
    refresh_app_update_controls();
    set_text(W.app_update_status, L"Checking for a new version...");
    UpdateCheckJob *job = xalloc(sizeof(UpdateCheckJob));
    job->automatic = automatic;
    start_thread(update_check_thread, job);
}

static void maybe_install_app_update(void)
{
    if (W.has_app_update && W.settings->app_update_install && !busy() && !W.retrieving_keys && !W.dialog_open &&
        time_now() >= W.next_app_update_attempt)
        install_app_update(true);
}

static void update_check_finished(UpdateCheckJob *job)
{
    wchar_t text[MSGN + 160], when[80], error[MSGN];
    W.app_update_checked = true;
    W.app_update_busy = false;
    if (job->result == SELF_UPDATE_FAILED)
    {
        W.next_app_update_attempt = time_now() + TICKS_PER_HOUR;
        str_fmt(text, COUNTOF(text), L"Could not check for a new version. %s%s", job->error, job->automatic ? L" It will try again in an hour." : L"");
        set_text(W.app_update_status, text);
    }
    else
    {
        W.settings->last_app_update_check = dto_now();
        store_save(W.store, W.settings, error, COUNTOF(error));
        bool found = job->result == SELF_UPDATE_FOUND;
        bool is_new = found && (!W.has_app_update || wcscmp(W.app_update.version, job->info.version) != 0);
        W.has_app_update = found;
        if (found) W.app_update = job->info;
        if (!found)
        {
            format_time(time_now(), when, COUNTOF(when));
            str_fmt(text, COUNTOF(text), L"You have the latest version (%s). Checked %s.", VERSION_TEXT, when);
            set_text(W.app_update_status, text);
        }
        else
        {
            bool installed = self_update_is_installed_copy();
            str_fmt(text, COUNTOF(text), L"Version %s is available. You have %s.", job->info.version, VERSION_TEXT);
            set_text(W.app_update_status, text);
            if (is_new)
            {
                str_fmt(text, COUNTOF(text), L"Version %s is available.", job->info.version);
                log_app_activity(text, false);
                if (!(job->automatic && W.settings->app_update_install && installed))
                {
                    if (installed)
                        str_fmt(text, COUNTOF(text), L"Version %s is ready. Open Settings and choose Install version %s under Auto-update.",
                            job->info.version, job->info.version);
                    else
                        str_fmt(text, COUNTOF(text), L"Version %s is ready. Open Settings and choose Open download page under Auto-update.",
                            job->info.version);
                    show_summary(L"A new version of Media Center Updater is available", text, NOTICE_INFO);
                }
            }
        }
    }
    refresh_app_update_controls();
    if (job->automatic) maybe_install_app_update();
}

/* Called by the 30-second timer. Checks once per app start and then daily, and installs when allowed. */
static void evaluate_app_update(void)
{
    if (W.app_update_busy || W.exiting || !self_update_is_installed_copy() || !W.settings->app_update_check) return;
    if (time_now() < W.next_app_update_attempt) return;
    if (!W.has_app_update)
    {
        if (W.app_update_checked && W.settings->last_app_update_check.has &&
            time_now() - W.settings->last_app_update_check.ticks < TICKS_PER_DAY)
            return;
        check_app_update(true);
        return;
    }
    maybe_install_app_update();
}

typedef struct UpdateDownloadJob { bool automatic, ok; SelfUpdateInfo info; wchar_t data_directory[PATHN], file[PATHN], error[MSGN]; } UpdateDownloadJob;

static DWORD WINAPI update_download_thread(LPVOID parameter)
{
    UpdateDownloadJob *job = parameter;
    job->ok = self_update_download(NULL, NULL, &job->info, job->data_directory, job->file, COUNTOF(job->file), job->error, COUNTOF(job->error));
    if (!PostMessageW(W.window, WM_APP_UPDATE_DOWNLOADED, 0, (LPARAM)job)) free(job);
    return 0;
}

static void install_app_update(bool automatic)
{
    if (!W.has_app_update || W.app_update_busy || W.exiting) return;
    if (!self_update_is_installed_copy())
    {
        if ((INT_PTR)ShellExecuteW(W.window, L"open", SELF_UPDATE_RELEASES_PAGE, NULL, NULL, SW_SHOWNORMAL) <= 32)
            show_notice(L"The download page could not be opened", L"Open " SELF_UPDATE_RELEASES_PAGE L" in your browser.");
        return;
    }
    if (busy() || W.retrieving_keys)
    {
        set_text(W.app_update_status, L"Wait for the current check or installation to finish, then try again.");
        return;
    }
    if (!save_settings(true)) return;
    wchar_t text[VERN + 64];
    W.app_update_busy = true;
    refresh_app_update_controls();
    str_fmt(text, COUNTOF(text), L"Downloading version %s...", W.app_update.version);
    set_text(W.app_update_status, text);
    UpdateDownloadJob *job = xalloc(sizeof(UpdateDownloadJob));
    job->automatic = automatic;
    job->info = W.app_update;
    str_copy(job->data_directory, COUNTOF(job->data_directory), W.store->data_directory);
    start_thread(update_download_thread, job);
}

static void app_update_failed(const wchar_t *reason, bool automatic)
{
    wchar_t text[MSGN + 160];
    W.next_app_update_attempt = time_now() + TICKS_PER_HOUR;
    str_fmt(text, COUNTOF(text), L"Version %s could not be installed. %s%s", W.app_update.version, reason,
        automatic ? L" It will try again in an hour." : L"");
    set_text(W.app_update_status, text);
    str_fmt(text, COUNTOF(text), L"Version %s could not be installed. %s", W.app_update.version, reason);
    log_app_activity(text, true);
    show_summary(L"Media Center Updater could not update itself", reason, NOTICE_WARNING);
    W.app_update_busy = false;
    refresh_app_update_controls();
}

static void close_window(void)
{
    W.exiting = true;
    tray_remove(&W.tray);
    DestroyWindow(W.window);
}

static void start_app_installer(void)
{
    wchar_t error[MSGN];
    if (!self_update_start_installer(W.installer_path, W.app_update.sha256, W.install_in_tray, error, COUNTOF(error)))
    {
        app_update_failed(error, false);
        return;
    }
    close_window();
}

static void update_download_finished(UpdateDownloadJob *job)
{
    wchar_t text[VERN + 160];
    if (!job->ok)
    {
        app_update_failed(job->error, job->automatic);
        return;
    }
    if (busy())
    {
        /* An app update started during the download. Try again once it has finished. */
        W.next_app_update_attempt = time_now() + 5 * TICKS_PER_MINUTE;
        str_fmt(text, COUNTOF(text), L"Version %s is downloaded. It will install after the current app updates finish.", W.app_update.version);
        set_text(W.app_update_status, text);
        W.app_update_busy = false;
        refresh_app_update_controls();
        return;
    }
    str_copy(W.installer_path, COUNTOF(W.installer_path), job->file);
    W.install_in_tray = W.hidden_to_tray;
    str_fmt(text, COUNTOF(text), L"Installing version %s. Media Center Updater will close and reopen.", W.app_update.version);
    log_app_activity(text, false);
    if (W.install_in_tray)
    {
        start_app_installer();
        return;
    }
    /* Give the user a moment to read what is about to happen before the window closes. */
    str_fmt(text, COUNTOF(text), L"Installing version %s. Media Center Updater will close, show the setup progress, then reopen.", W.app_update.version);
    set_text(W.app_update_status, text);
    str_fmt(text, COUNTOF(text), L"Installing version %s", W.app_update.version);
    show_summary(text, L"Media Center Updater will close in a moment. A setup window shows the progress, and the app reopens when it is done.", NOTICE_INFO);
    SetTimer(W.window, TIMER_INSTALL, 2000, NULL);
}

/* ---- Window visibility and exit ---- */

static void show_window(void)
{
    ShowWindow(W.window, SW_SHOW);
    if (IsIconic(W.window)) ShowWindow(W.window, SW_RESTORE);
    SetForegroundWindow(W.window);
    W.hidden_to_tray = false;
}

static void hide_to_tray(void)
{
    if (W.hidden_to_tray || !W.tray.added) return;
    W.hidden_to_tray = true;
    ShowWindow(W.window, SW_HIDE);
}

static void request_exit(void)
{
    if (busy())
    {
        show_window();
        show_notice(L"An update operation is running", L"Wait for the current check or installation to finish before exiting. You can minimize the app while it works.");
        return;
    }
    if (!save_settings(true)) return;
    close_window();
}

static void on_close(void)
{
    if (W.exiting)
    {
        DestroyWindow(W.window);
        return;
    }
    bool to_tray = W.settings->minimize_to_tray && W.tray.added;
    if (busy() && !to_tray)
    {
        show_notice(L"An update operation is running", L"Wait for the current check or installation to finish before exiting.");
        return;
    }
    if (!busy()) save_settings(true);
    if (to_tray) hide_to_tray();
    else close_window();
}

static void show_tray_menu(void)
{
    POINT cursor;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Media Center Updater");
    AppendMenuW(menu, MF_STRING, ID_TRAY_CHECK, L"Check for updates now");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);
    GetCursorPos(&cursor);
    /* Without this the menu does not close when the user clicks elsewhere. */
    SetForegroundWindow(W.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0, W.window, NULL);
    PostMessageW(W.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ---- Events ---- */

static void schedule_tick(void)
{
    wchar_t error[MSGN];
    if (W.exiting) return;
    evaluate_app_update();
    if (W.exiting || W.app_update_busy || W.retrieving_keys || busy() || !W.settings->schedule_enabled) return;
    switch (coordinator_schedule_due(W.coordinator, error, COUNTOF(error)))
    {
    case SCHEDULE_DUE: start_run(true, true); break;
    case SCHEDULE_SAVE_FAILED: show_summary(L"Scheduled check could not finish", error, NOTICE_ERROR); break;
    default: break;
    }
    refresh_schedule_text();
}

static void toggle_reveal(HWND edit, HWND button)
{
    bool hidden = SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0) != 0;
    SendMessageW(edit, EM_SETPASSWORDCHAR, hidden ? 0 : 0x25CF, 0);
    InvalidateRect(edit, NULL, TRUE);
    set_text(button, hidden ? L"Hide" : L"Show");
}

static void change_interval(int delta)
{
    wchar_t text[16];
    int value = W.settings->interval_hours;
    read_interval(&value);
    value += delta;
    str_fmt(text, COUNTOF(text), L"%d", value < 1 ? 1 : value > 168 ? 168 : value);
    SetWindowTextW(W.interval, text);
}

static void select_tab(int tab)
{
    W.tab = tab;
    for (int i = 0; i < 2; i++)
    {
        ui_set_selected(W.tabs[i], i == tab);
        ShowWindow(W.panels[i].window, i == tab ? SW_SHOW : SW_HIDE);
    }
}

static void on_startup_clicked(void)
{
    wchar_t error[MSGN];
    bool enabled = is_checked(W.startup);
    if (startup_set(enabled, error, COUNTOF(error)))
    {
        set_text(W.settings_status, enabled ? L"Media Center Updater will start when you sign in." : L"Sign-in startup is off.");
        return;
    }
    wchar_t ignored[MSGN];
    set_checked(W.startup, startup_state(ignored, COUNTOF(ignored)) == STARTUP_ON);
    set_text(W.settings_status, error);
    show_notice(L"Startup setting could not be changed", error);
}

static void on_command(int id, int code, HWND control)
{
    wchar_t text[KEYN], error[MSGN];
    if (id >= ID_MONITOR && id < ID_MONITOR + APP_COUNT && code == BN_CLICKED)
    {
        W.rows[id - ID_MONITOR].settings->enabled = is_checked(control);
        preference_changed();
        return;
    }
    if (id >= ID_AUTO_INSTALL && id < ID_AUTO_INSTALL + APP_COUNT && code == BN_CLICKED)
    {
        W.rows[id - ID_AUTO_INSTALL].settings->auto_install = is_checked(control);
        preference_changed();
        return;
    }
    if (id >= ID_USAGE && id < ID_USAGE + APP_COUNT && code == BN_CLICKED)
    {
        W.rows[id - ID_USAGE].settings->wait_while_in_use = is_checked(control);
        preference_changed();
        return;
    }
    /* Text boxes repaint their frame when the keyboard focus enters or leaves. */
    if (code == EN_SETFOCUS || code == EN_KILLFOCUS)
    {
        switch (id)
        {
        case ID_INTERVAL: case ID_PULSARR_KEY: case ID_RADARR_URL: case ID_RADARR_KEY: case ID_SONARR_URL: case ID_SONARR_KEY:
            InvalidateRect(GetParent(control), NULL, FALSE);
            if (id == ID_INTERVAL && code == EN_KILLFOCUS && !W.initializing)
            {
                str_fmt(text, 16, L"%d", W.settings->interval_hours);
                ui_set_text(W.interval, text);
            }
            return;
        }
    }
    switch (id)
    {
    case ID_CHECK: case ID_TRAY_CHECK: run_updates(false); break;
    case ID_INSTALL: case ID_SCHEDULE_CHECK_NOW: run_updates(true); break;
    case ID_TAB_UPDATES: select_tab(0); break;
    case ID_TAB_SETTINGS: select_tab(1); break;
    case ID_OPEN_LOGS:
        if ((INT_PTR)ShellExecuteW(W.window, L"open", W.store->data_directory, NULL, NULL, SW_SHOWNORMAL) <= 32)
            show_notice(L"Log folder could not be opened", W.store->data_directory);
        break;
    case ID_SCHEDULE_TOGGLE: case ID_TRAY: case ID_START_MINIMIZED:
        if (code == BN_CLICKED) preference_changed();
        break;
    case ID_SCHEDULE_MODE: case ID_HOUR: case ID_MINUTE:
        if (code == CBN_SELCHANGE) preference_changed();
        break;
    case ID_INTERVAL:
        if (code == EN_CHANGE) preference_changed();
        break;
    case ID_INTERVAL_DOWN: change_interval(-1); break;
    case ID_INTERVAL_UP: change_interval(1); break;
    case ID_THEME:
        if (code == CBN_SELCHANGE && !W.initializing)
        {
            save_settings(false);
            apply_theme();
        }
        break;
    case ID_PULSARR_KEY:
        if (code == EN_CHANGE && !W.initializing)
        {
            get_text(W.pulsarr_key, text, KEYN, true);
            if (wcscmp(program(APP_PULSARR)->api_key, text) != 0)
            {
                str_copy(program(APP_PULSARR)->api_key, KEYN, text);
                save_settings(false);
            }
        }
        break;
    case ID_PULSARR_KEY_SHOW: toggle_reveal(W.pulsarr_key, W.pulsarr_key_show); break;
    case ID_RADARR_KEY_SHOW: toggle_reveal(W.radarr_key, W.radarr_key_show); break;
    case ID_SONARR_KEY_SHOW: toggle_reveal(W.sonarr_key, W.sonarr_key_show); break;
    case ID_FIND_PULSARR_KEY: find_pulsarr_key(); break;
    case ID_STARTUP:
        if (code == BN_CLICKED) on_startup_clicked();
        break;
    case ID_APP_UPDATE_CHECK_TOGGLE: case ID_APP_UPDATE_INSTALL_TOGGLE:
        if (code == BN_CLICKED && !W.initializing)
        {
            save_settings(false);
            refresh_app_update_controls();
            /* Turning on automatic install acts on an update that was already found. */
            if (W.has_app_update && W.settings->app_update_check && W.settings->app_update_install)
                W.next_app_update_attempt = time_now();
        }
        break;
    case ID_APP_UPDATE_CHECK: check_app_update(false); break;
    case ID_APP_UPDATE_INSTALL: install_app_update(false); break;
    case ID_FIND_KEYS: refresh_detected_setup(true); break;
    case ID_SAVE_CONNECTIONS:
        if (save_settings(true)) set_text(W.settings_status, L"Connection details saved. Check for updates to test them.");
        break;
    case ID_ELEVATE:
        if (busy() || !save_settings(true)) break;
        if (restart_elevated(W.hidden_to_tray, error, COUNTOF(error))) close_window();
        else show_notice(L"Administrator restart could not complete", error);
        break;
    case ID_EXIT: case ID_TRAY_EXIT: request_exit(); break;
    case ID_TRAY_OPEN: show_window(); break;
    }
}

static void paint_window(void)
{
    PAINTSTRUCT paint;
    RECT client;
    const Theme *theme = ui_theme();
    HDC target = BeginPaint(W.window, &paint);
    GetClientRect(W.window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    HBRUSH page = CreateSolidBrush(theme->surface[SURFACE_PAGE]), line = CreateSolidBrush(theme->card_stroke);
    FillRect(dc, &client, page);
    ui_logo(dc, S(32), S(28), S(52));
    int footer_height = S(12) * 2 + ui_line_height(FONT_CAPTION);
    RECT border = { 0, client.bottom - footer_height, client.right, client.bottom - footer_height + 1 };
    FillRect(dc, &border, line);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    DeleteObject(page);
    DeleteObject(line);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(W.window, &paint);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    LRESULT result;
    if (message == W.taskbar_created && W.taskbar_created && W.tray.icon)
    {
        /* Explorer restarted, so the notification area icon has to be added again. */
        W.tray.added = false;
        tray_add(&W.tray, window, W.tray.icon, WM_APP_TRAY);
        return 0;
    }
    switch (message)
    {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED)
        {
            if (!W.exiting && W.settings->minimize_to_tray) hide_to_tray();
        }
        else if (W.title)
        {
            layout_all();
            /* Windows only repaints the newly uncovered part after a resize, which would leave the footer
               line behind at its earlier height. */
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lparam)->ptMinTrackSize.x = S(860);
        ((MINMAXINFO *)lparam)->ptMinTrackSize.y = S(560);
        return 0;
    case WM_DPICHANGED:
    {
        const RECT *suggested = (const RECT *)lparam;
        ui_set_dpi(HIWORD(wparam));
        ui_refresh_controls(window);
        SetWindowPos(window, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_all();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lparam && wcscmp((const wchar_t *)lparam, L"ImmersiveColorSet") == 0 && wcscmp(W.settings->theme, L"System") == 0)
            apply_theme();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_window();
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
        return ui_on_ctlcolor(message, (HDC)wparam, (HWND)lparam);
    case WM_NOTIFY:
        if (ui_on_notify((const NMHDR *)lparam, &result)) return result;
        break;
    case WM_COMMAND:
        on_command(LOWORD(wparam), HIWORD(wparam), (HWND)lparam);
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_SCHEDULE) schedule_tick();
        else if (wparam == TIMER_INSTALL)
        {
            KillTimer(window, TIMER_INSTALL);
            if (!W.exiting) start_app_installer();
        }
        return 0;
    case WM_APP_LAYOUT:
        layout_all();
        return 0;
    case WM_APP_RESULT:
        if (!W.exiting) apply_result((const UpdateResult *)lparam);
        free((void *)lparam);
        return 0;
    case WM_APP_ACTIVITY:
        if (!W.exiting) add_activity((const ActivityEntry *)lparam);
        free((void *)lparam);
        return 0;
    case WM_APP_BUSY:
        if (!W.exiting) set_busy(wparam != 0);
        return 0;
    case WM_APP_SUMMARY:
        if (!W.exiting) show_summary(wparam ? L"Latest result" : L"Working on your applications", (const wchar_t *)lparam, NOTICE_INFO);
        free((void *)lparam);
        return 0;
    case WM_APP_RUN_DONE:
        if (W.exiting) return 0;
        if (lparam) coordinator_schedule_finished(W.coordinator, (int)wparam);
        set_busy(busy());
        refresh_schedule_text();
        return 0;
    case WM_APP_DISCOVERY:
        if (!W.exiting) discovery_finished((DiscoveryJob *)lparam);
        SecureZeroMemory((void *)lparam, sizeof(DiscoveryJob));
        free((void *)lparam);
        return 0;
    case WM_APP_PULSARR_KEY:
        if (!W.exiting) pulsarr_key_finished((PulsarrKeyJob *)lparam);
        SecureZeroMemory((void *)lparam, sizeof(PulsarrKeyJob));
        free((void *)lparam);
        return 0;
    case WM_APP_UPDATE_CHECKED:
        if (!W.exiting) update_check_finished((UpdateCheckJob *)lparam);
        free((void *)lparam);
        return 0;
    case WM_APP_UPDATE_DOWNLOADED:
        if (!W.exiting) update_download_finished((UpdateDownloadJob *)lparam);
        free((void *)lparam);
        return 0;
    case WM_APP_TRAY:
        /* NIN_SELECT is a single left click; NIN_KEYSELECT is Enter or Space on the icon. */
        if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT) show_window();
        else if (LOWORD(lparam) == WM_CONTEXTMENU) show_tray_menu();
        return 0;
    case WM_APP_SHOW:
        show_window();
        return 0;
    case WM_CLOSE:
        on_close();
        return 0;
    case WM_QUERYENDSESSION:
        /* Windows is signing out or shutting down: keep what was typed, then let it go. */
        if (!busy() && !W.exiting) save_settings(true);
        return TRUE;
    case WM_DESTROY:
        W.exiting = true;
        KillTimer(window, TIMER_SCHEDULE);
        KillTimer(window, TIMER_INSTALL);
        tray_remove(&W.tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* ---- Building the window ---- */

static HWND label(HWND parent, const wchar_t *value, FontId font, TextRole role, Surface surface)
{
    return ui_label(parent, value, font, role, surface);
}

static void create_info(InfoBar *bar, HWND parent, const wchar_t *title, const wchar_t *message)
{
    bar->title = label(parent, title, FONT_STRONG, TEXT_PRIMARY, SURFACE_INFO);
    bar->message = label(parent, message, FONT_BODY, TEXT_PRIMARY, SURFACE_INFO);
    bar->severity = NOTICE_INFO;
    bar->open = true;
}

static void add_items(HWND combo, const wchar_t *const *items, int count)
{
    for (int i = 0; i < count; i++) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)items[i]);
}

static void create_updates_tab(HWND parent)
{
    wchar_t name[160];
    create_info(&W.summary, parent, L"Ready when you are",
        L"Check now, or enable a schedule in Settings. Scheduled runs install updates only for apps with Automatic install turned on.");
    W.apps_title = label(parent, L"Your applications", FONT_TITLE, TEXT_PRIMARY, SURFACE_PAGE);
    W.monitored_text = label(parent, L"", FONT_BODY, TEXT_SECONDARY, SURFACE_PAGE);
    for (int i = 0; i < APP_COUNT; i++)
    {
        ProgramRow *row = &W.rows[i];
        ApplicationId id = (ApplicationId)i;
        row->settings = program(id);
        row->editable = true;
        row->state = STATE_NOT_CHECKED;
        str_copy(row->installed, COUNTOF(row->installed), L"Not checked");
        str_copy(row->available, COUNTOF(row->available), L"Not checked");
        str_copy(row->message, COUNTOF(row->message), L"Ready to check for updates.");
        row->name = label(parent, app_name(id), FONT_SUBTITLE, TEXT_PRIMARY, SURFACE_CARD);
        row->description = label(parent, app_description(id), FONT_CAPTION, TEXT_SECONDARY, SURFACE_CARD);
        row->state_text = label(parent, L"", FONT_STRONG, TEXT_PRIMARY, SURFACE_CARD);
        row->version = label(parent, L"", FONT_CAPTION, TEXT_SECONDARY, SURFACE_CARD);
        row->message_text = label(parent, L"", FONT_CAPTION, TEXT_SECONDARY, SURFACE_CARD);
        row->monitor_header = label(parent, L"Monitor", FONT_BODY, TEXT_PRIMARY, SURFACE_CARD);
        str_fmt(name, COUNTOF(name), L"Monitor %s", app_name(id));
        row->monitor = ui_button(parent, ID_MONITOR + i, name, BUTTON_SWITCH, SURFACE_CARD);
        row->auto_header = label(parent, L"Automatic install", FONT_BODY, TEXT_PRIMARY, SURFACE_CARD);
        str_fmt(name, COUNTOF(name), L"Automatically install updates for %s", app_name(id));
        row->auto_install = ui_button(parent, ID_AUTO_INSTALL + i, name, BUTTON_SWITCH, SURFACE_CARD);
        set_checked(row->monitor, row->settings->enabled);
        set_checked(row->auto_install, row->settings->auto_install);
    }
    W.activity_title = label(parent, L"Recent activity", FONT_TITLE, TEXT_PRIMARY, SURFACE_PAGE);
    W.open_logs = ui_button(parent, ID_OPEN_LOGS, L"Open log folder", BUTTON_STANDARD, SURFACE_PAGE);
    W.empty_activity = label(parent, L"Your checks and update results will appear here.", FONT_BODY, TEXT_SECONDARY, SURFACE_CARD);
    for (int i = 0; i < ACTIVITY_ROWS; i++)
    {
        W.activity_rows[i].time = label(parent, L"", FONT_CAPTION, TEXT_SECONDARY, SURFACE_CARD);
        W.activity_rows[i].application = label(parent, L"", FONT_CAPTION_STRONG, TEXT_PRIMARY, SURFACE_CARD);
        W.activity_rows[i].message = label(parent, L"", FONT_CAPTION, TEXT_PRIMARY, SURFACE_CARD);
    }
}

static void create_settings_tab(HWND parent)
{
    static const wchar_t *const modes[] = { L"Every few hours", L"Once a day" };
    static const wchar_t *const themes[] = { L"Dark", L"Light", L"Use Windows setting" };
    wchar_t item[32];
    const Surface card = SURFACE_CARD;
    W.settings_title = label(parent, L"Make it run your way", FONT_TITLE, TEXT_PRIMARY, SURFACE_PAGE);
    W.settings_intro = label(parent, L"Preferences and automatically found keys save themselves. Use Save connections after entering details by hand.",
        FONT_BODY, TEXT_SECONDARY, SURFACE_PAGE);

    W.schedule_title = label(parent, L"Scheduled updates", FONT_SUBTITLE, TEXT_PRIMARY, card);
    W.schedule_description = label(parent, L"Check monitored apps and apply their automatic install preferences.", FONT_BODY, TEXT_SECONDARY, card);
    W.schedule_toggle = ui_button(parent, ID_SCHEDULE_TOGGLE, L"Enable scheduled updates", BUTTON_SWITCH, card);
    W.schedule_mode_label = label(parent, L"Frequency", FONT_BODY, TEXT_PRIMARY, card);
    W.schedule_mode = ui_combo(parent, ID_SCHEDULE_MODE);
    add_items(W.schedule_mode, modes, COUNTOF(modes));
    W.interval_label = label(parent, L"Hours between checks", FONT_BODY, TEXT_PRIMARY, card);
    W.interval = ui_edit(parent, ID_INTERVAL, false, NULL);
    SetWindowLongW(W.interval, GWL_STYLE, GetWindowLongW(W.interval, GWL_STYLE) | ES_NUMBER);
    SendMessageW(W.interval, EM_SETLIMITTEXT, 3, 0);
    W.interval_down = ui_button(parent, ID_INTERVAL_DOWN, L"\x2212", BUTTON_STANDARD, card);
    W.interval_up = ui_button(parent, ID_INTERVAL_UP, L"+", BUTTON_STANDARD, card);
    W.time_label = label(parent, L"Local time", FONT_BODY, TEXT_PRIMARY, card);
    W.hour = ui_combo(parent, ID_HOUR);
    W.minute = ui_combo(parent, ID_MINUTE);
    for (int i = 0; i < 24; i++)
    {
        str_fmt(item, COUNTOF(item), L"%d %s", i % 12 ? i % 12 : 12, i < 12 ? L"AM" : L"PM");
        SendMessageW(W.hour, CB_ADDSTRING, 0, (LPARAM)item);
    }
    for (int i = 0; i < 60; i++)
    {
        str_fmt(item, COUNTOF(item), L":%02d", i);
        SendMessageW(W.minute, CB_ADDSTRING, 0, (LPARAM)item);
    }
    W.schedule_detail = label(parent, L"", FONT_BODY, TEXT_SECONDARY, card);
    W.schedule_check_now = ui_button(parent, ID_SCHEDULE_CHECK_NOW, L"Check now", BUTTON_STANDARD, card);
    W.schedule_note = label(parent, L"The app must be running for scheduled checks. Missed checks run when it is next available. Updates may briefly interrupt playback or downloads unless you turn on Wait while apps are in use below.",
        FONT_CAPTION, TEXT_SECONDARY, card);

    W.usage_title = label(parent, L"Wait while apps are in use", FONT_SUBTITLE, TEXT_PRIMARY, card);
    W.usage_description = label(parent, L"Automatic installs skip a checked app while it is busy and try again in about 30 minutes. Checking for updates is not affected.",
        FONT_BODY, TEXT_SECONDARY, card);
    for (int i = 0; i < APP_COUNT; i++)
    {
        W.rows[i].usage = ui_button(parent, ID_USAGE + i, app_name((ApplicationId)i), BUTTON_CHECK, card);
        W.rows[i].usage_description = label(parent, app_usage_description((ApplicationId)i), FONT_CAPTION, TEXT_SECONDARY, card);
        set_checked(W.rows[i].usage, W.rows[i].settings->wait_while_in_use);
    }
    /* Shown only when Pulsarr requires sign-in, so its busy check needs a key. */
    W.pulsarr_key_title = label(parent, L"Pulsarr API key", FONT_STRONG, TEXT_PRIMARY, card);
    W.pulsarr_key_description = label(parent, L"Pulsarr requires sign-in, so the updater needs a key to ask whether Pulsarr is syncing. Find key reads one from Pulsarr on this PC.",
        FONT_CAPTION, TEXT_SECONDARY, card);
    W.pulsarr_key = ui_edit(parent, ID_PULSARR_KEY, true, NULL);
    W.pulsarr_key_show = ui_button(parent, ID_PULSARR_KEY_SHOW, L"Show", BUTTON_STANDARD, card);
    W.find_pulsarr_key = ui_button(parent, ID_FIND_PULSARR_KEY, L"Find key", BUTTON_STANDARD, card);
    W.pulsarr_key_status = label(parent, L"", FONT_CAPTION, TEXT_SECONDARY, card);

    W.appearance_title = label(parent, L"Appearance & background behavior", FONT_SUBTITLE, TEXT_PRIMARY, card);
    W.theme_label = label(parent, L"Color mode", FONT_BODY, TEXT_PRIMARY, card);
    W.theme = ui_combo(parent, ID_THEME);
    add_items(W.theme, themes, COUNTOF(themes));
    W.tray_label = label(parent, L"Keep running in the notification area", FONT_BODY, TEXT_PRIMARY, card);
    W.tray_toggle = ui_button(parent, ID_TRAY, L"Minimize and close to tray", BUTTON_SWITCH, card);
    W.tray_note = label(parent, L"Minimize or close the window to hide it. Right-click the tray icon to exit.", FONT_CAPTION, TEXT_SECONDARY, card);
    W.start_minimized = ui_button(parent, ID_START_MINIMIZED, L"Start in the notification area", BUTTON_CHECK, card);
    W.startup = ui_button(parent, ID_STARTUP, L"Start with Windows when I sign in", BUTTON_CHECK, card);

    W.app_update_title = label(parent, L"Auto-update", FONT_SUBTITLE, TEXT_PRIMARY, card);
    W.app_update_description = label(parent, L"Keep Media Center Updater itself up to date with new versions from GitHub. Each download is checked against its published checksum before it runs.",
        FONT_BODY, TEXT_SECONDARY, card);
    create_info(&W.app_update_unavailable, parent, L"Only for installed copies",
        L"Auto-update works when Media Center Updater was installed with its setup program (MediaCenterUpdater-Setup.exe). This copy can still check, then you can download the new version yourself.");
    W.app_update_check_label = label(parent, L"Check for new versions", FONT_BODY, TEXT_PRIMARY, card);
    W.app_update_check_toggle = ui_button(parent, ID_APP_UPDATE_CHECK_TOGGLE, L"Check for new versions of Media Center Updater", BUTTON_SWITCH, card);
    W.app_update_check_note = label(parent, L"Checks once a day while the app is running, and tells you when a new version is ready.", FONT_CAPTION, TEXT_SECONDARY, card);
    W.app_update_install_label = label(parent, L"Install new versions automatically", FONT_BODY, TEXT_PRIMARY, card);
    W.app_update_install_toggle = ui_button(parent, ID_APP_UPDATE_INSTALL_TOGGLE, L"Install new versions of Media Center Updater automatically", BUTTON_SWITCH, card);
    W.app_update_install_note = label(parent, L"Waits until no app updates are running, then installs and reopens the updater. Your settings are kept.", FONT_CAPTION, TEXT_SECONDARY, card);
    W.app_update_check = ui_button(parent, ID_APP_UPDATE_CHECK, L"Check now", BUTTON_STANDARD, card);
    W.app_update_install = ui_button(parent, ID_APP_UPDATE_INSTALL, L"Install update", BUTTON_ACCENT, card);
    W.app_update_ring = ui_spinner(parent, card);
    W.app_update_status = label(parent, L"", FONT_BODY, TEXT_SECONDARY, card);

    W.connections_title = label(parent, L"Radarr & Sonarr connections", FONT_SUBTITLE, TEXT_PRIMARY, card);
    W.connections_intro = label(parent, L"An API key is like a password that lets this updater check Radarr or Sonarr and ask it to install updates. We look for both keys on this PC when the updater opens and fill in any empty boxes. You usually do not need to enter them yourself.",
        FONT_BODY, TEXT_SECONDARY, card);
    W.connections_help = label(parent, L"If a key is missing or has changed, choose Find API keys again. Found keys are filled in and saved securely for your Windows account. If we cannot find a key, you can copy it from that app's Settings \x2192 General \x2192 Security page. Plex and SABnzbd do not need a key. If Pulsarr needs one, it appears under Wait while apps are in use.",
        FONT_BODY, TEXT_SECONDARY, card);
    W.radarr_url_label = label(parent, L"Radarr URL", FONT_BODY, TEXT_PRIMARY, card);
    W.radarr_url = ui_edit(parent, ID_RADARR_URL, false, L"http://localhost:7878");
    W.radarr_key_label = label(parent, L"Radarr API key", FONT_BODY, TEXT_PRIMARY, card);
    W.radarr_key = ui_edit(parent, ID_RADARR_KEY, true, NULL);
    W.radarr_key_show = ui_button(parent, ID_RADARR_KEY_SHOW, L"Show", BUTTON_STANDARD, card);
    W.radarr_key_status = label(parent, L"Looking for Radarr's key...", FONT_BODY, TEXT_SECONDARY, card);
    W.sonarr_url_label = label(parent, L"Sonarr URL", FONT_BODY, TEXT_PRIMARY, card);
    W.sonarr_url = ui_edit(parent, ID_SONARR_URL, false, L"http://localhost:8989");
    W.sonarr_key_label = label(parent, L"Sonarr API key", FONT_BODY, TEXT_PRIMARY, card);
    W.sonarr_key = ui_edit(parent, ID_SONARR_KEY, true, NULL);
    W.sonarr_key_show = ui_button(parent, ID_SONARR_KEY_SHOW, L"Show", BUTTON_STANDARD, card);
    W.sonarr_key_status = label(parent, L"Looking for Sonarr's key...", FONT_BODY, TEXT_SECONDARY, card);
    W.find_keys = ui_button(parent, ID_FIND_KEYS, L"Find API keys again", BUTTON_STANDARD, card);
    W.save_connections = ui_button(parent, ID_SAVE_CONNECTIONS, L"Save connections", BUTTON_STANDARD, card);
    W.key_search_ring = ui_spinner(parent, card);
    W.key_search_status = label(parent, L"", FONT_BODY, TEXT_SECONDARY, card);

    W.pulsarr_title = label(parent, L"Pulsarr installation", FONT_SUBTITLE, TEXT_PRIMARY, card);
    W.pulsarr_description = label(parent, L"For automatic updates, Pulsarr needs its Windows installer and Windows service. A service is a background program that Windows manages, so the installer can stop Pulsarr, update it, and start it again. We check the installation on this PC below; you do not need to reinstall Pulsarr if it already meets this requirement.",
        FONT_BODY, TEXT_SECONDARY, card);
    create_info(&W.pulsarr_setup, parent, L"Checking Pulsarr's installation", L"Checking whether this PC already has the supported setup...");

    create_info(&W.administrator, parent, L"Administrator access",
        L"Installing Windows apps and enabling sign-in startup may require administrator access.");
    W.elevate = W.administrator.action = ui_button(parent, ID_ELEVATE, L"Restart as administrator", BUTTON_STANDARD, SURFACE_INFO);
    W.settings_status = label(parent, L"", FONT_BODY, TEXT_SECONDARY, SURFACE_PAGE);
    W.exit_button = ui_button(parent, ID_EXIT, L"Exit Media Center Updater", BUTTON_STANDARD, SURFACE_PAGE);
    W.version_text = label(parent, L"Version " VERSION_TEXT, FONT_CAPTION, TEXT_SECONDARY, SURFACE_PAGE);

    ui_add_tooltip(W.schedule_check_now, L"Run a scheduled check now: check monitored apps and install updates where Automatic install is on. The next scheduled check is not changed.");
    ui_add_tooltip(W.find_pulsarr_key, L"Read an API key from Pulsarr's database on this PC. Nothing in Pulsarr is changed.");
    ui_add_tooltip(W.find_keys, L"Read Radarr, Sonarr, and Pulsarr's settings on this PC again. Replace keys only when a matching key is found.");
}

static void load_controls(void)
{
    const AppSettings *settings = W.settings;
    wchar_t text[200], when[80];
    SendMessageW(W.theme, CB_SETCURSEL, wcscmp(settings->theme, L"Light") == 0 ? 1 : wcscmp(settings->theme, L"System") == 0 ? 2 : 0, 0);
    set_checked(W.schedule_toggle, settings->schedule_enabled);
    SendMessageW(W.schedule_mode, CB_SETCURSEL, settings->schedule_mode == SCHEDULE_DAILY ? 1 : 0, 0);
    str_fmt(text, COUNTOF(text), L"%d", settings->interval_hours);
    SetWindowTextW(W.interval, text);
    SendMessageW(W.hour, CB_SETCURSEL, settings->daily_hour, 0);
    SendMessageW(W.minute, CB_SETCURSEL, settings->daily_minute, 0);
    set_checked(W.tray_toggle, settings->minimize_to_tray);
    set_checked(W.start_minimized, settings->start_minimized);
    set_checked(W.app_update_check_toggle, settings->app_update_check);
    set_checked(W.app_update_install_toggle, settings->app_update_install);
    info_open(&W.app_update_unavailable, !self_update_is_installed_copy());
    str_copy(text, COUNTOF(text), L"You have version " VERSION_TEXT L".");
    if (settings->last_app_update_check.has)
    {
        format_time(settings->last_app_update_check.ticks, when, COUNTOF(when));
        str_cat(text, COUNTOF(text), L" Last checked ");
        str_cat(text, COUNTOF(text), when);
        str_cat(text, COUNTOF(text), L".");
    }
    ui_set_text(W.app_update_status, text);
    SetWindowTextW(W.radarr_url, program(APP_RADARR)->server_url);
    SetWindowTextW(W.radarr_key, program(APP_RADARR)->api_key);
    SetWindowTextW(W.sonarr_url, program(APP_SONARR)->server_url);
    SetWindowTextW(W.sonarr_key, program(APP_SONARR)->api_key);
    SetWindowTextW(W.pulsarr_key, program(APP_PULSARR)->api_key);
    StartupState startup = startup_state(text, COUNTOF(text));
    set_checked(W.startup, startup == STARTUP_ON);
    if (startup == STARTUP_UNKNOWN) ui_set_text(W.settings_status, text);
    info_open(&W.administrator, !is_administrator());

    W.activity_count = store_read_recent_activity(W.store, W.activity, ACTIVITY_ROWS);
    refresh_activity();
}

bool main_window_create(HINSTANCE instance, AppSettings *settings, SettingsStore *store, Coordinator *coordinator,
    bool start_in_tray, int show_command)
{
    W.instance = instance;
    W.settings = settings;
    W.store = store;
    W.coordinator = coordinator;
    W.initializing = true;
    /* The first automatic check for a new version waits a minute so it doesn't compete with startup work. */
    W.next_app_update_attempt = time_now() + TICKS_PER_MINUTE;
    W.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    W.icon_large = LoadImageW(instance, MAKEINTRESOURCEW(APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    W.icon_small = LoadImageW(instance, MAKEINTRESOURCEW(APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);

    WNDCLASSEXW window_class = { sizeof(window_class) };
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hIcon = W.icon_large;
    window_class.hIconSm = W.icon_small;
    window_class.lpszClassName = L"MediaCenterUpdaterWindow";
    WNDCLASSEXW panel_class = { sizeof(panel_class) };
    panel_class.lpfnWndProc = panel_proc;
    panel_class.hInstance = instance;
    panel_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    panel_class.lpszClassName = L"MediaCenterUpdaterPanel";
    if (!RegisterClassExW(&window_class) || !RegisterClassExW(&panel_class)) return false;

    ui_init();
    W.window = CreateWindowExW(WS_EX_CONTROLPARENT, window_class.lpszClassName, L"Media Center Updater " VERSION_TEXT,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 900, NULL, NULL, instance, NULL);
    if (!W.window) return false;
    ui_set_dpi(GetDpiForWindow(W.window));

    W.title = label(W.window, L"Media Center Updater", FONT_HEADER, TEXT_PRIMARY, SURFACE_PAGE);
    W.subtitle = label(W.window, L"Keep your media services up to date.", FONT_BODY, TEXT_SECONDARY, SURFACE_PAGE);
    W.busy_ring = ui_spinner(W.window, SURFACE_PAGE);
    W.check = ui_button(W.window, ID_CHECK, L"Check for updates", BUTTON_STANDARD, SURFACE_PAGE);
    W.install = ui_button(W.window, ID_INSTALL, L"Check & install", BUTTON_ACCENT, SURFACE_PAGE);
    W.tabs[0] = ui_button(W.window, ID_TAB_UPDATES, L"Updates", BUTTON_TAB, SURFACE_PAGE);
    W.tabs[1] = ui_button(W.window, ID_TAB_SETTINGS, L"Settings", BUTTON_TAB, SURFACE_PAGE);
    for (int i = 0; i < 2; i++)
    {
        HWND panel = CreateWindowExW(WS_EX_CONTROLPARENT, panel_class.lpszClassName, NULL,
            WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN | (i == 0 ? WS_VISIBLE : 0), 0, 0, 0, 0, W.window, NULL, instance, NULL);
        SetWindowLongPtrW(panel, GWLP_USERDATA, i);
        W.panels[i].window = panel;
    }
    create_updates_tab(W.panels[0].window);
    create_settings_tab(W.panels[1].window);
    W.next_run_text = label(W.window, L"Schedule is off", FONT_CAPTION, TEXT_SECONDARY, SURFACE_PAGE);
    W.background_text = label(W.window, L"Close to tray is on", FONT_CAPTION, TEXT_SECONDARY, SURFACE_PAGE);
    ui_add_tooltip(W.check, L"Check versions without installing updates.");
    ui_add_tooltip(W.install, L"Check monitored apps and install updates where Automatic install is enabled.");
    ui_set_selected(W.tabs[0], true);

    load_controls();
    CoordinatorEvents events = { NULL, on_result, on_activity, on_busy, on_summary };
    coordinator->events = events;
    W.initializing = false;
    apply_theme();
    refresh_preferences();
    set_busy(busy());
    if (store->load_warning[0]) show_summary(L"Review your settings", store->load_warning, NOTICE_WARNING);

    /* Size the window for the display it opened on, without spilling off a small screen. */
    MONITORINFO monitor = { sizeof(monitor) };
    GetMonitorInfoW(MonitorFromWindow(W.window, MONITOR_DEFAULTTONEAREST), &monitor);
    int work_width = monitor.rcWork.right - monitor.rcWork.left, work_height = monitor.rcWork.bottom - monitor.rcWork.top;
    int width = S(1180) < work_width ? S(1180) : work_width, height = S(900) < work_height ? S(900) : work_height;
    SetWindowPos(W.window, NULL, monitor.rcWork.left + (work_width - width) / 2, monitor.rcWork.top + (work_height - height) / 2,
        width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    layout_all();

    bool tray = tray_add(&W.tray, W.window, W.icon_small, WM_APP_TRAY);
    if (!tray) show_summary(L"Notification area unavailable", L"The notification area icon could not be added, so closing the window exits the app.", NOTICE_WARNING);
    if (tray && settings->minimize_to_tray && (settings->start_minimized || start_in_tray)) W.hidden_to_tray = true;
    else
    {
        ShowWindow(W.window, show_command == SW_SHOWMINIMIZED || show_command == SW_SHOWMINNOACTIVE ? SW_SHOWNORMAL : show_command);
        UpdateWindow(W.window);
    }
    SetTimer(W.window, TIMER_SCHEDULE, 30000, NULL);
    refresh_schedule_text();
    refresh_detected_setup(false);
    return true;
}

void main_window_show(void)
{
    if (W.window) PostMessageW(W.window, WM_APP_SHOW, 0, 0);
}

bool main_window_handle_message(MSG *message)
{
    if (!W.window) return false;
    /* The wheel scrolls the open tab wherever the pointer is, unless a drop-down list is open under it. */
    if (message->message == WM_MOUSEWHEEL && IsChild(W.window, message->hwnd))
    {
        wchar_t class_name[32] = L"";
        GetClassNameW(message->hwnd, class_name, COUNTOF(class_name));
        if (_wcsicmp(class_name, L"ComboBox") == 0 && !SendMessageW(message->hwnd, CB_GETDROPPEDSTATE, 0, 0))
            message->hwnd = W.panels[W.tab].window;
    }
    bool handled = IsDialogMessageW(W.window, message) != 0;
    if (message->message == WM_KEYDOWN && (message->wParam == VK_TAB || (message->wParam >= VK_LEFT && message->wParam <= VK_DOWN)))
        ensure_visible(GetFocus());
    return handled;
}
