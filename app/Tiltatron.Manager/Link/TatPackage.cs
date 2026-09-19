using System;
using System.Buffers.Binary;
using System.IO;
using System.Text;

namespace Tiltatron.Manager.Link;

/// <summary>
/// Just enough of a .tat to decide whether it is worth sending: the header, and the checks
/// the watch would do anyway. Reading it here means a file that is not a package, or was
/// truncated on its way to you, is refused in a moment rather than after a transfer that
/// takes half a minute - and the reason can name the game.
///
/// The layout is docs/GAME_API.md section 2, written by tools/mktat.py.
/// </summary>
public sealed record TatPackage(
    string Id, string Name, string Author,
    int Version, int ApiMajor, int ApiMinor,
    byte AccentR, byte AccentG, byte AccentB,
    int Kind, long Bytes)
{
    const int HeaderBytes = 96;
    public const int KindGame = 1;

    public bool IsGame => Kind == KindGame;

    /// <summary>The same rule the console applies: same major, and a minor it can satisfy.</summary>
    public bool RunsOn(int major, int minor) => ApiMajor == major && ApiMinor <= minor;

    public static TatPackage Read(string path)
    {
        var bytes = File.ReadAllBytes(path);
        if (bytes.Length < HeaderBytes) throw new InvalidDataException("too short to be a package");

        var magic = Encoding.ASCII.GetString(bytes, 0, 6);
        if (magic != "TATPKG") throw new InvalidDataException("no TATPKG marker");
        if (bytes[7] != 1) throw new InvalidDataException($"format version {bytes[7]} is not one this app knows");

        var span = bytes.AsSpan();
        var headerCrc = BinaryPrimitives.ReadUInt32LittleEndian(span[8..]);
        var totalSize = BinaryPrimitives.ReadUInt32LittleEndian(span[12..]);
        if (totalSize != bytes.Length)
            throw new InvalidDataException($"says it is {totalSize} bytes but is {bytes.Length}");

        var kind = bytes[16];
        var apiMajor = BinaryPrimitives.ReadUInt16LittleEndian(span[18..]);
        var apiMinor = BinaryPrimitives.ReadUInt16LittleEndian(span[20..]);
        var version = BinaryPrimitives.ReadUInt16LittleEndian(span[22..]);
        var id = Str(span.Slice(24, 16));
        var name = Str(span.Slice(40, 24));
        var author = Str(span.Slice(64, 24));
        var sections = BinaryPrimitives.ReadUInt16LittleEndian(span[92..]);
        if (sections == 0 || sections > 64) throw new InvalidDataException("the section table makes no sense");

        // The header's CRC covers everything after the field itself, the section table
        // included, so a file that was cut short or meddled with is caught here.
        var covered = HeaderBytes + sections * 16 - 12;
        if (bytes.Length < 12 + covered) throw new InvalidDataException("the section table runs past the end");
        if (Crc32(span.Slice(12, covered)) != headerCrc) throw new InvalidDataException("it is damaged");

        if (id.Length == 0 || name.Length == 0) throw new InvalidDataException("it has no name");
        if (kind != KindGame) throw new InvalidDataException("it is not a game");

        return new TatPackage(id, name, author, version, apiMajor, apiMinor,
                              bytes[88], bytes[89], bytes[90], kind, bytes.Length);
    }

    static string Str(ReadOnlySpan<byte> fixedField)
    {
        var end = fixedField.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? fixedField : fixedField[..end]).Trim();
    }

    /// <summary>The same CRC-32 the watch and the packer use.</summary>
    static uint Crc32(ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            crc ^= b;
            for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (uint)(-(int)(crc & 1)));
        }
        return ~crc;
    }
}
