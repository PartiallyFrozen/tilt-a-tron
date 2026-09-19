using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using Avalonia;
using Tiltatron.Manager.Link;

namespace Tiltatron.Manager;

static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        // Run with arguments and it behaves like a command line tool instead of opening a
        // window - handy for scripting, and it is how the protocol gets tested without a
        // person watching. Same binary either way.
        if (args.Length > 0) return Cli(args);

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        return 0;
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().LogToTrace();

    // ------------------------------------------------------------------ command line

    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int processId);

    static int Cli(string[] args)
    {
        // A GUI binary on Windows has no console of its own; borrow the one it was
        // started from so output actually appears.
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) AttachConsole(-1);

        try
        {
            switch (args[0])
            {
                case "list":
                {
                    using var w = Connect();
                    var (total, free) = w.StorageFree();
                    Console.WriteLine($"Tilt-a-tron on {w.PortName}");
                    Console.WriteLine($"  firmware {w.Firmware}   board {w.Board}   game API {w.GameApi.Major}.{w.GameApi.Minor}");
                    Console.WriteLine($"  storage {free / 1024} KB free of {total / 1024} KB");
                    Console.WriteLine();
                    foreach (var g in w.Games())
                        Console.WriteLine($"  {g.Name,-14} {(g.BuiltIn ? "built in" : $"{g.Bytes / 1024} KB"),-10} {(g.Hidden ? "hidden" : "on")}");
                    foreach (var t in w.ListFiles("Theme"))
                        Console.WriteLine($"  theme: {t.Name}");
                    return 0;
                }
                case "library" when args.Length == 1:
                {
                    var lib = new Library();
                    Console.WriteLine($"library: {lib.Folder}");
                    foreach (var g in lib.Games())
                        Console.WriteLine($"  {g.Package.Name,-14} {g.Package.Id,-14} v{g.Package.Version,-4} "
                                          + $"{g.Package.Bytes / 1024,4} KB  {Path.GetFileName(g.Path)}");
                    return 0;
                }
                case "add" when args.Length == 2:
                {
                    var g = new Library().AddFile(args[1]);
                    Console.WriteLine($"{g.Package.Name} is in the library: {g.Path}");
                    return 0;
                }
                case "save" when args.Length == 2:
                {
                    // Watch to library; the game stays installed.
                    using var w = Connect();
                    var g = new Library().SaveFromWatch(w, args[1]);
                    Console.WriteLine($"{g.Package.Name} saved to {g.Path}");
                    return 0;
                }
                case "install" when args.Length == 2:
                {
                    // Either a game already in the library, by id, or a file - which goes
                    // into the library on its way, so that everything on a watch is also
                    // somewhere safe.
                    var lib = new Library();
                    var game = File.Exists(args[1])
                        ? lib.AddFile(args[1])
                        : lib.Games().FirstOrDefault(g => g.Package.Id == args[1])
                          ?? throw new FileNotFoundException($"{args[1]} is not a file and not in the library");
                    using var w = Connect();
                    Console.WriteLine($"installing {game.Package.Name} ({game.Package.Id}) "
                                      + $"v{game.Package.Version} by {game.Package.Author}");
                    Library.Install(w, game, (done, total) => Console.Write($"\r  {done / 1024} of {total / 1024} KB"));
                    Console.WriteLine();
                    Console.WriteLine("done - it is on the watch's home screen now");
                    return 0;
                }
                case "uninstall" when args.Length == 2:
                {
                    // Off the watch and into the library, in that order of safety: see
                    // Library.MoveFromWatch.
                    using var w = Connect();
                    var g = new Library().MoveFromWatch(w, args[1]);
                    Console.WriteLine($"{g.Package.Name} removed from the watch and kept in {g.Path}");
                    return 0;
                }
                case "send" when args.Length == 3:
                {
                    using var w = Connect();
                    Console.WriteLine($"sending {args[1]} -> {args[2]}");
                    w.SendFolder(args[1], args[2],
                        (file, done, total) => Console.Write($"\r  {file} {done * 100 / Math.Max(1, total)}%   "));
                    Console.WriteLine("\ndone");
                    return 0;
                }
                case "remove" when args.Length == 2:
                {
                    using var w = Connect();
                    w.Delete(args[1]);
                    Console.WriteLine($"deleted {args[1]}");
                    return 0;
                }
                case "bench":
                {
                    using var w = Connect();
                    // Where does a transfer's time actually go: per call, or per byte?
                    foreach (var size in new[] { 256, 1024, 2048, 4096 })
                    {
                        var data = new byte[size * 20];
                        var sw = System.Diagnostics.Stopwatch.StartNew();
                        w.WriteFileChunked("Theme/.bench", data, size);
                        sw.Stop();
                        Console.WriteLine($"  {size,5} byte chunks x20: {sw.ElapsedMilliseconds,6} ms"
                                          + $"  ({data.Length * 1000.0 / 1024 / sw.ElapsedMilliseconds,6:n1} KB/s,"
                                          + $" {sw.ElapsedMilliseconds / 20.0,5:n1} ms per call)");
                    }
                    w.Delete("Theme/.bench");
                    return 0;
                }
                default:
                    Console.WriteLine("""
                        Tilt-a-tron manager

                          tiltatron-manager                        open the window
                          tiltatron-manager list                   what is on the watch
                          tiltatron-manager library                what is in your library
                          tiltatron-manager add <file.tat>         put a game in the library
                          tiltatron-manager install <id|file.tat>  library to watch
                          tiltatron-manager save <id>              watch to library, keeping it installed
                          tiltatron-manager uninstall <id>         off the watch, kept in the library
                          tiltatron-manager send <folder> <remote> copy a folder to it
                          tiltatron-manager remove <remote>        delete a file or folder
                        """);
                    return 1;
            }
        }
        catch (Exception e)
        {
            Console.Error.WriteLine($"error: {e.Message}");
            return 1;
        }
    }

    static Watch Connect() =>
        Watch.Find() ?? throw new IOException("no watch found - is it plugged in with a data cable, and awake?");
}
