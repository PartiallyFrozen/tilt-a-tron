using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Tiltatron.Manager.Link;

namespace Tiltatron.Manager;

public partial class MainWindow : Window
{
    /// <summary>The three kinds of thing that can be picked, and so acted on.</summary>
    enum Kind { WatchGame, LibraryGame, Theme }

    // What travels with a drag. Private names, so a drag from some other program is never
    // mistaken for one of ours.
    const string WatchGameFormat = "tiltatron/watch-game";
    const string LibraryGameFormat = "tiltatron/library-game";

    static readonly IBrush Clear = new SolidColorBrush(Colors.Transparent);
    static readonly IBrush Picked = new SolidColorBrush(Color.Parse("#FFD93D"));
    static readonly IBrush PaneEdge = new SolidColorBrush(Color.Parse("#3A4260"));
    static readonly IBrush DropEdge = new SolidColorBrush(Color.Parse("#28C86E"));
    static readonly IBrush Dim = new SolidColorBrush(Color.Parse("#8A97C0"));
    static readonly IBrush Warn = new SolidColorBrush(Color.Parse("#FFB347"));
    static readonly IBrush Good = new SolidColorBrush(Color.Parse("#28C86E"));

    readonly Library _library = new();
    Watch? _watch;
    bool _busy;
    (Kind Kind, string Key)? _pick;             // Key: a game id, a library file path, a theme name
    (Kind Kind, string Key)? _armed;            // what a second click of Remove will really delete
    readonly List<(Border Card, Kind Kind, string Key)> _cards = new();
    List<GameEntry> _watchGames = new();
    List<LibraryGame> _libraryGames = new();

    public MainWindow()
    {
        // Generated from the XAML: loads it and hands us the x:Name'd controls.
        InitializeComponent();
        ConnectButton.Click += (_, _) => _ = ConnectAsync();
        AddGamesButton.Click += (_, _) => _ = AddGamesAsync();
        SendThemeButton.Click += (_, _) => _ = SendThemeAsync();
        BackupButton.Click += (_, _) => _ = BackUpAsync();
        TransferButton.Click += (_, _) => _ = TransferAsync();
        RemoveButton.Click += (_, _) => _ = RemoveAsync();
        OpenFolderButton.Click += (_, _) => OpenLibraryFolder();
        LibraryPath.Text = _library.Folder;

        AcceptDrops(WatchPane, LibraryGameFormat, DropOnWatchAsync);
        AcceptDrops(LibraryPane, WatchGameFormat, DropOnLibraryAsync);

        Opened += (_, _) =>
        {
            // The library does not need a watch: it is there the moment the window is.
            RefreshLibrary();
            _ = ConnectAsync();
        };
    }

    // ------------------------------------------------------------------ connecting

    async Task ConnectAsync()
    {
        _watch?.Dispose();
        _watch = null;
        _watchGames = new();
        SetBusy("looking for a watch");
        ClearWatchSide();

        var found = await Task.Run(() => Watch.Find());
        if (found is null)
        {
            StatusLine.Text = "No watch found. Plug it in with a cable that carries data, and wake it.";
            ContentPanel.Children.Add(Note("Your library is still here. Plug a watch in to move games on and off it."));
            SetBusy(null);
            RefreshLibrary();
            UpdateButtons();
            return;
        }

        _watch = found;
        await RefreshAsync();
        SetBusy(null);
    }

