using System;
using System.IO;
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
