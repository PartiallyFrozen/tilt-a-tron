using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Text;

namespace Tiltatron.Manager.Link;

/// <summary>
/// Talks to a Tilt-a-tron over its USB serial port. This is the same protocol as
/// components/link and tools/tatlink.py - see docs/GAME_API.md section 7.
/// </summary>
public sealed class Watch : IDisposable
{
    const byte HELLO = 0x01, INFO = 0x02, LIST = 0x03, ICON = 0x04;
    const byte FS_FREE = 0x10, FS_LIST = 0x11, FS_PUT = 0x12, FS_DATA = 0x13,
               FS_END = 0x14, FS_GET = 0x15, FS_DELETE = 0x16, FS_MKDIR = 0x17;
    const byte ERR = 0xFF;
    const int MaxPayload = 4096;

    readonly SerialPort _port;
    byte _seq;

    public string PortName { get; }
    public string Firmware { get; private set; } = "";
    public string Board { get; private set; } = "";
    public (int Major, int Minor) GameApi { get; private set; }

    Watch(SerialPort port, string name)
    {
        _port = port;
        PortName = name;
    }

    /// <summary>Opens a port without touching DTR or RTS: esptool uses those lines to reset
    /// the chip, and asserting them would reboot the watch the moment we connect.</summary>
    static SerialPort OpenPort(string name) =>
        new(name, 115200) { ReadTimeout = 250, WriteTimeout = 2000, DtrEnable = false, RtsEnable = false };

    /// <summary>Tries every serial port and returns the first that answers. Null if none do.</summary>
    public static Watch? Find(Action<string>? note = null)
    {
        foreach (var name in SerialPort.GetPortNames())
        {
            note?.Invoke($"looking on {name}");
            SerialPort? port = null;
            try
            {
                port = OpenPort(name);
                port.Open();
                var watch = new Watch(port, name);
                watch.SayHello();
                return watch;
            }
            catch
            {
                port?.Dispose();
            }
        }
        return null;
    }

    public static Watch Open(string portName)
    {
        var port = OpenPort(portName);
        port.Open();
        var watch = new Watch(port, portName);
        watch.SayHello();
        return watch;
    }

    public void Dispose() => _port.Dispose();

    // ------------------------------------------------------------------ framing

    static ushort Crc16(ReadOnlySpan<byte> data, ushort crc = 0xFFFF)
    {
        foreach (var b in data)
        {
            crc ^= (ushort)(b << 8);
            for (var i = 0; i < 8; i++)
                crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
        }
        return crc;
    }

    byte[] Call(byte cmd, ReadOnlySpan<byte> payload = default, int tries = 3)
    {
        Exception? last = null;
        for (var attempt = 0; attempt < tries; attempt++)
        {
            if (attempt > 0 || cmd == HELLO) _port.DiscardInBuffer();
            _seq++;
            var frame = new byte[8 + payload.Length];
            frame[0] = 0xA5;
            frame[1] = 0x5A;
            frame[2] = (byte)payload.Length;
            frame[3] = (byte)(payload.Length >> 8);
            frame[4] = _seq;
            frame[5] = cmd;
            payload.CopyTo(frame.AsSpan(6));
            var crc = Crc16(payload, Crc16(frame.AsSpan(4, 2)));
            frame[^2] = (byte)(crc >> 8);
            frame[^1] = (byte)crc;
            _port.Write(frame, 0, frame.Length);
            try
            {
                return ReadReply(cmd);
            }
            catch (TimeoutException e)
            {
                last = e;   // a reply can be lost to a burst of log output on the same wire
            }
        }
        throw last ?? new TimeoutException("the watch didn't answer");
    }

    readonly byte[] _rx = new byte[MaxPayload + 64];
    int _rxLen;

    byte[] ReadReply(byte cmd)
    {
        var deadline = Environment.TickCount64 + 4000;
        while (true)
        {
            // The log shares this wire, so hunt for the sync word and trust the CRC.
            var at = 0;
            while (at + 8 <= _rxLen)
            {
                if (_rx[at] != 0xA5 || _rx[at + 1] != 0x5A)
                {
                    at++;
                    continue;
                }
                int len = _rx[at + 2] | (_rx[at + 3] << 8);
                if (len > MaxPayload)
                {
                    at += 2;   // not one of ours, or a corrupt header
                    continue;
                }
                var end = at + 6 + len + 2;
                if (end > _rxLen) break;   // the rest is still on the wire

                var seq = _rx[at + 4];
                var replyCmd = _rx[at + 5];
                var body = _rx.AsSpan(at + 6, len).ToArray();
                var got = (ushort)((_rx[end - 2] << 8) | _rx[end - 1]);
                Consume(end);
                at = 0;
                if (got != Crc16(body, Crc16(new[] { seq, replyCmd }))) continue;
                if (replyCmd == ERR) throw new WatchException(Encoding.UTF8.GetString(body));
                if (replyCmd == (cmd | 0x80)) return body;
            }
            if (at > 0) Consume(at);   // scanned past, keep only the tail

            if (Environment.TickCount64 > deadline) break;
            if (_rxLen == _rx.Length) _rxLen = 0;   // nothing parseable in a full buffer
            try
            {
                // Blocks until at least one byte arrives, so there is no spinning on
                // timeouts - that alone was costing most of a transfer's time.
                _rxLen += _port.Read(_rx, _rxLen, _rx.Length - _rxLen);
            }
            catch (TimeoutException)
            {
                // nothing arrived in time; the deadline above decides when to give up
            }
        }
        throw new TimeoutException("the watch didn't answer (is it awake?)");
    }

