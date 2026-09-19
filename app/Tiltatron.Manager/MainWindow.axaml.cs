using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Markup.Xaml;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Tiltatron.Manager.Link;

namespace Tiltatron.Manager;

public partial class MainWindow : Window
{
    Watch? _watch;
    string? _selected;          // the theme folder currently picked, if any
    string? _selectedGame;      // the installed game currently picked, by its id
    readonly List<Border> _themeCards = new();
    readonly List<Border> _gameCards = new();

    public MainWindow()
    {
        // Generated from the XAML: loads it and hands us the x:Name'd controls.
        InitializeComponent();
        ConnectButton.Click += (_, _) => _ = ConnectAsync();
        InstallGameButton.Click += (_, _) => _ = InstallGameAsync();
        SendThemeButton.Click += (_, _) => _ = SendThemeAsync();
        BackupButton.Click += (_, _) => _ = BackUpAsync();
        RemoveButton.Click += (_, _) => _ = RemoveAsync();
        Opened += (_, _) => _ = ConnectAsync();
    }

    // ------------------------------------------------------------------ connecting

    async Task ConnectAsync()
    {
        _watch?.Dispose();
        _watch = null;
        SetBusy("looking for a watch");
        ContentPanel.Children.Clear();

        var found = await Task.Run(() => Watch.Find());
        if (found is null)
        {
            StatusLine.Text = "No watch found. Plug it in with a cable that carries data, and wake it.";
            SetBusy(null);
            EnableActions(false);
            return;
        }

        _watch = found;
        var (total, free) = await Task.Run(() => found.StorageFree());
        StatusLine.Text = $"{found.PortName}  ·  firmware {found.Firmware}  ·  {found.Board}  ·  "
                          + $"{free / 1024} KB free of {total / 1024} KB";
        await RefreshAsync();
        SetBusy(null);
    }

