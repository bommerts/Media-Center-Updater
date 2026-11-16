/* Program entry: one updater per desktop session, then the window and its message loop. */
#include "app.h"
#include "integration.h"
#include "providers.h"
#include <shellapi.h>

#ifdef MCU_DEV
#define INSTANCE_MUTEX L"Local\\MediaCenterUpdater.Dev.Instance"
#define SHOW_EVENT L"Local\\MediaCenterUpdater.Dev.Show"
#else
#define INSTANCE_MUTEX L"Local\\MediaCenterUpdater.Instance"
#define SHOW_EVENT L"Local\\MediaCenterUpdater.Show"
#endif

static AppSettings settings;
static SettingsStore store;
static Providers providers;
static Coordinator coordinator;

static bool has_argument(int count, wchar_t **arguments, const wchar_t *name, bool ignore_case)
{
    for (int i = 1; i < count; i++)
        if (ignore_case ? str_ieq(arguments[i], name) : wcscmp(arguments[i], name) == 0) return true;
    return false;
}

static LONG WINAPI record_crash(EXCEPTION_POINTERS *exception)
{
    wchar_t path[PATHN], line[200];
    DateTimeOffset now = dto_now();
    dto_format(&now, line, COUNTOF(line));
    size_t used = wcslen(line);
    str_fmt(line + used, COUNTOF(line) - used, L" Exception 0x%08lX at %p\r\n",
        exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress);
    path_join(path, COUNTOF(path), store.data_directory, L"crash.log");
    char *utf8 = wide_to_utf8(line, &used);
    file_append(path, utf8, used);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void CALLBACK show_requested(void *context, BOOLEAN timed_out)
{
    main_window_show();
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show_command)
{
    int count = 0;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) count = 0;

    /* Used by the uninstaller: remove the sign-in startup task, then quit without showing a window. */
    if (has_argument(count, arguments, L"--remove-startup", true))
    {
        wchar_t error[MSGN];
        startup_set(false, error, COUNTOF(error));
        return 0;
    }

    /* One updater per desktop session, including elevated and ordinary launches. */
    HANDLE mutex = CreateMutexW(NULL, TRUE, INSTANCE_MUTEX);
    if (!mutex) return 0;
    bool first = GetLastError() != ERROR_ALREADY_EXISTS;
    if (!first && has_argument(count, arguments, L"--elevated-handoff", false))
    {
        /* The copy that asked for administrator rights is closing; wait for it to let go. */
        DWORD waited = WaitForSingleObject(mutex, 15000);
        first = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    if (!first)
    {
        HANDLE existing = OpenEventW(EVENT_MODIFY_STATE, FALSE, SHOW_EVENT);
        if (existing)
        {
            SetEvent(existing);
            CloseHandle(existing);
        }
        CloseHandle(mutex);
        return 0;
    }
    HANDLE show_event = CreateEventW(NULL, FALSE, FALSE, SHOW_EVENT), show_wait = NULL;

    store_init(&store, NULL);
    SetUnhandledExceptionFilter(record_crash);
    store_load(&store, &settings);
    providers_init(&providers, NULL, NULL);
    coordinator_init(&coordinator, &settings, &store, providers_interface(&providers));

    int exit_code = 1;
    if (main_window_create(instance, &settings, &store, &coordinator, has_argument(count, arguments, L"--tray", true), show_command))
    {
        if (show_event) RegisterWaitForSingleObject(&show_wait, show_event, show_requested, NULL, INFINITE, WT_EXECUTEDEFAULT);
        MSG message;
        while (GetMessageW(&message, NULL, 0, 0) > 0)
        {
            if (main_window_handle_message(&message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        exit_code = (int)message.wParam;
    }
    if (show_wait) UnregisterWaitEx(show_wait, INVALID_HANDLE_VALUE);
    if (show_event) CloseHandle(show_event);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return exit_code;
}