    void Consume(int count)
    {
        Buffer.BlockCopy(_rx, count, _rx, 0, _rxLen - count);
        _rxLen -= count;
    }

    // ------------------------------------------------------------------ commands

    void SayHello()
    {
        var b = Call(HELLO);
        GameApi = (BitConverter.ToUInt16(b, 2), BitConverter.ToUInt16(b, 4));
        var rest = Encoding.UTF8.GetString(b, 6, b.Length - 6).Split('\0');
        Firmware = rest.Length > 0 ? rest[0] : "";
        Board = rest.Length > 1 ? rest[1] : "";
    }

    public (uint Total, uint Free, int Count) Info()
    {
        var b = Call(INFO);
        return (BitConverter.ToUInt32(b, 0), BitConverter.ToUInt32(b, 4), b[8]);
    }

    public List<GameEntry> Games()
    {
        var b = Call(LIST);
        var games = new List<GameEntry>();
        for (var i = 0; i < b[0]; i++)
        {
            var at = 1 + i * 48;
            games.Add(new GameEntry(
                Id: Cstr(b, at, 16),
                Name: Cstr(b, at + 16, 24),
                Accent: BitConverter.ToUInt16(b, at + 40),
                BuiltIn: (b[at + 42] & 1) != 0,
                Hidden: (b[at + 42] & 2) != 0,
                Bytes: BitConverter.ToUInt32(b, at + 44)));
        }
        return games;
    }

    public byte[] Icon(string id) => Call(ICON, Fixed(id, 16));

    public (uint Total, uint Free) StorageFree()
    {
        var b = Call(FS_FREE);
        return (BitConverter.ToUInt32(b, 0), BitConverter.ToUInt32(b, 4));
    }

    public List<FileEntry> ListFiles(string path = "")
    {
        var b = Call(FS_LIST, Encoding.UTF8.GetBytes(path));
        var files = new List<FileEntry>();
        var i = 0;
        while (i < b.Length)
        {
            var isDir = b[i] != 0;
            var size = BitConverter.ToUInt32(b, i + 1);
            var end = Array.IndexOf(b, (byte)0, i + 5);
            if (end < 0) break;
            files.Add(new FileEntry(Encoding.UTF8.GetString(b, i + 5, end - i - 5), isDir, size));
            i = end + 1;
        }
        return files;
    }

    public void MakeFolder(string path) => Call(FS_MKDIR, Encoding.UTF8.GetBytes(path));

    public void Delete(string path) => Call(FS_DELETE, Encoding.UTF8.GetBytes(path));

    public byte[] ReadFile(string path)
    {
        var all = new List<byte>();
        while (true)
        {
            var head = new byte[4 + Encoding.UTF8.GetByteCount(path)];
            BitConverter.GetBytes((uint)all.Count).CopyTo(head, 0);
            Encoding.UTF8.GetBytes(path).CopyTo(head, 4);
            var part = Call(FS_GET, head);
            all.AddRange(part);
            if (part.Length < MaxPayload) return all.ToArray();
        }
    }

    public void WriteFile(string path, byte[] data, Action<long, long>? progress = null) =>
        WriteFileChunked(path, data, MaxPayload, progress);

    public void WriteFileChunked(string path, byte[] data, int chunk, Action<long, long>? progress = null)
    {
        var head = new byte[8 + Encoding.UTF8.GetByteCount(path)];
        BitConverter.GetBytes((uint)data.Length).CopyTo(head, 0);
        BitConverter.GetBytes(Crc32(data)).CopyTo(head, 4);
        Encoding.UTF8.GetBytes(path).CopyTo(head, 8);
        Call(FS_PUT, head);
        for (var off = 0; off < data.Length; off += chunk)
        {
            var n = Math.Min(chunk, data.Length - off);
            Call(FS_DATA, data.AsSpan(off, n));
            progress?.Invoke(off + n, data.Length);
        }
        Call(FS_END);
    }

    /// <summary>Copies a folder from this computer onto the watch, making folders as it goes.</summary>
    public void SendFolder(string localDir, string remoteDir, Action<string, long, long>? progress = null)
    {
        MakeFolder(remoteDir);
        foreach (var dir in Directory.GetDirectories(localDir, "*", SearchOption.AllDirectories))
            MakeFolder($"{remoteDir}/{Relative(localDir, dir)}");
        foreach (var file in Directory.GetFiles(localDir, "*", SearchOption.AllDirectories))
        {
            var remote = $"{remoteDir}/{Relative(localDir, file)}";
            var data = File.ReadAllBytes(file);
            WriteFile(remote, data, (done, total) => progress?.Invoke(Path.GetFileName(file), done, total));
        }
    }

    static string Relative(string root, string path) =>
        Path.GetRelativePath(root, path).Replace('\\', '/');

    static string Cstr(byte[] b, int at, int max)
    {
        var end = Array.IndexOf(b, (byte)0, at, max);
        return Encoding.UTF8.GetString(b, at, (end < 0 ? at + max : end) - at);
    }

    static byte[] Fixed(string s, int len)
    {
        var buf = new byte[len];
        Encoding.UTF8.GetBytes(s).AsSpan(0, Math.Min(len, s.Length)).CopyTo(buf);
        return buf;
    }

    static uint Crc32(byte[] data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (var b in data)
        {
            crc ^= b;
            for (var i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320 & (uint)-(crc & 1));
        }
        return ~crc;
    }
}

public record GameEntry(string Id, string Name, ushort Accent, bool BuiltIn, bool Hidden, uint Bytes);

public record FileEntry(string Name, bool IsDirectory, uint Size);

/// <summary>The watch refused, and said why in plain words.</summary>
public sealed class WatchException(string message) : Exception(message);
