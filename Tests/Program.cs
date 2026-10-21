using MediaCenterUpdater;
using ApplicationId = MediaCenterUpdater.ApplicationId;

if (args.Contains("--inspect-local"))
{
    // Deployment verification calls only the provider's read-only check method.
    var checker = new UpdateProviders();
    var checks = new List<UpdateResult>();
    foreach (var program in new AppSettings().Programs)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(2));
        checks.Add(await checker.CheckAsync(program, timeout.Token));
    }
    Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(checks));
    return;
}

var directory = Path.Combine(Path.GetTempPath(), "MediaCenterUpdater.Tests", Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(directory);
var count = 0;
void Assert(bool value, string message) { if (!value) throw new Exception(message); count++; Console.WriteLine("PASS " + message); }
SettingsStore Store(string name) => new(Path.Combine(directory, name));
AppSettings Only(params ApplicationId[] ids) => new() { Programs = ids.Select(id => new ProgramSettings { Id = id }).ToList() };

var defaults = new AppSettings();
Assert(defaults.Theme == "Dark" && defaults.MinimizeToTray && !defaults.ScheduleEnabled, "Dark and tray defaults, schedule requires opt-in");
var testStore = Store("secrets");
defaults.Programs[2].ApiKey = "test-key-not-real-4321";
testStore.Save(defaults);
Assert(!File.ReadAllText(Path.Combine(testStore.DataDirectory, "settings.json")).Contains("test-key-not-real"), "API keys are encrypted on disk");
Assert(testStore.Load().Programs[2].ApiKey == defaults.Programs[2].ApiKey, "API keys round-trip for the Windows user");
Assert(defaults.Programs[2].ApiKey == "test-key-not-real-4321", "Saving does not replace the in-memory API key");
File.WriteAllText(Path.Combine(testStore.DataDirectory, "settings.json"), "{ broken");
Assert(!testStore.Load().ScheduleEnabled && testStore.LoadWarning is not null, "Corrupt settings cannot resume automatic updates");

var settings = Only(ApplicationId.Plex, ApplicationId.Sabnzbd, ApplicationId.Sonarr);
settings.Programs[1].Enabled = false;
settings.Programs[2].AutoInstall = false;
var provider = new FakeProvider();
var coordinator = new UpdateCoordinator(settings, Store("check"), provider);
await coordinator.RunAsync(false);
Assert(provider.Checked.SequenceEqual(new[] { ApplicationId.Plex, ApplicationId.Sonarr }) && provider.Installed.Count == 0, "Check-only scans skip disabled apps and never install");
provider.Checked.Clear();
await coordinator.RunAsync(true);
Assert(provider.Installed.SequenceEqual(new[] { ApplicationId.Plex }), "Automatic updates respect each program's install switch");

provider = new FakeProvider { FailId = ApplicationId.Plex };
coordinator = new(settings, Store("failure"), provider);
var errors = new List<UpdateResult>();
coordinator.ResultChanged += errors.Add;
await coordinator.RunAsync(true);
Assert(provider.Checked.Contains(ApplicationId.Sonarr) && !coordinator.IsBusy, "A failed app does not stop the remaining checks or leave the updater busy");
Assert(errors.Any(r => r.Id == ApplicationId.Plex && r.State == UpdateState.Error && !r.Message.Contains("secret")), "Provider exceptions do not leak credentials to the UI");

provider = new FakeProvider { Block = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously) };
coordinator = new(Only(ApplicationId.Plex), Store("overlap"), provider);
var firstRun = coordinator.RunAsync(true);
await coordinator.RunAsync(true);
Assert(provider.Checked.Count == 1, "Overlapping manual and scheduled runs are excluded");
provider.Block.SetResult();
await firstRun;
Assert(provider.Installed.Count == 1, "Overlapping requests cannot install an update twice");

settings = Only(ApplicationId.Plex);
settings.ScheduleEnabled = true;
settings.IntervalHours = 6;
settings.NextRun = DateTimeOffset.Now.AddDays(-3);
provider = new FakeProvider();
var scheduleStore = Store("schedule");
coordinator = new(settings, scheduleStore, provider);
await coordinator.EvaluateScheduleAsync();
await coordinator.EvaluateScheduleAsync();
Assert(provider.Checked.Count == 1 && settings.NextRun > DateTimeOffset.Now, "A missed schedule runs once and advances to the next interval");
Assert(scheduleStore.Load().NextRun == settings.NextRun, "Next scheduled run survives restart");
settings.ScheduleEnabled = false;
settings.NextRun = DateTimeOffset.Now.AddMinutes(-1);
await coordinator.EvaluateScheduleAsync();
Assert(provider.Checked.Count == 1, "Disabling the schedule blocks overdue work");
settings.ScheduleMode = ScheduleMode.Daily;
settings.DailyHour = 3;
settings.DailyMinute = 15;
var now = new DateTimeOffset(2026, 9, 11, 16, 0, 0, TimeSpan.Zero);
var next = UpdateCoordinator.ComputeNextRun(settings, now);
Assert(next > now && next - now <= TimeSpan.FromHours(25) && next.LocalDateTime.Hour == 3 && next.LocalDateTime.Minute == 15, "Daily schedule uses the next occurrence in local time");
settings.DailyHour = 1;
settings.DailyMinute = 30;
var repeatedHour = new DateTimeOffset(2026, 11, 1, 1, 15, 0, TimeSpan.FromHours(-5));
Assert(UpdateCoordinator.ComputeNextRun(settings, repeatedHour) > repeatedHour, "Repeated daylight-saving hour cannot produce a past next-run time");

