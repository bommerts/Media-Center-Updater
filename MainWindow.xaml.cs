using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace MediaCenterUpdater;

public sealed partial class MainWindow : Window
{
    private readonly AppSettings _settings;
    private readonly SettingsStore _store;
    private readonly UpdateCoordinator _coordinator;
    private readonly List<ProgramRow> _programs = new();
    private readonly ObservableCollection<ActivityRow> _activity = new();
    private WindowsIntegration? _integration;
    private DispatcherQueueTimer? _scheduleTimer;
    private bool _initializing = true;
    private bool _loaded;
    private bool _exiting;
    private bool _dialogOpen;
    private bool _retrievingKeys;

    // Comes from <Version> in the project file.
    public static string AppVersion { get; } =
        System.Reflection.CustomAttributeExtensions.GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>(
            typeof(MainWindow).Assembly)?.InformationalVersion ?? "unknown";

    public MainWindow(AppSettings settings, SettingsStore store, UpdateCoordinator coordinator)
    {
        _settings = settings;
        _store = store;
        _coordinator = coordinator;
        InitializeComponent();
        AppWindow.Resize(new SizeInt32(1180, 900));
        Title = "Media Center Updater";
        AppVersionText.Text = "Version " + AppVersion;
        AppWindow.Closing += Window_Closing;
        Closed += Window_Closed;

        foreach (ApplicationId id in Enum.GetValues<ApplicationId>())
        {
            ProgramSettings? program = settings.Programs.FirstOrDefault(p => p.Id == id);
            if (program is null)
            {
                program = new ProgramSettings { Id = id };
                settings.Programs.Add(program);
            }
            _programs.Add(new ProgramRow(program, ProgramPreferenceChanged));
        }
        ProgramsList.ItemsSource = _programs;
        ActivityList.ItemsSource = _activity;
        foreach (ActivityEntry entry in store.ReadRecentActivity().OrderByDescending(a => a.Time).Take(30))
            _activity.Add(new ActivityRow(entry));
        EmptyActivityText.Visibility = _activity.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        ThemeBox.SelectedIndex = settings.Theme == "Light" ? 1 : settings.Theme == "System" ? 2 : 0;
        ScheduleToggle.IsOn = settings.ScheduleEnabled;
        ScheduleModeBox.SelectedIndex = settings.ScheduleMode == ScheduleMode.Daily ? 1 : 0;
        IntervalBox.Value = settings.IntervalHours;
        DailyTimePicker.Time = new TimeSpan(settings.DailyHour, settings.DailyMinute, 0);
        TrayToggle.IsOn = settings.MinimizeToTray;
        StartMinimizedBox.IsChecked = settings.StartMinimized;
        ProgramSettings radarr = Program(ApplicationId.Radarr);
        ProgramSettings sonarr = Program(ApplicationId.Sonarr);
        RadarrUrlBox.Text = radarr.ServerUrl;
        RadarrKeyBox.Password = radarr.ApiKey;
        SonarrUrlBox.Text = sonarr.ServerUrl;
        SonarrKeyBox.Password = sonarr.ApiKey;
        try { StartupBox.IsChecked = WindowsIntegration.IsStartupEnabled(); }
        catch (Exception ex) { SettingsStatusText.Text = ex.Message; }

        AdministratorBar.IsOpen = !WindowsIntegration.IsAdministrator;
        _coordinator.ResultChanged += Coordinator_ResultChanged;
        _coordinator.Activity += Coordinator_Activity;
        _coordinator.BusyChanged += Coordinator_BusyChanged;
        _coordinator.SummaryChanged += Coordinator_SummaryChanged;
        _initializing = false;
        Root.ActualThemeChanged += (_, _) => ApplyTitleBarTheme();
        ApplyTheme();
        RefreshPreferences();
        SetBusy(coordinator.IsBusy);
        if (!string.IsNullOrWhiteSpace(store.LoadWarning))
            ShowSummary("Review your settings", store.LoadWarning, InfoBarSeverity.Warning);
    }

    public void ShowFromTray()
    {
        if (_integration is not null) _integration.ShowWindow();
        else { AppWindow.Show(); Activate(); }
    }

