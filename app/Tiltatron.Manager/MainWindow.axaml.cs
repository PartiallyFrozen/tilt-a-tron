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
    readonly List<Border> _themeCards = new();

    public MainWindow()
    {
        // Generated from the XAML: loads it and hands us the x:Name'd controls.
        InitializeComponent();
        ConnectButton.Click += (_, _) => _ = ConnectAsync();
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
        _themeCards.Clear();
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

        var themes = await Task.Run(() => w.ListFiles("Theme").Where(f => f.IsDirectory).ToList());
        ContentPanel.Children.Add(Heading("THEMES"));
        var list = new StackPanel { Spacing = 6 };
        foreach (var t in themes) list.Children.Add(ThemeCard(t.Name));
        ContentPanel.Children.Add(list);

        EnableActions(true);
    }

    // ------------------------------------------------------------------ actions

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
        if (_watch is not { } w || _selected is null) return;
        var name = _selected;
        await RunAsync($"removing {name}", () => w.Delete($"Theme/{name}"));
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
        SendThemeButton.IsEnabled = on;
        BackupButton.IsEnabled = on;
        RemoveButton.IsEnabled = on && _selected is not null;
    }

    static TextBlock Heading(string text) => new()
    {
        Text = text,
        FontSize = 12,
        FontWeight = FontWeight.Bold,
        Foreground = new SolidColorBrush(Color.Parse("#7FE8FF")),
    };

    static Control GameTile(GameEntry g, byte[]? iconPng)
    {
        var stack = new StackPanel { Width = 96, Spacing = 6, Margin = new Avalonia.Thickness(0, 0, 10, 10) };
        if (iconPng is not null)
        {
            try
            {
                stack.Children.Add(new Image
                {
                    Source = new Bitmap(new MemoryStream(iconPng)),
                    Width = 72,
                    Height = 72,
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
        return stack;
    }

    Control ThemeCard(string name)
    {
        var card = new Border
        {
            Background = new SolidColorBrush(Color.Parse("#22252F")),
            BorderBrush = new SolidColorBrush(Colors.Transparent),
            BorderThickness = new Avalonia.Thickness(2),
            CornerRadius = new Avalonia.CornerRadius(3),
            Padding = new Avalonia.Thickness(10, 8),
            Child = new TextBlock { Text = name, Foreground = new SolidColorBrush(Color.Parse("#EAF0FF")) },
        };
        card.PointerPressed += (_, _) =>
        {
            _selected = name;
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
