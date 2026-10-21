using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Xml;
using System.Xml.Linq;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Forms = System.Windows.Forms;

namespace MediaCenterUpdater;

/// <summary>Owns the notification icon, window visibility, and optional Windows startup task.</summary>
public sealed class WindowsIntegration : IDisposable
{
    private readonly Microsoft.UI.Xaml.Window _window;
    private readonly AppWindow _appWindow;
    private readonly DispatcherQueue _dispatcher;
    private readonly Func<bool> _minimizeToTray;
    private readonly Forms.NotifyIcon _trayIcon;
    private readonly Forms.ContextMenuStrip _trayMenu;
    private readonly Icon _icon;
    private bool _hiddenToTray;
    private bool _changingVisibility;
    private bool _disposed;

    public WindowsIntegration(Microsoft.UI.Xaml.Window window, Func<bool> minimizeToTray,
        Action checkNow, Action exit)
    {
        ArgumentNullException.ThrowIfNull(window);
        ArgumentNullException.ThrowIfNull(minimizeToTray);
        ArgumentNullException.ThrowIfNull(checkNow);
        ArgumentNullException.ThrowIfNull(exit);

        _window = window;
        _appWindow = window.AppWindow;
        _dispatcher = window.DispatcherQueue;
        _minimizeToTray = minimizeToTray;
        // Load the size Windows uses for tray icons at the current display scale.
        try { _icon = new Icon(MainWindow.AssetPath("AppIcon.ico"), Forms.SystemInformation.SmallIconSize); }
        catch (Exception exception) when (exception is IOException or ArgumentException or UnauthorizedAccessException)
        { _icon = (Icon)SystemIcons.Application.Clone(); }
        _trayMenu = new Forms.ContextMenuStrip();
        _trayMenu.Items.Add("Open Media Center Updater", null, (_, _) => ShowWindow());
        _trayMenu.Items.Add("Check for updates now", null, (_, _) => OnUiThread(checkNow));
        _trayMenu.Items.Add(new Forms.ToolStripSeparator());
        _trayMenu.Items.Add("Exit", null, (_, _) => OnUiThread(exit));
        _trayIcon = new Forms.NotifyIcon
        {
            Text = "Media Center Updater",
            Icon = _icon,
            ContextMenuStrip = _trayMenu,
            Visible = true
        };
        _trayIcon.DoubleClick += OnTrayDoubleClick;
        _appWindow.Changed += OnAppWindowChanged;
        _appWindow.Closing += OnAppWindowClosing;
    }

    public static bool IsAdministrator
    {
        get
        {
            using var identity = WindowsIdentity.GetCurrent();
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
        }
    }

    public bool IsHiddenToTray => _hiddenToTray;

    public void ShowWindow() => OnUiThread(() =>
    {
        _changingVisibility = true;
        try
        {
            _appWindow.Show();
            var handle = WinRT.Interop.WindowNative.GetWindowHandle(_window);
            RestoreWindow(handle, 9); // SW_RESTORE also restores a previously minimized window.
            _window.Activate();
            SetForegroundWindow(handle);
            _hiddenToTray = false;
        }
        finally
        {
            _changingVisibility = false;
        }
    });

    public void HideToTray() => OnUiThread(() =>
    {
        if (_hiddenToTray)
            return;
        _trayIcon.Visible = true;
        _hiddenToTray = true;
        try
        {
            _appWindow.Hide();
        }
        catch
        {
            _hiddenToTray = false;
            throw;
        }
    });

    /// <summary>Starts an elevated instance. The caller closes this instance after success.</summary>
    public static void RestartElevated()
    {
        var startInfo = new ProcessStartInfo(ExecutablePath)
        {
            UseShellExecute = true,
            Verb = "runas",
            WorkingDirectory = AppContext.BaseDirectory
        };
        startInfo.ArgumentList.Add("--elevated-handoff");
        if (Environment.GetCommandLineArgs().Skip(1).Contains("--tray", StringComparer.OrdinalIgnoreCase))
            startInfo.ArgumentList.Add("--tray");

        // A declined UAC prompt throws Win32Exception with NativeErrorCode 1223.
        // Leave the existing window open so its caller can report the cancellation.
        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Windows could not start an administrator instance.");
    }