    private void Root_Loaded(object sender, RoutedEventArgs e)
    {
        if (_loaded) return;
        _loaded = true;
        try
        {
            _integration = new WindowsIntegration(this, () => _settings.MinimizeToTray,
                () => _ = RunAsync(false), RequestExit);
            if (_settings.MinimizeToTray && (_settings.StartMinimized ||
                Environment.GetCommandLineArgs().Any(a => a.Equals("--tray", StringComparison.OrdinalIgnoreCase))))
                _integration.HideToTray();
        }
        catch (Exception ex)
        {
            Activate();
            ShowSummary("Notification area unavailable", ex.Message, InfoBarSeverity.Warning);
        }

        _scheduleTimer = DispatcherQueue.CreateTimer();
        _scheduleTimer.Interval = TimeSpan.FromSeconds(30);
        _scheduleTimer.Tick += ScheduleTimer_Tick;
        _scheduleTimer.Start();
        RefreshScheduleText();
        _ = RefreshDetectedSetupAsync(replaceExistingKeys: false);
    }

    private async void ScheduleTimer_Tick(DispatcherQueueTimer sender, object args)
    {
        if (_exiting || _retrievingKeys || _coordinator.IsBusy || !_settings.ScheduleEnabled) return;
        try
        {
            await _coordinator.EvaluateScheduleAsync();
            RefreshScheduleText();
        }
        catch (Exception ex) { ShowSummary("Scheduled check could not finish", ex.Message, InfoBarSeverity.Error); }
    }

    private void Coordinator_ResultChanged(UpdateResult result) => OnUi(() =>
        _programs.FirstOrDefault(p => p.Id == result.Id)?.Apply(result));

    private void Coordinator_Activity(ActivityEntry entry) => OnUi(() =>
    {
        _activity.Insert(0, new ActivityRow(entry));
        while (_activity.Count > 30) _activity.RemoveAt(_activity.Count - 1);
        EmptyActivityText.Visibility = Visibility.Collapsed;
    });

    private void Coordinator_BusyChanged(bool busy) => OnUi(() => SetBusy(busy));

    private void Coordinator_SummaryChanged(string summary) => OnUi(() =>
        ShowSummary(_coordinator.IsBusy ? "Working on your applications" : "Latest result", summary, InfoBarSeverity.Informational));

    private void OnUi(Action action)
    {
        if (_exiting) return;
        if (DispatcherQueue.HasThreadAccess) action();
        else DispatcherQueue.TryEnqueue(() => { if (!_exiting) action(); });
    }

    private async void Check_Click(object sender, RoutedEventArgs e) => await RunAsync(false);
    private async void Install_Click(object sender, RoutedEventArgs e) => await RunAsync(true);

    private async Task RunAsync(bool install)
    {
        if (_coordinator.IsBusy || _retrievingKeys || _exiting || !SaveSettings(saveConnections: true)) return;
        if (!_settings.Programs.Any(p => p.Enabled))
        {
            ShowSummary("Choose an application", "Turn on Monitor for at least one app before checking for updates.", InfoBarSeverity.Warning);
            return;
        }
        SetBusy(true);
        try { await _coordinator.RunAsync(install); }
        catch (Exception ex) { ShowSummary("Update check could not finish", ex.Message, InfoBarSeverity.Error); }
        finally { SetBusy(_coordinator.IsBusy); RefreshScheduleText(); }
    }

    private void SetBusy(bool busy)
    {
        bool connectionControlsEnabled = !busy && !_retrievingKeys;
        CheckButton.IsEnabled = connectionControlsEnabled;
        InstallButton.IsEnabled = connectionControlsEnabled;
        BusyRing.IsActive = busy;
        BusyRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        SaveConnectionsButton.IsEnabled = connectionControlsEnabled;
        FindKeysButton.IsEnabled = connectionControlsEnabled;
        RadarrUrlBox.IsEnabled = connectionControlsEnabled;
        RadarrKeyBox.IsEnabled = connectionControlsEnabled;
        SonarrUrlBox.IsEnabled = connectionControlsEnabled;
        SonarrKeyBox.IsEnabled = connectionControlsEnabled;
        ScheduleToggle.IsEnabled = !busy;
        ScheduleModeBox.IsEnabled = !busy && _settings.ScheduleEnabled;
        IntervalBox.IsEnabled = !busy && _settings.ScheduleEnabled;
        DailyTimePicker.IsEnabled = !busy && _settings.ScheduleEnabled;
        ElevateButton.IsEnabled = connectionControlsEnabled;
        StartupBox.IsEnabled = !busy;
        foreach (ProgramRow program in _programs) program.IsEditable = !busy;
        RefreshScheduleText();
    }