    async Task RefreshAsync()
    {
        if (_watch is not { } w)
        {
            RefreshLibrary();
            return;
        }
        ClearWatchSide();

        var (total, free) = await Task.Run(() => w.StorageFree());
        StatusLine.Text = $"{w.PortName}  ·  firmware {w.Firmware}  ·  {w.Board}  ·  "
                          + $"{free / 1024} KB free of {total / 1024} KB";

        _watchGames = await Task.Run(() => w.Games());
        _libraryGames = await Task.Run(() => _library.Games());
        var kept = _libraryGames.Select(g => g.Package.Id).ToHashSet();

        ContentPanel.Children.Add(Heading("ON THE WATCH"));
        var grid = new WrapPanel { Orientation = Orientation.Horizontal };
        foreach (var g in _watchGames)
        {
            var icon = await Task.Run(() =>
            {
                try { return w.Icon(g.Id); }
                catch { return null; }
            });
            // An installed game that is nowhere else is the thing this window exists to
            // point out; one that is safely in the library needs no comment.
            var (tag, brush) = g.BuiltIn ? ("built in", Dim)
                : kept.Contains(g.Id) ? ($"{g.Bytes / 1024} KB", Dim)
                : ("not in library", Warn);
            var card = Tile(g.Name, Rgb565(g.Accent), icon, tag, brush, g.Hidden ? 0.35 : 1.0);
            // Only an installed game can be picked or dragged. A built-in is part of the
            // firmware: there is no file to copy and the most you can do is hide it.
            if (!g.BuiltIn) MakePickable(card, Kind.WatchGame, g.Id, WatchGameFormat);
            grid.Children.Add(card);
        }
        ContentPanel.Children.Add(grid);

        var themes = await Task.Run(() => w.ListFiles("Theme")
            .Where(f => f.IsDirectory)
            .Select(f => (f.Name, Size: FolderSize(w, $"Theme/{f.Name}")))
            .ToList());
        ContentPanel.Children.Add(Heading("THEMES"));
        var list = new StackPanel { Spacing = 5 };
        foreach (var t in themes) list.Children.Add(ThemeCard(t.Name, t.Size));
        ContentPanel.Children.Add(list);

        RefreshLibrary();
    }

    void ClearWatchSide()
    {
        if (_pick?.Kind != Kind.LibraryGame) _pick = null;
        _armed = null;
        _cards.RemoveAll(c => c.Kind != Kind.LibraryGame);
        ContentPanel.Children.Clear();
    }

    void RefreshLibrary()
    {
        if (_pick?.Kind == Kind.LibraryGame) _pick = null;
        _armed = null;
        _cards.RemoveAll(c => c.Kind == Kind.LibraryGame);
        LibraryPanel.Children.Clear();

        _libraryGames = _library.Games();
        if (_libraryGames.Count == 0)
        {
            LibraryPanel.Children.Add(Note(
                "Nothing here yet.\n\nDrag a game across from the watch to keep a copy, or drop .tat files "
                + "here. Anything in the library can be put back on a watch whenever you like."));
        }
        else
        {
            var installed = _watchGames.Where(g => !g.BuiltIn).Select(g => g.Id).ToHashSet();
            var grid = new WrapPanel { Orientation = Orientation.Horizontal };
            foreach (var g in _libraryGames)
            {
                var p = g.Package;
                var on = installed.Contains(p.Id);
                var card = Tile(p.Name, Color.FromRgb(p.AccentR, p.AccentG, p.AccentB), p.IconPng,
                                on ? "on the watch" : $"v{p.Version} · {p.Bytes / 1024} KB", on ? Good : Dim, 1.0);
                ToolTip.SetTip(card, $"{p.Name} v{p.Version} by {p.Author}\n{Path.GetFileName(g.Path)}");
                MakePickable(card, Kind.LibraryGame, g.Path, LibraryGameFormat);
                grid.Children.Add(card);
            }
            LibraryPanel.Children.Add(grid);
        }
        UpdateButtons();
    }

    // ------------------------------------------------------------------ actions