    public static bool IsStartupEnabled()
    {
        var result = RunTaskCommand("/Query", "/TN", StartupTaskName, "/XML");
        if (result.ExitCode != 0)
        {
            if (IsTaskNotFound(result)) return false;
            throw TaskCommandError("read", result);
        }
        try
        {
            XNamespace ns = "http://schemas.microsoft.com/windows/2004/02/mit/task";
            var task = XDocument.Parse(result.Output).Root;
            if (task?.Name != ns + "Task") throw new XmlException("Unexpected task document.");
            var enabled = (string?)task.Element(ns + "Settings")?.Element(ns + "Enabled");
            return enabled is null || XmlConvert.ToBoolean(enabled);
        }
        catch (Exception exception) when (exception is XmlException or FormatException)
        {
            throw new InvalidOperationException("Windows returned an unreadable startup task.", exception);
        }
    }

    public static void SetStartupEnabled(bool enabled)
    {
        if (enabled && !IsAdministrator)
            throw new InvalidOperationException(
                "Restart Media Center Updater as administrator before enabling Windows startup. " +
                "The startup task needs administrator permission to install updates without repeated prompts.");

        string? temporary = null;
        try
        {
            TaskCommandResult result;
            if (enabled)
            {
                temporary = Path.GetTempFileName();
                // schtasks.exe only accepts task XML files saved as UTF-16 with a byte order mark.
                File.WriteAllText(temporary, CreateStartupTaskXml(), new UnicodeEncoding(false, true));
                result = RunTaskCommand("/Create", "/TN", StartupTaskName, "/XML", temporary, "/F");
            }
            else
            {
                result = RunTaskCommand("/Delete", "/TN", StartupTaskName, "/F");
                if (IsTaskNotFound(result)) return;
            }
            if (result.ExitCode != 0) throw TaskCommandError(enabled ? "enable" : "disable", result);
        }
        finally
        {
            if (temporary is not null)
                try { File.Delete(temporary); }
                catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
        }
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        _appWindow.Changed -= OnAppWindowChanged;
        _appWindow.Closing -= OnAppWindowClosing;
        _trayIcon.DoubleClick -= OnTrayDoubleClick;
        _trayIcon.Visible = false;
        _trayIcon.Dispose();
        _trayMenu.Dispose();
        _icon.Dispose();
    }

    private void OnTrayDoubleClick(object? sender, EventArgs e) => ShowWindow();