    private void ProgramPreferenceChanged()
    {
        if (_initializing) return;
        SaveSettings();
        RefreshPreferences();
    }

    private void Preference_Changed(object sender, RoutedEventArgs e)
    {
        if (_initializing) return;
        SaveSettings();
        RefreshPreferences();
    }

    private void Selection_Changed(object sender, SelectionChangedEventArgs e) => Preference_Changed(sender, e);
    private void Interval_Changed(NumberBox sender, NumberBoxValueChangedEventArgs args) => Preference_Changed(sender, new RoutedEventArgs());
    private void DailyTime_Changed(object sender, TimePickerValueChangedEventArgs e) => Preference_Changed(sender, new RoutedEventArgs());

    private void Theme_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_initializing) return;
        SaveSettings();
        ApplyTheme();
    }

    private void ApplyTheme()
    {
        Root.RequestedTheme = _settings.Theme switch
        {
            "Light" => ElementTheme.Light,
            "System" => ElementTheme.Default,
            _ => ElementTheme.Dark
        };
        ApplyTitleBarTheme();
    }

    private void ApplyTitleBarTheme()
    {
        int dark = Root.ActualTheme == ElementTheme.Dark ? 1 : 0;
        _ = DwmSetWindowAttribute(WinRT.Interop.WindowNative.GetWindowHandle(this), 20, ref dark, sizeof(int));
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(nint window, int attribute, ref int value, int size);

    private bool SaveSettings(bool saveConnections = false)
    {
        if (_initializing) return true;
        bool scheduleChanged = _settings.ScheduleEnabled != ScheduleToggle.IsOn ||
            _settings.ScheduleMode != (ScheduleModeBox.SelectedIndex == 1 ? ScheduleMode.Daily : ScheduleMode.Interval) ||
            (!double.IsNaN(IntervalBox.Value) && _settings.IntervalHours != (int)IntervalBox.Value) ||
            _settings.DailyHour != DailyTimePicker.Time.Hours || _settings.DailyMinute != DailyTimePicker.Time.Minutes;

        _settings.Theme = (ThemeBox.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "Dark";
        _settings.ScheduleEnabled = ScheduleToggle.IsOn;
        _settings.ScheduleMode = ScheduleModeBox.SelectedIndex == 1 ? ScheduleMode.Daily : ScheduleMode.Interval;
        if (!double.IsNaN(IntervalBox.Value)) _settings.IntervalHours = Math.Clamp((int)IntervalBox.Value, 1, 168);
        _settings.DailyHour = DailyTimePicker.Time.Hours;
        _settings.DailyMinute = DailyTimePicker.Time.Minutes;
        _settings.MinimizeToTray = TrayToggle.IsOn;
        _settings.StartMinimized = StartMinimizedBox.IsChecked == true;
        if (saveConnections)
        {
            Program(ApplicationId.Radarr).ServerUrl = RadarrUrlBox.Text.Trim();
            Program(ApplicationId.Radarr).ApiKey = RadarrKeyBox.Password.Trim();
            Program(ApplicationId.Sonarr).ServerUrl = SonarrUrlBox.Text.Trim();
            Program(ApplicationId.Sonarr).ApiKey = SonarrKeyBox.Password.Trim();
        }
        try
        {
            if (scheduleChanged) _coordinator.RecalculateSchedule();
            _store.Save(_settings);
            SettingsStatusText.Text = "Preferences saved.";
            return true;
        }
        catch (Exception ex)
        {
            SettingsStatusText.Text = "Could not save preferences: " + ex.Message;
            ShowSummary("Preferences could not be saved", ex.Message, InfoBarSeverity.Error);
            return false;
        }
    }

    private ProgramSettings Program(ApplicationId id) => _settings.Programs.First(p => p.Id == id);

    private void RefreshPreferences()
    {
        bool daily = ScheduleModeBox.SelectedIndex == 1;
        ScheduleModeBox.IsEnabled = ScheduleToggle.IsOn && !_coordinator.IsBusy;
        IntervalBox.IsEnabled = ScheduleToggle.IsOn && !_coordinator.IsBusy;
        DailyTimePicker.IsEnabled = ScheduleToggle.IsOn && !_coordinator.IsBusy;
        IntervalBox.Visibility = daily ? Visibility.Collapsed : Visibility.Visible;
        DailyTimePicker.Visibility = daily ? Visibility.Visible : Visibility.Collapsed;
        StartMinimizedBox.IsEnabled = TrayToggle.IsOn;
        BackgroundText.Text = TrayToggle.IsOn ? "Close to tray is on" : "Closing the window exits the app";
        int count = _programs.Count(p => p.Enabled);
        MonitoredText.Text = $"{count} of {_programs.Count} monitored";
        RefreshScheduleText();
    }

    private void RefreshScheduleText()
    {
        string text = !_settings.ScheduleEnabled ? "Schedule is off. You can check for updates at any time." :
            _settings.NextRun is { } next ? "Next scheduled check: " + next.ToLocalTime().ToString("ddd, MMM d 'at' h:mm tt") :
            "Scheduled checks are enabled.";
        ScheduleDetailText.Text = text;
        NextRunText.Text = _coordinator.IsBusy ? "Update check in progress" : text;
    }

    private void SaveConnections_Click(object sender, RoutedEventArgs e)
    {
        if (SaveSettings(saveConnections: true)) SettingsStatusText.Text = "Connection details saved. Check for updates to test them.";
    }

    private async void FindKeys_Click(object sender, RoutedEventArgs e) =>
        await RefreshDetectedSetupAsync(replaceExistingKeys: true);

    private async Task RefreshDetectedSetupAsync(bool replaceExistingKeys)
    {
        if (_retrievingKeys || _coordinator.IsBusy || _exiting) return;
        _retrievingKeys = true;
        SetBusy(_coordinator.IsBusy);
        KeySearchRing.IsActive = true;
        KeySearchRing.Visibility = Visibility.Visible;
        KeySearchStatusText.Text = "Looking in this PC's application settings...";
        string radarrUrl = RadarrUrlBox.Text.Trim();
        string sonarrUrl = SonarrUrlBox.Text.Trim();
        string radarrKey = RadarrKeyBox.Password;
        string sonarrKey = SonarrKeyBox.Password;
        try
        {
            var discoveries = await Task.Run(() => (
                Radarr: replaceExistingKeys || string.IsNullOrWhiteSpace(radarrKey)
                    ? LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, radarrUrl) : null,
                Sonarr: replaceExistingKeys || string.IsNullOrWhiteSpace(sonarrKey)
                    ? LocalSetupDiscovery.FindApiKey(ApplicationId.Sonarr, sonarrUrl) : null,
                Pulsarr: LocalSetupDiscovery.CheckPulsarrSetup()));
            if (_exiting) return;

            int filled = 0;
            if (ApplyDiscoveredKey(ApplicationId.Radarr, discoveries.Radarr, RadarrUrlBox, RadarrKeyBox, RadarrKeyStatusText, radarrUrl, radarrKey)) filled++;
            if (ApplyDiscoveredKey(ApplicationId.Sonarr, discoveries.Sonarr, SonarrUrlBox, SonarrKeyBox, SonarrKeyStatusText, sonarrUrl, sonarrKey)) filled++;
            PulsarrSetupBar.Title = discoveries.Pulsarr.IsSupported ? "This PC meets Pulsarr's requirement" : "Pulsarr setup needs attention";
            PulsarrSetupBar.Message = discoveries.Pulsarr.Message;
            PulsarrSetupBar.Severity = discoveries.Pulsarr.IsSupported ? InfoBarSeverity.Success : InfoBarSeverity.Warning;

            if (filled > 0)
            {
                // Save only the retrieved connections, not other edits still waiting in the form.
                _store.Save(_settings);
                KeySearchStatusText.Text = filled == 2 ? "Both API keys were found, filled in, and saved securely." : "One API key was found, filled in, and saved securely. See each app's result above.";
            }
            else
                KeySearchStatusText.Text = replaceExistingKeys
                    ? "Search finished. No replacement keys were found. Any keys already entered have been kept."
                    : "Setup check finished. Any saved or manually entered keys have been kept. Use Find API keys again to refresh them.";
        }
        catch (Exception)
        {
            if (_exiting) return;
            // File and XML errors must never display configuration contents or key values.
            KeySearchStatusText.Text = "The setup check or saving could not finish. Any keys already shown have been kept. Try Find API keys again, or use Save connections.";
            if (PulsarrSetupBar.Severity == InfoBarSeverity.Informational)
            {
                PulsarrSetupBar.Title = "Pulsarr setup could not be checked";
                PulsarrSetupBar.Message = "Try Find API keys again to repeat the setup check.";
                PulsarrSetupBar.Severity = InfoBarSeverity.Warning;
            }
        }
        finally
        {
            _retrievingKeys = false;
            if (!_exiting)
            {
                KeySearchRing.IsActive = false;
                KeySearchRing.Visibility = Visibility.Collapsed;
                SetBusy(_coordinator.IsBusy);
            }
        }
    }

    private bool ApplyDiscoveredKey(ApplicationId id, ApiKeyDiscoveryResult? result, TextBox addressBox,
        PasswordBox keyBox, TextBlock statusText, string originalAddress, string originalKey)
    {
        string name = ApplicationCatalog.Name(id);
        if (addressBox.Text.Trim() != originalAddress || keyBox.Password != originalKey)
        {
            statusText.Text = $"{name}: Your latest changes were kept. Choose Find API keys again to search with the new details.";
            return false;
        }
        if (result is null)
        {
            statusText.Text = $"{name}: A saved key is already filled in. Find API keys again can refresh it from the app's settings.";
            return false;
        }
        statusText.Text = $"{name}: {result.Message}";
        if (!result.Found) return false;
        keyBox.Password = result.ApiKey;
        var program = Program(id);
        program.ServerUrl = originalAddress;
        program.ApiKey = result.ApiKey;
        return true;
    }

    private void Startup_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            WindowsIntegration.SetStartupEnabled(StartupBox.IsChecked == true);
            SettingsStatusText.Text = StartupBox.IsChecked == true ? "Media Center Updater will start when you sign in." : "Sign-in startup is off.";
        }
        catch (Exception ex)
        {
            try { StartupBox.IsChecked = WindowsIntegration.IsStartupEnabled(); }
            catch { StartupBox.IsChecked = false; }
            SettingsStatusText.Text = ex.Message;
            _ = ShowNoticeAsync("Startup setting could not be changed", ex.Message);
        }
    }

    private void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        try { Process.Start(new ProcessStartInfo("explorer.exe", _store.DataDirectory) { UseShellExecute = true }); }
        catch (Exception ex) { _ = ShowNoticeAsync("Log folder could not be opened", ex.Message); }
    }

    private void Elevate_Click(object sender, RoutedEventArgs e)
    {
        if (_coordinator.IsBusy || !SaveSettings(saveConnections: true)) return;
        try
        {
            WindowsIntegration.RestartElevated();
            _exiting = true;
            _integration?.Dispose();
            Close();
        }
        catch (Exception ex) { _ = ShowNoticeAsync("Administrator restart could not complete", ex.Message); }
    }

    private void Exit_Click(object sender, RoutedEventArgs e) => RequestExit();

    private void RequestExit()
    {
        if (_coordinator.IsBusy)
        {
            _integration?.ShowWindow();
            _ = ShowNoticeAsync("An update operation is running", "Wait for the current check or installation to finish before exiting. You can minimize the app while it works.");
            return;
        }
        if (!SaveSettings(saveConnections: true)) return;
        _exiting = true;
        _integration?.Dispose();
        Close();
    }

    private void Window_Closing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_exiting) return;
        if (_coordinator.IsBusy && !_settings.MinimizeToTray)
        {
            args.Cancel = true;
            _ = ShowNoticeAsync("An update operation is running", "Wait for the current check or installation to finish before exiting.");
            return;
        }
        if (!_coordinator.IsBusy) SaveSettings(saveConnections: true);
    }

    private void Window_Closed(object sender, WindowEventArgs args)
    {
        _exiting = true;
        _scheduleTimer?.Stop();
        _integration?.Dispose();
        _coordinator.ResultChanged -= Coordinator_ResultChanged;
        _coordinator.Activity -= Coordinator_Activity;
        _coordinator.BusyChanged -= Coordinator_BusyChanged;
        _coordinator.SummaryChanged -= Coordinator_SummaryChanged;
    }

    private void ShowSummary(string title, string message, InfoBarSeverity severity)
    {
        SummaryBar.Title = title;
        SummaryBar.Message = message;
        SummaryBar.Severity = severity;
        SummaryBar.IsOpen = true;
    }

    private async Task ShowNoticeAsync(string title, string message)
    {
        if (_dialogOpen || Root.XamlRoot is null) return;
        _dialogOpen = true;
        try
        {
            await new ContentDialog
            {
                XamlRoot = Root.XamlRoot,
                RequestedTheme = Root.RequestedTheme,
                Title = title,
                Content = new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
                CloseButtonText = "OK",
                DefaultButton = ContentDialogButton.Close
            }.ShowAsync();
        }
        finally { _dialogOpen = false; }
    }
}

