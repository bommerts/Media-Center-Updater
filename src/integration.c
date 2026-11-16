#include "integration.h"
#include "system.h"
#include "xml.h"
#include <shellapi.h>

#define TASK_NAMESPACE L"http://schemas.microsoft.com/windows/2004/02/mit/task"

/* ---- Notification area icon ---- */

bool tray_add(TrayIcon *tray, HWND window, HICON icon, UINT message)
{
    NOTIFYICONDATAW data = { sizeof(data) };
    tray->window = window;
    tray->icon = icon;
    tray->message = message;
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = message;
    data.hIcon = icon;
    str_copy(data.szTip, COUNTOF(data.szTip), L"Media Center Updater");
    tray->added = Shell_NotifyIconW(NIM_ADD, &data) != 0;
    if (tray->added)
    {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    return tray->added;
}

void tray_remove(TrayIcon *tray)
{
    if (!tray->added) return;
    NOTIFYICONDATAW data = { sizeof(data) };
    data.hWnd = tray->window;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    tray->added = false;
}

/* ---- Startup task. Task Scheduler is driven through schtasks.exe. ---- */

typedef struct TaskResult { DWORD exit_code; wchar_t *output; } TaskResult;

static bool task_name(wchar_t *dst, size_t cap, wchar_t *error, size_t error_cap)
{
    wchar_t sid[200];
    if (!current_user_sid(sid, COUNTOF(sid)))
    {
        str_copy(error, error_cap, L"Windows could not identify the signed-in user.");
        return false;
    }
    str_fmt(dst, cap, L"Media Center Updater %s", sid);
    return true;
}

/* Redirected XML may be UTF-16, while diagnostic messages use the Windows OEM code page. */
static wchar_t *decode_task_output(const char *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    wchar_t *text;
    bool big_endian = length >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF;
    if (big_endian || (length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) || (length >= 4 && bytes[1] == 0 && bytes[3] == 0))
    {
        size_t count = length / 2;
        text = xalloc((count + 1) * sizeof(wchar_t));
        for (size_t i = 0; i < count; i++)
            text[i] = (wchar_t)(big_endian ? (bytes[i * 2] << 8) | bytes[i * 2 + 1] : (bytes[i * 2 + 1] << 8) | bytes[i * 2]);
    }
    else
    {
        UINT code_page = length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? CP_UTF8 : GetOEMCP();
        int needed = length ? MultiByteToWideChar(code_page, 0, data, (int)length, NULL, 0) : 0;
        text = xalloc(((size_t)needed + 1) * sizeof(wchar_t));
        if (needed) MultiByteToWideChar(code_page, 0, data, (int)length, text, needed);
    }
    str_trim_chars(text, L"\xFEFF \t\r\n");
    return text;
}

static bool run_task_command(const wchar_t *const *arguments, int count, TaskResult *result, wchar_t *error, size_t error_cap)
{
    wchar_t system[PATHN], program[PATHN];
    ProcResult process;
    folder_system(system, COUNTOF(system));
    path_join(program, COUNTOF(program), system, L"schtasks.exe");
    ProcStatus status = proc_run(program, arguments, count, 15000, true, true, &process);
    if (status == PROC_TIMED_OUT)
    {
        free(process.output);
        str_copy(error, error_cap, L"Windows Task Scheduler did not respond within 15 seconds.");
        return false;
    }
    if (status != PROC_OK)
    {
        str_copy(error, error_cap, L"Windows Task Scheduler could not be opened.");
        return false;
    }
    result->exit_code = process.exit_code;
    result->output = decode_task_output(process.output, process.output_length);
    free(process.output);
    return true;
}

static bool is_task_not_found(const TaskResult *result)
{
    wchar_t not_found[512];
    win_error_text(ERROR_FILE_NOT_FOUND, not_found, COUNTOF(not_found));
    str_trim_chars(not_found, L". \r\n");
    return result->exit_code != 0 && not_found[0] && str_ifind(result->output, not_found);
}

StartupState startup_state(wchar_t *error, size_t error_cap)
{
    wchar_t name[260];
    TaskResult result;
    if (!task_name(name, COUNTOF(name), error, error_cap)) return STARTUP_UNKNOWN;
    const wchar_t *arguments[] = { L"/Query", L"/TN", name, L"/XML" };
    if (!run_task_command(arguments, COUNTOF(arguments), &result, error, error_cap)) return STARTUP_UNKNOWN;

    StartupState state = STARTUP_UNKNOWN;
    if (result.exit_code != 0)
    {
        if (is_task_not_found(&result)) state = STARTUP_OFF;
        else str_fmt(error, error_cap, L"Windows could not read the startup task. %s", result.output);
    }
    else
    {
        XmlDoc *doc = xml_parse_text(result.output);
        if (doc && wcscmp(doc->root->name, L"Task") == 0 && doc->root->xmlns && wcscmp(doc->root->xmlns, TASK_NAMESPACE) == 0)
        {
            const XmlNode *enabled = xml_child(xml_child(doc->root, L"Settings"), L"Enabled");
            wchar_t value[16];
            str_copy(value, COUNTOF(value), enabled ? enabled->text : L"true");
            str_trim(value);
            if (!wcscmp(value, L"true") || !wcscmp(value, L"1")) state = STARTUP_ON;
            else if (!wcscmp(value, L"false") || !wcscmp(value, L"0")) state = STARTUP_OFF;
        }
        if (state == STARTUP_UNKNOWN) str_copy(error, error_cap, L"Windows returned an unreadable startup task.");
        xml_free(doc);
    }
    free(result.output);
    return state;
}

static void element(StrBuf *xml, const wchar_t *name, const wchar_t *text)
{
    sb_appendf(xml, L"<%s>", name);
    xml_escape(xml, text);
    sb_appendf(xml, L"</%s>", name);
}

static bool startup_task_xml(StrBuf *xml, wchar_t *error, size_t error_cap)
{
    wchar_t sid[200], program[PATHN], directory[PATHN];
    if (!current_user_sid(sid, COUNTOF(sid)))
    {
        str_copy(error, error_cap, L"Windows could not identify the signed-in user.");
        return false;
    }
    app_exe_path(program, COUNTOF(program));
    app_exe_directory(directory, COUNTOF(directory));
    sb_append(xml, L"<?xml version=\"1.0\" encoding=\"UTF-16\"?><Task version=\"1.2\" xmlns=\"" TASK_NAMESPACE L"\">"
        L"<RegistrationInfo><Description>Start Media Center Updater in the notification area at sign-in.</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled>");
    element(xml, L"UserId", sid);
    sb_append(xml, L"</LogonTrigger></Triggers><Principals><Principal id=\"Author\">");
    element(xml, L"UserId", sid);
    sb_append(xml, L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable><Enabled>true</Enabled>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings><Actions Context=\"Author\"><Exec>");
    element(xml, L"Command", program);
    sb_append(xml, L"<Arguments>--tray</Arguments>");
    element(xml, L"WorkingDirectory", directory);
    sb_append(xml, L"</Exec></Actions></Task>");
    return true;
}

bool startup_set(bool enabled, wchar_t *error, size_t error_cap)
{
    wchar_t name[260], temporary[MAX_PATH] = L"", folder[MAX_PATH];
    TaskResult result;
    bool ran;
    if (enabled && !is_administrator())
    {
        str_copy(error, error_cap, L"Restart Media Center Updater as administrator before enabling Windows startup. "
            L"The startup task needs administrator permission to install updates without repeated prompts.");
        return false;
    }
    if (!task_name(name, COUNTOF(name), error, error_cap)) return false;
    if (enabled)
    {
        StrBuf xml;
        sb_init(&xml);
        bool written = startup_task_xml(&xml, error, error_cap);
        if (written)
        {
            /* schtasks.exe only accepts task XML files saved as UTF-16 with a byte order mark. */
            size_t bytes = (xml.len + 1) * sizeof(wchar_t);
            wchar_t *content = xalloc(bytes);
            content[0] = 0xFEFF;
            memcpy(content + 1, xml.p, xml.len * sizeof(wchar_t));
            written = GetTempPathW(MAX_PATH, folder) && GetTempFileNameW(folder, L"mcu", 0, temporary) &&
                file_write_all(temporary, content, bytes);
            free(content);
            if (!written) str_copy(error, error_cap, L"The startup task could not be prepared.");
        }
        sb_free(&xml);
        if (!written)
        {
            if (temporary[0]) DeleteFileW(temporary);
            return false;
        }
        const wchar_t *arguments[] = { L"/Create", L"/TN", name, L"/XML", temporary, L"/F" };
        ran = run_task_command(arguments, COUNTOF(arguments), &result, error, error_cap);
        DeleteFileW(temporary);
    }
    else
    {
        const wchar_t *arguments[] = { L"/Delete", L"/TN", name, L"/F" };
        ran = run_task_command(arguments, COUNTOF(arguments), &result, error, error_cap);
    }
    if (!ran) return false;
    bool ok = result.exit_code == 0 || (!enabled && is_task_not_found(&result));
    if (!ok) str_fmt(error, error_cap, L"Windows could not %s the startup task. %s", enabled ? L"enable" : L"disable", result.output);
    free(result.output);
    return ok;
}

/* ---- Administrator restart ---- */

bool restart_elevated(bool in_tray, wchar_t *error, size_t error_cap)
{
    wchar_t program[PATHN], directory[PATHN];
    SHELLEXECUTEINFOW info = { sizeof(info) };
    app_exe_path(program, COUNTOF(program));
    app_exe_directory(directory, COUNTOF(directory));
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = program;
    info.lpParameters = in_tray ? L"--elevated-handoff --tray" : L"--elevated-handoff";
    info.lpDirectory = directory;
    info.nShow = SW_SHOWNORMAL;
    /* A declined administrator prompt fails with error 1223. The existing window stays open. */
    if (ShellExecuteExW(&info)) return true;
    win_error_text(GetLastError(), error, error_cap);
    if (!error[0]) str_copy(error, error_cap, L"Windows could not start an administrator instance.");
    return false;
}