    private void OnAppWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (!_disposed && !_changingVisibility && !_hiddenToTray && _minimizeToTray() &&
            sender.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized })
            HideToTray();
    }

    private void OnAppWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_disposed || args.Cancel || !_minimizeToTray())
            return;
        args.Cancel = true;
        HideToTray();
    }

    private void OnUiThread(Action action)
    {
        if (_disposed)
            return;
        if (_dispatcher.HasThreadAccess)
            action();
        else
            _dispatcher.TryEnqueue(() => { if (!_disposed) action(); });
    }

    private static string ExecutablePath => Environment.ProcessPath
        ?? throw new InvalidOperationException("The application's executable location is unavailable.");

    private static string CurrentUserSid
    {
        get
        {
            using var identity = WindowsIdentity.GetCurrent();
            return identity.User?.Value
                ?? throw new InvalidOperationException("Windows could not identify the signed-in user.");
        }
    }

    private static string StartupTaskName => "Media Center Updater " + CurrentUserSid;

    private sealed record TaskCommandResult(int ExitCode, string Output, string Error);

    private static TaskCommandResult RunTaskCommand(params string[] arguments)
    {
        var startInfo = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "schtasks.exe"))
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in arguments) startInfo.ArgumentList.Add(argument);
        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Windows Task Scheduler could not be opened.");
        using var output = new MemoryStream();
        using var error = new MemoryStream();
        var outputRead = process.StandardOutput.BaseStream.CopyToAsync(output);
        var errorRead = process.StandardError.BaseStream.CopyToAsync(error);
        if (!process.WaitForExit(15000))
        {
            try { process.Kill(); process.WaitForExit(2000); }
            catch (Exception exception) when (exception is InvalidOperationException or Win32Exception) { }
            throw new InvalidOperationException("Windows Task Scheduler did not respond within 15 seconds.");
        }
        Task.WhenAll(outputRead, errorRead).GetAwaiter().GetResult();
        return new TaskCommandResult(process.ExitCode, DecodeTaskOutput(output.ToArray()), DecodeTaskOutput(error.ToArray()));
    }

    private static string DecodeTaskOutput(byte[] bytes)
    {
        if (bytes.Length == 0) return "";
        // Redirected XML may be UTF-16, while diagnostic messages use the Windows OEM code page.
        Encoding encoding;
        if (bytes.Length >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
            encoding = Encoding.BigEndianUnicode;
        else if (bytes.Length >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE ||
                 bytes.Length >= 4 && bytes[1] == 0 && bytes[3] == 0)
            encoding = Encoding.Unicode;
        else if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
            encoding = Encoding.UTF8;
        else
        {
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            encoding = Encoding.GetEncoding((int)GetOEMCP());
        }
        return encoding.GetString(bytes).TrimStart('\uFEFF').Trim();
    }

    private static bool IsTaskNotFound(TaskCommandResult result) => result.ExitCode != 0 &&
        (result.Error + result.Output).Contains(new Win32Exception(2).Message.TrimEnd('.'), StringComparison.OrdinalIgnoreCase);

    private static InvalidOperationException TaskCommandError(string operation, TaskCommandResult result)
    {
        var detail = string.IsNullOrWhiteSpace(result.Error) ? result.Output : result.Error;
        return new InvalidOperationException($"Windows could not {operation} the startup task. {detail}");
    }

    private static string CreateStartupTaskXml()
    {
        XNamespace ns = "http://schemas.microsoft.com/windows/2004/02/mit/task";
        return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>" + new XDocument(new XElement(ns + "Task", new XAttribute("version", "1.2"),
            new XElement(ns + "RegistrationInfo",
                new XElement(ns + "Description", "Start Media Center Updater in the notification area at sign-in.")),
            new XElement(ns + "Triggers", new XElement(ns + "LogonTrigger",
                new XElement(ns + "Enabled", true), new XElement(ns + "UserId", CurrentUserSid))),
            new XElement(ns + "Principals", new XElement(ns + "Principal", new XAttribute("id", "Author"),
                new XElement(ns + "UserId", CurrentUserSid),
                new XElement(ns + "LogonType", "InteractiveToken"),
                new XElement(ns + "RunLevel", "HighestAvailable"))),
            new XElement(ns + "Settings",
                new XElement(ns + "MultipleInstancesPolicy", "IgnoreNew"),
                new XElement(ns + "DisallowStartIfOnBatteries", false),
                new XElement(ns + "StopIfGoingOnBatteries", false),
                new XElement(ns + "StartWhenAvailable", true),
                new XElement(ns + "Enabled", true),
                new XElement(ns + "ExecutionTimeLimit", "PT0S")),
            new XElement(ns + "Actions", new XAttribute("Context", "Author"),
                new XElement(ns + "Exec", new XElement(ns + "Command", ExecutablePath),
                    new XElement(ns + "Arguments", "--tray"),
                    new XElement(ns + "WorkingDirectory", AppContext.BaseDirectory)))))
            .ToString(SaveOptions.DisableFormatting);
    }

    [DllImport("kernel32.dll")]
    private static extern uint GetOEMCP();

    [DllImport("user32.dll", EntryPoint = "ShowWindow")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RestoreWindow(nint windowHandle, int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(nint windowHandle);
}