public sealed class ProgramRow : INotifyPropertyChanged
{
    private readonly ProgramSettings _settings;
    private readonly Action _changed;
    private bool _editable = true;
    private UpdateState _state = UpdateState.NotChecked;
    private string _installed = "Not checked";
    private string _available = "Not checked";
    private string _message = "Ready to check for updates.";

    public ProgramRow(ProgramSettings settings, Action changed) { _settings = settings; _changed = changed; }
    public ApplicationId Id => _settings.Id;
    public string Name => ApplicationCatalog.Name(Id);
    public string Description => ApplicationCatalog.Description(Id);
    public string MonitorAccessibleName => "Monitor " + Name;
    public string InstallAccessibleName => "Automatically install updates for " + Name;
    public bool Enabled
    {
        get => _settings.Enabled;
        set
        {
            if (_settings.Enabled == value) return;
            _settings.Enabled = value;
            Notify(); Notify(nameof(CanAutoInstall)); Notify(nameof(StateText)); Notify(nameof(StateBrush));
            _changed();
        }
    }
    public bool AutoInstall
    {
        get => _settings.AutoInstall;
        set { if (_settings.AutoInstall == value) return; _settings.AutoInstall = value; Notify(); _changed(); }
    }
    public bool IsEditable
    {
        get => _editable;
        set { _editable = value; Notify(); Notify(nameof(CanAutoInstall)); }
    }
    public bool CanAutoInstall => Enabled && IsEditable;
    public string VersionText => $"Installed: {_installed}   •   Latest: {_available}";
    public string Message => _message;
    public string StateText => !Enabled ? "Monitoring off" : _state switch
    {
        UpdateState.Checking => "Checking for updates",
        UpdateState.UpToDate => "Up to date",
        UpdateState.UpdateAvailable => "Update available",
        UpdateState.Installing => "Installing update",
        UpdateState.Updated => "Updated successfully",
        UpdateState.NotInstalled => "Not detected",
        UpdateState.NeedsSetup => "Setup needed",
        UpdateState.Error => "Needs attention",
        UpdateState.RestartRequired => "Restart required",
        _ => "Not checked"
    };
    public SolidColorBrush StateBrush => new(!Enabled ? Colors.Gray : _state switch
    {
        UpdateState.UpToDate or UpdateState.Updated => Windows.UI.Color.FromArgb(255, 36, 157, 100),
        UpdateState.Error => Windows.UI.Color.FromArgb(255, 219, 75, 75),
        UpdateState.NeedsSetup or UpdateState.RestartRequired or UpdateState.UpdateAvailable => Windows.UI.Color.FromArgb(255, 210, 140, 31),
        UpdateState.Checking or UpdateState.Installing => Windows.UI.Color.FromArgb(255, 67, 129, 244),
        _ => Colors.Gray
    });

    public void Apply(UpdateResult result)
    {
        _state = result.State;
        if (!string.IsNullOrWhiteSpace(result.InstalledVersion)) _installed = result.InstalledVersion;
        else if (result.State == UpdateState.NotInstalled) _installed = "Not detected";
        if (!string.IsNullOrWhiteSpace(result.AvailableVersion)) _available = result.AvailableVersion;
        _message = result.Message;
        Notify(nameof(StateText)); Notify(nameof(StateBrush)); Notify(nameof(VersionText)); Notify(nameof(Message));
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Notify([CallerMemberName] string? property = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
}

public sealed class ActivityRow
{
    private readonly ActivityEntry _entry;
    public ActivityRow(ActivityEntry entry) => _entry = entry;
    public string TimeText => _entry.Time.ToLocalTime().ToString("MMM d, h:mm tt");
    public string Application => _entry.Application;
    public string Message => _entry.Message;
}
