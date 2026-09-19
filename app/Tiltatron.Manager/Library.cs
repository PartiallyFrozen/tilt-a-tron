using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Tiltatron.Manager.Link;

namespace Tiltatron.Manager;

/// <summary>One game in the library: what its header says, and where the file is.</summary>
public sealed record LibraryGame(TatPackage Package, string Path);

/// <summary>
/// The games you own, whether or not they are on a watch right now.
///
/// A watch holds a handful of games and removing one deletes it, so without somewhere else
/// to keep them, tidying the watch means losing things. The library is that somewhere: a
/// plain folder of .tat files in your documents. There is no database and nothing hidden -
/// a game you copy into the folder by hand is in the library, and a game you copy out of
/// it is a file you can give to someone.
///
/// Nothing here knows about windows, so the command line and the tests use the same code
/// the drag and drop does.
/// </summary>
public sealed class Library
{
    public string Folder { get; }

    /// <param name="folder">Where to keep it. The default is Documents/Tilt-a-tron/Games,
    /// and TILTATRON_LIBRARY overrides that for anyone who keeps their files elsewhere.</param>
    public Library(string? folder = null)
    {
        Folder = System.IO.Path.GetFullPath(
            folder
            ?? Environment.GetEnvironmentVariable("TILTATRON_LIBRARY")
            ?? System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Tilt-a-tron", "Games"));
    }

    /// <summary>Every readable game in the folder. A file that is not a package is skipped,
    /// not reported: it is your folder, and what else you keep in it is your business.</summary>
    public List<LibraryGame> Games()
    {
        var found = new List<LibraryGame>();
        if (!Directory.Exists(Folder)) return found;
        foreach (var path in Directory.EnumerateFiles(Folder, "*.tat"))
        {
            try { found.Add(new LibraryGame(TatPackage.Read(path), path)); }
            catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException) { }
        }
        return found.OrderBy(g => g.Package.Name, StringComparer.OrdinalIgnoreCase)
                    .ThenByDescending(g => g.Package.Version).ToList();
    }

    public LibraryGame AddFile(string path) => Add(File.ReadAllBytes(path));

    /// <summary>
    /// Files a game under its own id. The same bytes again is not an error and not a copy.
    /// Different bytes under the same id - another version, usually - never overwrite what
    /// is there: the one already in the library is set aside under its version number
    /// first, because the whole point of this folder is that things put in it stay put.
    /// </summary>
    public LibraryGame Add(byte[] bytes)
    {
        var pkg = TatPackage.Parse(bytes);   // throws if it is not an intact game
        Directory.CreateDirectory(Folder);
        var path = System.IO.Path.Combine(Folder, $"{pkg.Id}.tat");

        if (File.Exists(path))
        {
            var existing = File.ReadAllBytes(path);
            if (existing.AsSpan().SequenceEqual(bytes)) return new LibraryGame(pkg, path);
            SetAside(path, existing);
        }
        // Written beside the real name and moved into place, so a crash half way through
        // leaves either the old file or the new one and never half of each.
        var temp = path + ".part";
        File.WriteAllBytes(temp, bytes);
        File.Move(temp, path, overwrite: false);
        return new LibraryGame(pkg, path);
    }

    void SetAside(string path, byte[] contents)
    {
        var version = "old";
        try { version = $"v{TatPackage.Parse(contents).Version}"; }
        catch (InvalidDataException) { }

        var stem = System.IO.Path.GetFileNameWithoutExtension(path);
        for (var n = 0; ; n++)
        {
            var aside = System.IO.Path.Combine(Folder, n == 0 ? $"{stem}.{version}.tat" : $"{stem}.{version}.{n}.tat");
            if (!File.Exists(aside))
            {
                File.Move(path, aside);
                return;
            }
            if (File.ReadAllBytes(aside).AsSpan().SequenceEqual(contents))
            {
                File.Delete(path);   // that exact file has been set aside before
                return;
            }
        }
    }

    /// <summary>Takes a game out of the library. This is the one place a game can really be
    /// lost, so nothing calls it without the person having asked twice.</summary>
    public void Remove(LibraryGame game)
    {
        var dir = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(game.Path));
        if (!string.Equals(dir, Folder, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("that file is not in the library");
        File.Delete(game.Path);
    }

    // ------------------------------------------------------------------ to and from a watch

    /// <summary>
    /// Finds and reads an installed game. This app always installs to Games/&lt;id&gt;.tat,
    /// but a file dropped onto the watch's USB drive keeps whatever name it had, and the
    /// watch goes by the id in the header rather than the name - so when the obvious name
    /// is not the one, look inside the others.
    /// </summary>
    public static (string Path, byte[] Bytes) ReadFromWatch(Watch w, string id)
    {
        var obvious = $"{id}.tat";
        var files = w.ListFiles("Games")
            .Where(f => !f.IsDirectory && f.Name.EndsWith(".tat", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => string.Equals(f.Name, obvious, StringComparison.OrdinalIgnoreCase) ? 0 : 1);
        foreach (var f in files)
        {
            var path = $"Games/{f.Name}";
            var bytes = w.ReadFile(path);
            if (HeaderId(bytes) == id) return (path, bytes);
        }
        throw new WatchException($"{id} is not installed on this watch");
    }

    /// <summary>Just the id, from a file that may be too damaged to pass as a package.</summary>
    static string HeaderId(byte[] bytes)
    {
        if (bytes.Length < 40 || bytes[0] != 'T' || bytes[1] != 'A' || bytes[2] != 'T') return "";
        var field = bytes.AsSpan(24, 16);
        var end = field.IndexOf((byte)0);
        return System.Text.Encoding.UTF8.GetString(end < 0 ? field : field[..end]).Trim();
    }

    /// <summary>Copies an installed game into the library. It stays on the watch.</summary>
    public LibraryGame SaveFromWatch(Watch w, string id) => Add(ReadFromWatch(w, id).Bytes);

    /// <summary>
    /// Removes a game from the watch, and keeps it. The copy goes into the library and is
    /// read back from disk before anything is deleted, so there is no order of failures -
    /// cable pulled, disk full, damaged file - that ends with the game in neither place.
    /// A game too damaged to file is left where it is: a broken copy is still the only copy.
    /// </summary>
    public LibraryGame MoveFromWatch(Watch w, string id)
    {
        var (path, bytes) = ReadFromWatch(w, id);
        var saved = Add(bytes);
        if (!File.ReadAllBytes(saved.Path).AsSpan().SequenceEqual(bytes))
            throw new IOException("the library copy did not read back the same, so nothing was removed");
        w.Delete(path);
        return saved;
    }

    /// <summary>Installs a library game on the watch, under its id.</summary>
    public static void Install(Watch w, LibraryGame game, Action<long, long>? progress = null)
    {
        var pkg = game.Package;
        if (!pkg.RunsOn(w.GameApi.Major, w.GameApi.Minor))
            throw new WatchException($"{pkg.Name} needs game API {pkg.ApiMajor}.{pkg.ApiMinor}; "
                                     + $"this watch has {w.GameApi.Major}.{w.GameApi.Minor}");
        w.MakeFolder("Games");
        w.WriteFile($"Games/{pkg.Id}.tat", File.ReadAllBytes(game.Path), progress);
    }
}