    async Task RefreshAsync()
    {
        if (_watch is not { } w) return;
        _selected = null;
        _selectedGame = null;
        _themeCards.Clear();
        _gameCards.Clear();
        ContentPanel.Children.Clear();

        var games = await Task.Run(() => w.Games());
        ContentPanel.Children.Add(Heading("GAMES"));
        var grid = new WrapPanel { Orientation = Orientation.Horizontal };
        foreach (var g in games)
        {
            var icon = await Task.Run(() =>
            {
                try { return w.Icon(g.Id); }
                catch { return null; }
            });
            grid.Children.Add(GameTile(g, icon));
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

        EnableActions(true);
    }

    // ------------------------------------------------------------------ actions

    /// <summary>
    /// Installing a game is writing its file to the games folder - there is no separate
    /// install command, and the watch picks it up next time it starts. The header is read
    /// here first so that a file which is not a package, or is built for a firmware this
    /// watch does not have, is refused on this side rather than after a slow transfer.
    /// </summary>
    async Task InstallGameAsync()
    {
        if (_watch is not { } w) return;
        var picked = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Pick a game to install",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("Tilt-a-tron game") { Patterns = new[] { "*.tat" } },
            },
        });
        if (picked.Count == 0) return;
        var local = picked[0].Path.LocalPath;

        TatPackage pkg;
        try
        {
            pkg = TatPackage.Read(local);
        }
        catch (Exception e)
        {
            SetBusy($"that file is not a game: {e.Message}");
            return;
        }
        if (!pkg.RunsOn(w.GameApi.Major, w.GameApi.Minor))
        {
            SetBusy($"{pkg.Name} needs game API {pkg.ApiMajor}.{pkg.ApiMinor}; "
                    + $"this watch has {w.GameApi.Major}.{w.GameApi.Minor}");
            return;
        }

        // Always under the game's own id, so removing it later is just deleting that name.
        var remote = $"Games/{pkg.Id}.tat";
        await RunAsync($"installing {pkg.Name}", () =>
        {
            w.MakeFolder("Games");
            w.WriteFile(remote, File.ReadAllBytes(local), (done, total) =>
                Report($"{pkg.Name}  {done / 1024} of {total / 1024} KB",
                       total == 0 ? 0 : done * 100.0 / total));
        });
        SetBusy($"{pkg.Name} installed - restart the watch to see it on the home screen");
        await RefreshAsync();
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

        await RunAsync("backing up", () => BackUpFolder(w, "", root));
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

    async Task RemoveAsync()
    {
        if (_watch is not { } w) return;
        if (_selectedGame is { } id)
        {
            // Uninstalling is deleting the file. The game's saves stay in the watch's
            // settings store, so installing it again finds your scores where you left them.
            await RunAsync($"removing {id}", () => w.Delete($"Games/{id}.tat"));
            SetBusy("removed - restart the watch to take it off the home screen");
        }
        else if (_selected is { } name)
        {
            await RunAsync($"removing {name}", () => w.Delete($"Theme/{name}"));
        }
        else
        {
            return;
        }
        await RefreshAsync();
    }

    /// <summary>Runs something slow off the UI thread with the progress bar showing.</summary>
    async Task RunAsync(string what, Action work)
    {
        EnableActions(false);
        SetBusy(what);
        Progress.IsVisible = true;
        Progress.Value = 0;
        try
        {
            await Task.Run(work);
            SetBusy(null);
        }
        catch (Exception e)
        {
            SetBusy($"{what} failed: {e.Message}");
        }
        finally
        {
            Progress.IsVisible = false;
            EnableActions(_watch is not null);
        }
    }

    void Report(string what, double percent) =>
        Dispatcher.UIThread.Post(() =>
        {
            BusyLine.Text = what;
            Progress.Value = percent;
        });

    // ------------------------------------------------------------------ bits of screen

    void SetBusy(string? what) => BusyLine.Text = what ?? "";

    void EnableActions(bool on)
    {
        InstallGameButton.IsEnabled = on;
        SendThemeButton.IsEnabled = on;
        BackupButton.IsEnabled = on;
        RemoveButton.IsEnabled = on && (_selected is not null || _selectedGame is not null);
    }

    static TextBlock Heading(string text) => new()
    {
        Text = text,
        FontSize = 12,
        FontWeight = FontWeight.Bold,
        Foreground = new SolidColorBrush(Color.Parse("#7FE8FF")),
    };

    Control GameTile(GameEntry g, byte[]? iconPng)
    {
        var stack = new StackPanel { Width = 78, Spacing = 3 };
        if (iconPng is not null)
        {
            try
            {
                stack.Children.Add(new Image
                {
                    Source = new Bitmap(new MemoryStream(iconPng)),
                    Width = 56,
                    Height = 56,
                });
            }
            catch
            {
                // a picture we can't decode isn't worth failing the window over
            }
        }
        stack.Children.Add(new TextBlock
        {
            Text = g.Name,
            FontSize = 11,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(Rgb565(g.Accent)),
            Opacity = g.Hidden ? 0.35 : 1.0,
        });
        stack.Children.Add(new TextBlock
        {
            Text = g.BuiltIn ? "built in" : $"{g.Bytes / 1024} KB",
            FontSize = 10,
            TextAlignment = TextAlignment.Center,
            Foreground = new SolidColorBrush(Color.Parse("#8A97C0")),
        });

        var card = new Border
        {
            BorderBrush = new SolidColorBrush(Colors.Transparent),
            BorderThickness = new Avalonia.Thickness(2),
            CornerRadius = new Avalonia.CornerRadius(3),
            Padding = new Avalonia.Thickness(4),
            Margin = new Avalonia.Thickness(0, 0, 8, 8),
            Child = stack,
        };
        // Only an installed game can be picked, because only an installed game can be
        // removed. A built-in is part of the firmware and the most you can do is hide it.
        if (!g.BuiltIn)
        {
            card.PointerPressed += (_, _) =>
            {
                _selectedGame = g.Id;
                _selected = null;
                foreach (var c in _themeCards) c.BorderBrush = new SolidColorBrush(Colors.Transparent);
                foreach (var c in _gameCards)
                    c.BorderBrush = new SolidColorBrush(c == card ? Color.Parse("#FFD93D") : Colors.Transparent);
                RemoveButton.IsEnabled = true;
            };
            _gameCards.Add(card);
        }
        return card;
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
            BorderBrush = new SolidColorBrush(Colors.Transparent),
            BorderThickness = new Avalonia.Thickness(2),
            CornerRadius = new Avalonia.CornerRadius(3),
            Padding = new Avalonia.Thickness(10, 8),
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
                        Foreground = new SolidColorBrush(Color.Parse("#8A97C0")),
                    },
                },
            },
        };
        card.PointerPressed += (_, _) =>
        {
            _selected = name;
            _selectedGame = null;
            foreach (var c in _gameCards) c.BorderBrush = new SolidColorBrush(Colors.Transparent);
            foreach (var c in _themeCards)
                c.BorderBrush = new SolidColorBrush(c == card ? Color.Parse("#FFD93D") : Colors.Transparent);
            RemoveButton.IsEnabled = true;
        };
        _themeCards.Add(card);
        return card;
    }

    /// <summary>The watch speaks RGB565 in the panel's byte order.</summary>
    static Color Rgb565(ushort c) =>
        Color.FromRgb((byte)((c >> 11) << 3), (byte)(((c >> 5) & 0x3F) << 2), (byte)((c & 0x1F) << 3));
}
