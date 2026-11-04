using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace Contour.Deployment {
    public sealed class RawValue {
        public uint Type;
        public string Data;
        public RawValue(uint type, string data) { Type = type; Data = data; }
    }
    public sealed class Entry {
        public string Path;
        public string Name;
        public RawValue Value;
    }

    // All paths are relative to the supplied 64-bit registry root. Tests use
    // an isolated HKCU subtree; the lab entry point supplies HKLM explicitly.
    public sealed class RegistryStore : IDisposable {
        private readonly RegistryKey root;
        private readonly bool writable;
        public RegistryStore(RegistryKey root, bool writable) {
            this.root = root; this.writable = writable;
        }
        public void Dispose() { root.Dispose(); }
        private void RequireWrite() {
            if (!writable) throw new InvalidOperationException("This registry store is read-only.");
        }
        private static void Check(int error) {
            if (error != 0) throw new Win32Exception(error);
        }
        public bool Exists(string path) {
            using (var key = root.OpenSubKey(path, false)) { return key != null; }
        }
        public bool CanWrite(string path) {
            try { using (var key = root.OpenSubKey(path, RegistryKeyPermissionCheck.ReadWriteSubTree,
                System.Security.AccessControl.RegistryRights.SetValue |
                System.Security.AccessControl.RegistryRights.QueryValues)) { return key != null; } }
            catch (UnauthorizedAccessException) { return false; }
            catch (System.Security.SecurityException) { return false; }
        }
        public RawValue Read(string path, string name) {
            using (var key = root.OpenSubKey(path, false)) {
                if (key == null) return null;
                uint type, size = 0;
                int result = RegQueryValueEx(key.Handle, name, IntPtr.Zero, out type, null, ref size);
                if (result == 2) return null;
                Check(result);
                if (size > 1024 * 1024) throw new InvalidDataException("Registry value exceeds snapshot limit.");
                var bytes = new byte[size];
                Check(RegQueryValueEx(key.Handle, name, IntPtr.Zero, out type, bytes, ref size));
                Array.Resize(ref bytes, checked((int)size));
                return new RawValue(type, BitConverter.ToString(bytes).Replace("-", ""));
            }
        }
        public Entry[] Entries(string path) {
            var entries = new List<Entry>();
            Collect(path, entries, null, 0);
            return entries.ToArray();
        }
        public string Snapshot(string path) {
            if (!Exists(path)) return "ABSENT";
            var result = new StringBuilder();
            Collect(path, null, result, 0);
            return result.ToString();
        }
        private void Collect(string path, List<Entry> entries, StringBuilder snapshot, int depth) {
            if (depth > 32) throw new InvalidDataException("Registry snapshot depth exceeded.");
            using (var key = root.OpenSubKey(path, false)) {
                if (key == null) throw new IOException("Registry changed while being inspected: " + path);
                if (snapshot != null) snapshot.Append("K:").Append(Encode(path)).Append('\n');
                var names = key.GetValueNames();
                Array.Sort(names, StringComparer.OrdinalIgnoreCase);
                foreach (var name in names) {
                    var value = Read(path, name);
                    if (value == null) throw new IOException("Registry changed during snapshot.");
                    if (entries != null) entries.Add(new Entry { Path = path, Name = name, Value = value });
                    if (snapshot != null) snapshot.Append("V:").Append(Encode(name)).Append(':')
                        .Append(value.Type).Append(':').Append(value.Data).Append('\n');
                }
                var children = key.GetSubKeyNames();
                Array.Sort(children, StringComparer.OrdinalIgnoreCase);
                foreach (var child in children) Collect(path + "\\" + child, entries, snapshot, depth + 1);
            }
        }
        private static string Encode(string text) {
            return Convert.ToBase64String(Encoding.Unicode.GetBytes(text));
        }
        public void Create(string path) {
            RequireWrite();
            IntPtr key;
            uint disposition;
            Check(RegCreateKeyEx(root.Handle, path, 0, null, 0, 0xF003F | 0x100,
                IntPtr.Zero, out key, out disposition));
            try {
                if (disposition != 1) throw new IOException("Key appeared after planning: " + path);
                Check(RegFlushKey(key));
            } finally { RegCloseKey(key); }
        }
        public void Write(string path, string name, uint type, string hex) {
            RequireWrite();
            using (var key = root.OpenSubKey(path, RegistryKeyPermissionCheck.ReadWriteSubTree,
                System.Security.AccessControl.RegistryRights.SetValue |
                System.Security.AccessControl.RegistryRights.QueryValues)) {
                if (key == null) throw new IOException("Missing write target: " + path);
                var data = Decode(hex);
                Check(RegSetValueEx(key.Handle, name, 0, type, data, data.Length));
                key.Flush();
            }
        }
        public void DeleteValue(string path, string name) {
            RequireWrite();
            using (var key = root.OpenSubKey(path, RegistryKeyPermissionCheck.ReadWriteSubTree,
                System.Security.AccessControl.RegistryRights.SetValue |
                System.Security.AccessControl.RegistryRights.QueryValues)) {
                if (key == null) return;
                key.DeleteValue(name, false);
                key.Flush();
            }
        }
        public bool DeleteEmpty(string path) {
            RequireWrite();
            using (var key = root.OpenSubKey(path, false)) {
                if (key == null) return true;
                if (key.ValueCount != 0 || key.SubKeyCount != 0) return false;
            }
            root.DeleteSubKey(path, false);
            root.Flush();
            return true;
        }
        public static byte[] Decode(string hex) {
            if (hex == null || hex.Length % 2 != 0 || hex.Length > 2 * 1024 * 1024)
                throw new InvalidDataException("Invalid registry byte string.");
            var data = new byte[hex.Length / 2];
            for (int i = 0; i < data.Length; ++i) data[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            return data;
        }
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        private static extern int RegQueryValueEx(SafeRegistryHandle key, string name, IntPtr reserved,
            out uint type, byte[] data, ref uint size);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        private static extern int RegSetValueEx(SafeRegistryHandle key, string name, uint reserved,
            uint type, byte[] data, int size);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
        private static extern int RegCreateKeyEx(SafeRegistryHandle root, string path, int reserved, string cls,
            int options, int access, IntPtr security, out IntPtr key, out uint disposition);
        [DllImport("advapi32.dll")] private static extern int RegFlushKey(IntPtr key);
        [DllImport("advapi32.dll")] private static extern int RegCloseKey(IntPtr key);
    }

    public static class DurableFile {
        // Flush new content before an atomic same-directory rename. A crash can
        // leave an unused .next file; the named journal is always a full document.
        public static void Save(string path, string text, bool createNew) {
            path = Path.GetFullPath(path);
            string next = path + ".next-" + Guid.NewGuid().ToString("N");
            try {
                using (var stream = new FileStream(next, FileMode.CreateNew, FileAccess.Write,
                    FileShare.None, 4096, FileOptions.WriteThrough)) {
                    byte[] bytes = new UTF8Encoding(false).GetBytes(text);
                    stream.Write(bytes, 0, bytes.Length);
                    stream.Flush(true);
                }
                if (!MoveFileEx(next, path, createNew ? 8 : 9))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
            } finally { if (File.Exists(next)) File.Delete(next); }
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool MoveFileEx(string source, string destination, int flags);
    }
}