Assert(UpdateProviders.TryVersion("v1.2.3", out var parsed) && parsed == new Version(1, 2, 3, 0), "Official release tags compare with normalized installed versions");
Assert(!UpdateProviders.TryVersion("unknown", out _), "Unknown versions cannot be treated as update candidates");

var discoveryRoot = Path.Combine(directory, "local-setup");
var radarrDirectory = Path.Combine(discoveryRoot, "Radarr");
Directory.CreateDirectory(radarrDirectory);
var configPath = Path.Combine(radarrDirectory, "config.xml");
var discovery = LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878", discoveryRoot);
Assert(!discovery.Found && discovery.ApiKey.Length == 0, "Missing application settings leave the API key empty");
File.WriteAllText(configPath, "<Config><Port>7878</Port><ApiKey>test-discovered-key</ApiKey><UrlBase></UrlBase></Config>");
discovery = LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878", discoveryRoot);
Assert(discovery.Found && discovery.ApiKey == "test-discovered-key", "Local retrieval finds the key in the matching application's existing settings");
Assert(!discovery.Message.Contains("test-discovered-key") && !discovery.ToString().Contains("test-discovered-key"), "Discovery status and diagnostic text do not reveal API keys");
Assert(LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://" + Environment.MachineName + ":7878", discoveryRoot).Found, "This PC's exact computer name is supported for local retrieval");
Assert(!LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://other-computer.invalid:7878", discoveryRoot).Found, "A local API key is never discovered for another computer's address");
Assert(!LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:9999", discoveryRoot).Found, "Another local port cannot receive the wrong application's key");
Assert(!LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878/another-instance", discoveryRoot).Found, "Another base path cannot receive the wrong application's key");
File.WriteAllText(configPath, "<Config><Port>7878</Port><ApiKey>test-discovered-key</ApiKey><UrlBase>/radarr</UrlBase></Config>");
Assert(LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://127.0.0.1:7878/radarr/", discoveryRoot).Found, "Retrieval accepts the configured base path and loopback address");
File.WriteAllText(configPath, "<Config><Port>7878</Port><ApiKey> </ApiKey></Config>");
Assert(!LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878", discoveryRoot).Found, "An empty API key is reported without filling a field");
File.WriteAllText(configPath, "<Config><ApiKey>test-secret");
discovery = LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878", discoveryRoot);
Assert(!discovery.Found && !discovery.Message.Contains("test-secret"), "Malformed settings fail without exposing configuration contents");
File.WriteAllText(configPath, "<!DOCTYPE Config [<!ENTITY key 'test-secret'>]><Config><ApiKey>&key;</ApiKey></Config>");
Assert(!LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878", discoveryRoot).Found, "Configuration retrieval prohibits XML document type declarations");
File.WriteAllText(configPath, "<Config><ApiKey>" + new string('x', 1_000_000) + "</ApiKey></Config>");
Assert(!LocalSetupDiscovery.FindApiKey(ApplicationId.Radarr, "http://localhost:7878", discoveryRoot).Found, "Oversized configuration files are rejected before parsing");
var pulsarrExecutable = Path.Combine(directory, "Pulsarr", "pulsarr-service.exe");
var supportedPulsarr = LocalSetupDiscovery.EvaluatePulsarrSetup(true, pulsarrExecutable, true, true, "\"" + pulsarrExecutable + "\"", "startafterinstall,firewall");
Assert(supportedPulsarr.IsSupported && supportedPulsarr.Message.Contains("no reinstall"), "A matching official Pulsarr service with saved options satisfies the requirement as-is");
Assert(!LocalSetupDiscovery.EvaluatePulsarrSetup(false, "", false, false, null, null).IsSupported, "Unregistered Pulsarr copies cannot claim automatic-update support");
Assert(!LocalSetupDiscovery.EvaluatePulsarrSetup(true, pulsarrExecutable, true, false, null, "").IsSupported, "A Pulsarr installation without its Windows service needs setup");
Assert(!LocalSetupDiscovery.EvaluatePulsarrSetup(true, pulsarrExecutable, true, true, "\"" + Path.Combine(directory, "Other", "pulsarr-service.exe") + "\"", "").IsSupported, "A service for a different Pulsarr copy cannot claim automatic-update support");
Assert(!LocalSetupDiscovery.EvaluatePulsarrSetup(true, pulsarrExecutable, true, true, "\"" + pulsarrExecutable + "\"", null).IsSupported, "Pulsarr support requires saved installation choices so updates can preserve them");

var handler = new FeedHandler();
var actualProvider = new UpdateProviders(new HttpClient(handler));
var connection = new ProgramSettings { Id = ApplicationId.Radarr, ServerUrl = "http://localhost:7878/radarr", ApiKey = "test-token" };
var result = await actualProvider.CheckAsync(connection, CancellationToken.None);
Assert(result.State == UpdateState.UpdateAvailable && handler.Paths.SequenceEqual(new[] { "/radarr/api/v3/system/status", "/radarr/api/v3/update" }), "Arr checks preserve URL base paths and recognize a newer release");
Assert(handler.Authenticated == 2 && handler.Posts == 0, "Read-only provider checks authenticate without posting an update command");
handler.Installable = false;
result = await actualProvider.CheckAsync(connection, CancellationToken.None);
Assert(result.State == UpdateState.NeedsSetup, "A newer release blocked by the app is not reported as up to date");
handler.Installable = true;
handler.AvailableVersion = "0.9.0";
result = await actualProvider.CheckAsync(connection, CancellationToken.None);
Assert(result.State == UpdateState.UpToDate, "A lower available version cannot trigger a downgrade");
handler.Docker = true;
var before = handler.Paths.Count;
result = await actualProvider.CheckAsync(connection, CancellationToken.None);
Assert(result.State == UpdateState.NeedsSetup && handler.Paths.Count == before + 1, "Container installations are identified and excluded from native updates");
handler.Docker = false;
handler.InstalledVersion = "invalid";
result = await actualProvider.CheckAsync(connection, CancellationToken.None);
Assert(result.State == UpdateState.Error, "Malformed server versions are rejected");
connection.ServerUrl = "http://user:secret@localhost:7878";
before = handler.Paths.Count;
result = await actualProvider.CheckAsync(connection, CancellationToken.None);
Assert(result.State == UpdateState.Error && handler.Paths.Count == before && !result.Message.Contains("secret"), "Embedded URL credentials are rejected before sending requests");
Console.WriteLine($"All {count} assertions passed. No real updates were installed.");
Directory.Delete(directory, true);

sealed class FakeProvider : IUpdateProvider
{
    public List<ApplicationId> Checked { get; } = new();
    public List<ApplicationId> Installed { get; } = new();
    public ApplicationId? FailId { get; init; }
    public TaskCompletionSource? Block { get; init; }
    public async Task<UpdateResult> CheckAsync(ProgramSettings settings, CancellationToken cancellationToken)
    {
        Checked.Add(settings.Id);
        if (Block is not null) await Block.Task.WaitAsync(cancellationToken);
        if (settings.Id == FailId) throw new Exception("https://secret@example.invalid/?apikey=secret");
        return new(settings.Id, UpdateState.UpdateAvailable, "1.0.0", "2.0.0", "Update available.");
    }
    public Task<UpdateResult> InstallAsync(ProgramSettings settings, UpdateResult update, CancellationToken cancellationToken)
    {
        Installed.Add(settings.Id);
        return Task.FromResult(update with { State = UpdateState.Updated, InstalledVersion = "2.0.0", Message = "Updated." });
    }
}

sealed class FeedHandler : HttpMessageHandler
{
    public string InstalledVersion { get; set; } = "1.0.0";
    public string AvailableVersion { get; set; } = "2.0.0";
    public bool Installable { get; set; } = true;
    public bool Docker { get; set; }
    public List<string> Paths { get; } = new();
    public int Authenticated { get; private set; }
    public int Posts { get; private set; }
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        Paths.Add(request.RequestUri!.AbsolutePath);
        if (request.Method == HttpMethod.Post) Posts++;
        if (request.Headers.TryGetValues("X-Api-Key", out var values) && values.Single() == "test-token") Authenticated++;
        object body = request.RequestUri.AbsolutePath.EndsWith("/status")
            ? new { version = InstalledVersion, isDocker = Docker, packageUpdateMechanism = "builtIn" }
            : new[] { new { version = AvailableVersion, latest = true, installable = Installable } };
        return Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new StringContent(System.Text.Json.JsonSerializer.Serialize(body)) });
    }
}
