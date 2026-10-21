using System;
using System.IO;
using System.Linq;
using System.Threading;
using Microsoft.UI.Xaml;

namespace MediaCenterUpdater;

public partial class App : Microsoft.UI.Xaml.Application
{
    private MainWindow? window;
    private Mutex? instanceMutex;
    private EventWaitHandle? showEvent;
    private RegisteredWaitHandle? showWait;

    public App() { InitializeComponent(); }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // Used by the uninstaller: remove the sign-in startup task, then quit without showing a window.
        if (Environment.GetCommandLineArgs().Contains("--remove-startup", StringComparer.OrdinalIgnoreCase))
        {
            try { WindowsIntegration.SetStartupEnabled(false); } catch (Exception) { }
            Exit();
            return;
        }

        // One updater per desktop session, including elevated and ordinary launches.
        bool isFirst;
        try { instanceMutex = new Mutex(true, @"Local\MediaCenterUpdater.Instance", out isFirst); }
        catch (UnauthorizedAccessException) { Exit(); return; }
        if (!isFirst && Environment.GetCommandLineArgs().Contains("--elevated-handoff"))
        {
            try { isFirst = instanceMutex.WaitOne(TimeSpan.FromSeconds(15)); }
            catch (AbandonedMutexException) { isFirst = true; }
        }
        if (!isFirst)
        {
            try { using var existing = EventWaitHandle.OpenExisting(@"Local\MediaCenterUpdater.Show"); existing.Set(); }
            catch (WaitHandleCannotBeOpenedException) { }
            catch (UnauthorizedAccessException) { }
            instanceMutex.Dispose();
            Exit();
            return;
        }
        showEvent = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\MediaCenterUpdater.Show");
        var store = new SettingsStore();
        UnhandledException += (_, e) =>
        {
            try { File.AppendAllText(Path.Combine(store.DataDirectory, "crash.log"), $"{DateTimeOffset.Now:O} {e.Exception.GetType().Name}: {e.Message}{Environment.NewLine}{e.Exception.StackTrace}{Environment.NewLine}"); }
            catch { }
        };
        var settings = store.Load();
        window = new MainWindow(settings, store, new UpdateCoordinator(settings, store, new UpdateProviders()));
        showWait = ThreadPool.RegisterWaitForSingleObject(showEvent, (_, _) => window.DispatcherQueue.TryEnqueue(() =>
        {
            window.ShowFromTray();
        }), null, Timeout.Infinite, false);
        window.Closed += (_, _) =>
        {
            showWait?.Unregister(null);
            showEvent?.Dispose();
            instanceMutex?.ReleaseMutex();
            instanceMutex?.Dispose();
        };
        window.Activate();
    }
}