    /// <summary>Picks .tat files and files them in the library. Nothing is sent anywhere.</summary>
    async Task AddGamesAsync()
    {
        var picked = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Pick games to add to your library",
            AllowMultiple = true,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("Tilt-a-tron game") { Patterns = new[] { "*.tat" } },
            },
        });
        if (picked.Count == 0) return;
        AddFiles(picked.Select(f => f.Path.LocalPath));
        RefreshLibrary();
    }

    /// <summary>Files each one; says what went wrong with any that are not games.</summary>
    List<LibraryGame> AddFiles(IEnumerable<string> paths)
    {
        var added = new List<LibraryGame>();
        var refused = new List<string>();
        foreach (var path in paths)
        {
            try { added.Add(_library.AddFile(path)); }
            catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException)
            {
                refused.Add($"{Path.GetFileName(path)}: {e.Message}");
            }
        }
        SetBusy(refused.Count > 0 ? "not added - " + string.Join("; ", refused)
            : added.Count == 1 ? $"{added[0].Package.Name} is in your library"
            : $"{added.Count} games added to your library");
        return added;
    }

    /// <summary>The button that does what a drag does, whichever way the pick points.</summary>
    Task TransferAsync() => _pick switch
    {
        (Kind.WatchGame, var id) => KeepAsync(id),
        (Kind.LibraryGame, var path) => InstallAsync(new[] { path }),
        _ => Task.CompletedTask,
    };

    /// <summary>Watch to library. The game stays on the watch.</summary>
    async Task KeepAsync(string id)
    {
        if (_watch is not { } w) return;
        var name = NameOf(id);
        LibraryGame? saved = null;
        if (!await RunAsync($"copying {name} to your library", () => saved = _library.SaveFromWatch(w, id))) return;
        await RefreshAsync();
        SetBusy($"{name} is in your library");
    }

    /// <summary>
    /// Library to watch. Installing is writing the file to the games folder - there is no
    /// separate install command, and the watch's home screen notices by itself. Whether
    /// this watch can run it is checked before the transfer, not after it.
    /// </summary>
    async Task InstallAsync(IEnumerable<string> libraryPaths)
    {
        if (_watch is not { } w) return;
        var games = _library.Games().Where(g => libraryPaths.Contains(g.Path)).ToList();
        if (games.Count == 0) return;

        var ok = await RunAsync(games.Count == 1 ? $"installing {games[0].Package.Name}" : "installing", () =>
        {
            foreach (var g in games)
                Library.Install(w, g, (done, total) =>
                    Report($"{g.Package.Name}  {done / 1024} of {total / 1024} KB",
                           total == 0 ? 0 : done * 100.0 / total));
        });
        await RefreshAsync();
        if (ok) SetBusy($"{string.Join(", ", games.Select(g => g.Package.Name))} installed - "
                        + "it is on the watch's home screen now");
    }

    async Task SendThemeAsync()
    {
        if (_watch is not { } w) return;
        var picked = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Pick a theme folder to send",
            AllowMultiple = false,
        });
        if (picked.Count == 0) return;
        var local = picked[0].Path.LocalPath;
        var name = new DirectoryInfo(local).Name;

        await RunAsync($"sending {name}", () =>
            w.SendFolder(local, $"Theme/{name}", (file, done, total) =>
                Report($"{name}/{file}", total == 0 ? 0 : done * 100.0 / total)));
        await RefreshAsync();
    }

    async Task BackUpAsync()
    {
        if (_watch is not { } w) return;
        var picked = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Where should the backup go?",
            AllowMultiple = false,
        });
        if (picked.Count == 0) return;
        var root = Path.Combine(picked[0].Path.LocalPath, "tiltatron-backup");

        if (await RunAsync("backing up", () => BackUpFolder(w, "", root)))
            SetBusy($"backed up to {root}");
    }

    static void BackUpFolder(Watch w, string remote, string localDir)
    {
        Directory.CreateDirectory(localDir);
        foreach (var e in w.ListFiles(remote))
        {
            var path = remote.Length == 0 ? e.Name : $"{remote}/{e.Name}";
            if (e.IsDirectory)
            {
                if (e.Name == "System Volume Information") continue;
                BackUpFolder(w, path, Path.Combine(localDir, e.Name));
            }
            else
            {
                File.WriteAllBytes(Path.Combine(localDir, e.Name), w.ReadFile(path));
            }
        }
    }

    /// <summary>
    /// Remove never loses anything on the first click. A game leaves the watch by moving to
    /// the library; a theme is copied beside the library first. The only things that are
    /// really deleted are a library game and a game too damaged to keep, and both of those
    /// say so and wait to be asked again.
    /// </summary>
    async Task RemoveAsync()
    {
        if (_pick is not { } pick) return;
        var confirmed = _armed == pick;
        _armed = null;

        switch (pick.Kind)
        {
            case Kind.WatchGame when _watch is { } w:
            {
                var name = NameOf(pick.Key);
                if (confirmed)
                {
                    if (await RunAsync($"deleting {name}", () => w.Delete(Library.ReadFromWatch(w, pick.Key).Path)))
                    {
                        await RefreshAsync();
                        SetBusy($"{name} deleted from the watch");
                    }
                    return;
                }
                var damaged = false;
                var ok = await RunAsync($"moving {name} to your library", () =>
                {
                    try { _library.MoveFromWatch(w, pick.Key); }
                    catch (InvalidDataException) { damaged = true; throw; }
                });
                if (damaged)
                {
                    Arm(pick, $"{name} is damaged and cannot be kept. Click Delete to remove it anyway.");
                    return;
                }
                await RefreshAsync();
                if (ok) SetBusy($"{name} is off the watch and safe in your library");
                return;
            }
            case Kind.LibraryGame:
            {
                var game = _libraryGames.FirstOrDefault(g => g.Path == pick.Key);
                if (game is null) return;
                if (!confirmed)
                {
                    var onWatch = _watchGames.Any(g => !g.BuiltIn && g.Id == game.Package.Id);
                    Arm(pick, onWatch
                        ? $"Delete {game.Package.Name} from this computer? It stays on the watch. Click Delete to confirm."
                        : $"Delete {game.Package.Name} for good? It is not on the watch either. Click Delete to confirm.");
                    return;
                }
                try
                {
                    _library.Remove(game);
                    SetBusy($"{game.Package.Name} deleted from your library");
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException)
                {
                    SetBusy($"could not delete it: {e.Message}");
                }
                RefreshLibrary();
                return;
            }
            case Kind.Theme when _watch is { } w:
            {
                var name = pick.Key;
                var kept = "";
                var ok = await RunAsync($"keeping a copy of {name}", () =>
                {
                    kept = FreshFolder(Path.Combine(Path.GetDirectoryName(_library.Folder)!, "Themes"), name);
                    BackUpFolder(w, $"Theme/{name}", kept);
                    Report($"removing {name}", 100);
                    w.Delete($"Theme/{name}");
                });
                await RefreshAsync();
                if (ok) SetBusy($"{name} removed; a copy is in {kept}");
                return;
            }
        }
    }

    /// <summary>A folder under <paramref name="root"/> that does not exist yet, so keeping a
    /// copy of something never writes over an earlier copy of it.</summary>
    static string FreshFolder(string root, string name)
    {
        for (var n = 1; ; n++)
        {
            var path = Path.Combine(root, n == 1 ? name : $"{name} ({n})");
            if (!Directory.Exists(path) && !File.Exists(path)) return path;
        }
    }

    void Arm((Kind, string) pick, string question)
    {
        _armed = pick;
        SetBusy(question);
        UpdateButtons();
    }

    void OpenLibraryFolder()
    {
        try
        {
            Directory.CreateDirectory(_library.Folder);
            Process.Start(new ProcessStartInfo(_library.Folder) { UseShellExecute = true });
        }
        catch (Exception e)
        {
            SetBusy($"could not open {_library.Folder}: {e.Message}");
        }
    }

    // ------------------------------------------------------------------ drag and drop

    /// <summary>
    /// Makes a pane somewhere things can be dropped: one of our own drags from the other
    /// pane, or .tat files from the desktop. The pane's edge lights up while something it
    /// would accept is over it, which is the only feedback a drag gets before it lands.
    /// </summary>
    void AcceptDrops(Border pane, string ownFormat, Func<DragEventArgs, Task> dropped)
    {
        DragDrop.SetAllowDrop(pane, true);
        bool Wanted(DragEventArgs e) =>
            !_busy && (e.Data.Contains(ownFormat) || DroppedPackages(e).Count > 0)
                   && (pane != WatchPane || _watch is not null);

        pane.AddHandler(DragDrop.DragOverEvent, (_, e) =>
        {
            e.DragEffects = Wanted(e) ? DragDropEffects.Copy : DragDropEffects.None;
            pane.BorderBrush = Wanted(e) ? DropEdge : PaneEdge;
        });
        pane.AddHandler(DragDrop.DragLeaveEvent, (_, _) => pane.BorderBrush = PaneEdge);
        pane.AddHandler(DragDrop.DropEvent, (_, e) =>
        {
            pane.BorderBrush = PaneEdge;
            if (Wanted(e)) _ = dropped(e);
        });
    }

    static List<string> DroppedPackages(DragEventArgs e) =>
        (e.Data.GetFiles() ?? Enumerable.Empty<IStorageItem>())
        .Select(f => f.TryGetLocalPath())
        .Where(p => p is not null && p.EndsWith(".tat", StringComparison.OrdinalIgnoreCase))
        .Select(p => p!)
        .ToList();

    Task DropOnLibraryAsync(DragEventArgs e)
    {
        if (e.Data.Get(WatchGameFormat) is string id) return KeepAsync(id);
        AddFiles(DroppedPackages(e));
        RefreshLibrary();
        return Task.CompletedTask;
    }

    /// <summary>A file dropped straight onto the watch goes through the library on its way,
    /// so that nothing is ever on a watch and nowhere else.</summary>
    Task DropOnWatchAsync(DragEventArgs e)
    {
        if (e.Data.Get(LibraryGameFormat) is string path) return InstallAsync(new[] { path });
        var added = AddFiles(DroppedPackages(e));
        RefreshLibrary();
        return added.Count == 0 ? Task.CompletedTask : InstallAsync(added.Select(g => g.Path).ToList());
    }

    /// <summary>
    /// Click to pick, drag to move. The drag only starts once the pointer has travelled a
    /// little with the button down, so an unsteady click is still a click.
    /// </summary>
    void MakePickable(Border card, Kind kind, string key, string? dragFormat)
    {
        Point? pressedAt = null;
        card.Cursor = new Cursor(StandardCursorType.Hand);
        card.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(card).Properties.IsLeftButtonPressed) return;
            pressedAt = e.GetPosition(card);
            Pick(kind, key);
        };
        card.PointerReleased += (_, _) => pressedAt = null;
        _cards.Add((card, kind, key));
        if (dragFormat is null) return;
        card.PointerMoved += async (_, e) =>
        {
            if (pressedAt is not { } from || _busy) return;
            var now = e.GetPosition(card);
            if (Math.Abs(now.X - from.X) < 6 && Math.Abs(now.Y - from.Y) < 6) return;
            pressedAt = null;
            var data = new DataObject();
            data.Set(dragFormat, key);
            await DragDrop.DoDragDrop(e, data, DragDropEffects.Copy);
        };
    }

    void Pick(Kind kind, string key)
    {
        _pick = (kind, key);
        _armed = null;
        foreach (var c in _cards)
            c.Card.BorderBrush = c.Kind == kind && c.Key == key ? Picked : Clear;
        UpdateButtons();
    }

    // ------------------------------------------------------------------ bits of screen

    /// <summary>Runs something slow off the UI thread with the progress bar showing.
    /// Returns whether it worked; if it did not, the reason is already on screen.</summary>
    async Task<bool> RunAsync(string what, Action work)
    {
        _busy = true;
        UpdateButtons();
        SetBusy(what);
        Progress.IsVisible = true;
        Progress.Value = 0;
        try
        {
            await Task.Run(work);
            SetBusy(null);
            return true;
        }
        catch (Exception e)
        {
            SetBusy($"{what} failed: {e.Message}");
            return false;
        }
        finally
        {
            Progress.IsVisible = false;
            _busy = false;
            UpdateButtons();
        }
    }

    void Report(string what, double percent) =>
        Dispatcher.UIThread.Post(() =>
        {
            BusyLine.Text = what;
            Progress.Value = percent;
        });

    void SetBusy(string? what) => BusyLine.Text = what ?? "";

    string NameOf(string id) => _watchGames.FirstOrDefault(g => g.Id == id)?.Name ?? id;

    void UpdateButtons()
    {
        var watch = _watch is not null && !_busy;
        AddGamesButton.IsEnabled = !_busy;
        SendThemeButton.IsEnabled = watch;
        BackupButton.IsEnabled = watch;

        (TransferButton.Content, TransferButton.IsEnabled) = _pick?.Kind switch
        {
            Kind.WatchGame => ("Copy to library  →", watch),
            Kind.LibraryGame => ("←  Install on watch", watch),
            _ => ("Copy", false),
        };
        RemoveButton.Content = _armed is not null ? "Delete"
            : _pick?.Kind == Kind.WatchGame ? "Move to library"
            : "Remove";
        RemoveButton.IsEnabled = !_busy && _pick?.Kind switch
        {
            Kind.LibraryGame => true,
            Kind.WatchGame or Kind.Theme => _watch is not null,
            _ => false,
        };
    }

    static TextBlock Heading(string text) => new()
    {
        Text = text,
        FontSize = 12,
        FontWeight = FontWeight.Bold,
        Foreground = new SolidColorBrush(Color.Parse("#7FE8FF")),
    };

    static TextBlock Note(string text) => new()
    {
        Text = text,
        FontSize = 12,
        TextWrapping = TextWrapping.Wrap,
        Foreground = Dim,
    };

    /// <summary>One game, on either side: its picture, its name, and one line about it.</summary>
    static Border Tile(string name, Color accent, byte[]? iconPng, string tag, IBrush tagBrush, double opacity)
    {
        var stack = new StackPanel { Width = 84, Spacing = 3, Opacity = opacity };
        Control? picture = null;
        if (iconPng is not null)
        {
            try
            {
                picture = new Image { Source = new Bitmap(new MemoryStream(iconPng)), Width = 56, Height = 56 };
            }
            catch
            {
                // a picture we can't decode isn't worth failing the window over
            }
        }
        // A game that packed no icon still gets a face: its own colour and its initial.
        picture ??= new Border
        {
            Width = 56,
            Height = 56,
            CornerRadius = new CornerRadius(28),
            Background = new SolidColorBrush(accent, 0.18),
            BorderBrush = new SolidColorBrush(accent),
            BorderThickness = new Thickness(2),
            HorizontalAlignment = HorizontalAlignment.Center,
            Child = new TextBlock
            {
                Text = name.Length > 0 ? name[..1] : "?",
                FontSize = 24,
                FontWeight = FontWeight.Bold,
                Foreground = new SolidColorBrush(accent),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            },
        };
        stack.Children.Add(picture);
        stack.Children.Add(new TextBlock
        {
            Text = name,
            FontSize = 11,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(accent),
        });
        stack.Children.Add(new TextBlock
        {
            Text = tag,
            FontSize = 10,
            TextAlignment = TextAlignment.Center,
            Foreground = tagBrush,
        });

        return new Border
        {
            Background = Clear,          // so the whole tile takes the pointer, not just its ink
            BorderBrush = Clear,
            BorderThickness = new Thickness(2),
            CornerRadius = new CornerRadius(3),
            Padding = new Thickness(4),
            Margin = new Thickness(0, 0, 8, 8),
            Child = stack,
        };
    }

    static uint FolderSize(Watch w, string path)
    {
        uint total = 0;
        foreach (var e in w.ListFiles(path))
            total += e.IsDirectory ? FolderSize(w, $"{path}/{e.Name}") : e.Size;
        return total;
    }

    Control ThemeCard(string name, uint bytes)
    {
        var card = new Border
        {
            Background = new SolidColorBrush(Color.Parse("#22252F")),
            BorderBrush = Clear,
            BorderThickness = new Thickness(2),
            CornerRadius = new CornerRadius(3),
            Padding = new Thickness(10, 8),
            Child = new Grid
            {
                ColumnDefinitions = new ColumnDefinitions("*,Auto"),
                Children =
                {
                    new TextBlock { Text = name, Foreground = new SolidColorBrush(Color.Parse("#EAF0FF")) },
                    new TextBlock
                    {
                        Text = $"{bytes / 1024} KB",
                        [Grid.ColumnProperty] = 1,
                        Foreground = Dim,
                    },
                },
            },
        };
        MakePickable(card, Kind.Theme, name, null);
        return card;
    }

    /// <summary>The watch speaks RGB565 in the panel's byte order.</summary>
    static Color Rgb565(ushort c) =>
        Color.FromRgb((byte)((c >> 11) << 3), (byte)(((c >> 5) & 0x3F) << 2), (byte)((c & 0x1F) << 3));
}
