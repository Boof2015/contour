using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace Contour.Deployment {
    public static class UserDataFile {
        [StructLayout(LayoutKind.Sequential)]
        struct Attributes { public uint Value, Tag; }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern SafeFileHandle CreateFile(string path, uint access, uint sharing,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern uint GetFinalPathNameByHandle(SafeFileHandle file, StringBuilder path, uint capacity, uint flags);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetFileInformationByHandleEx(SafeFileHandle file, int kind, out Attributes value, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetFileInformationByHandle(SafeFileHandle file, int kind, ref byte delete, uint size);

        public static void Delete(string path) {
            var expected = Path.GetFullPath(path);
            // Open the exact file for deletion, without following a leaf link.
            // Keep the handle through validation and deletion, avoiding a second
            // path lookup if a user changes a directory during elevated cleanup.
            using (var file = CreateFile(expected, 0x10080, 3, IntPtr.Zero, 3, 0x00200000, IntPtr.Zero)) {
                if (file.IsInvalid) {
                    var error = Marshal.GetLastWin32Error();
                    if (error == 2 || error == 3) return;
                    throw new Win32Exception(error, "Cannot remove saved Contour data.");
                }
                Attributes attributes;
                if (!GetFileInformationByHandleEx(file, 9, out attributes, 8)) throw new Win32Exception();
                if ((attributes.Value & 0x410) != 0) throw new IOException("Saved data is redirected or is not a file.");
                var name = new StringBuilder(32768);
                var length = GetFinalPathNameByHandle(file, name, (uint)name.Capacity, 0);
                if (length == 0 || length >= name.Capacity) throw new IOException("Cannot verify saved data location.");
                var final = name.ToString();
                if (final.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) final = @"\\" + final.Substring(8);
                else if (final.StartsWith(@"\\?\", StringComparison.Ordinal)) final = final.Substring(4);
                if (!String.Equals(final, expected, StringComparison.OrdinalIgnoreCase)) throw new IOException("Saved data location changed. The file was kept.");
                byte delete = 1;
                if (!SetFileInformationByHandle(file, 4, ref delete, 1)) throw new Win32Exception();
            }
        }
    }

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
        public string Security(string path) {
            using (var key = root.OpenSubKey(path, RegistryKeyPermissionCheck.ReadSubTree,
                System.Security.AccessControl.RegistryRights.ReadPermissions)) {
                if (key == null) throw new IOException("Missing security target: " + path);
                return key.GetAccessControl(System.Security.AccessControl.AccessControlSections.Access |
                    System.Security.AccessControl.AccessControlSections.Owner |
                    System.Security.AccessControl.AccessControlSections.Group).GetSecurityDescriptorSddlForm(
                        System.Security.AccessControl.AccessControlSections.Access |
                        System.Security.AccessControl.AccessControlSections.Owner |
                        System.Security.AccessControl.AccessControlSections.Group);
            }
        }
        // This opt-in installer operation can create only Contour's four context
        // keys. It never changes an existing key's owner or DACL. Some endpoint
        // stores deny CreateSubKey even to elevated administrators; Windows'
        // documented restore privilege permits this narrowly scoped creation.
        public void CreateControlContextKey(string path, bool allowRestorePrivilege) {
            RequireWrite();
            const string pattern = @"\ASOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}\\FxProperties\\\{8b5699ab-dbc3-4ff7-82ed-8fbcbf7c8cf4\}(\\(Default|User|Volatile))?\z";
            if (!System.Text.RegularExpressions.Regex.IsMatch(path, pattern, System.Text.RegularExpressions.RegexOptions.IgnoreCase))
                throw new InvalidOperationException("Context creation path is outside the allowlist.");
            if (Exists(path) || !Exists(path.Substring(0, path.LastIndexOf('\\'))))
                throw new IOException("Context target exists or its immediate parent is absent.");
            IntPtr descriptor = IntPtr.Zero, inherited = IntPtr.Zero, created = IntPtr.Zero, token = IntPtr.Zero;
            uint descriptorSize, disposition;
            // Administrators can remove the newly created keys during recovery.
            // Parent inheritance remains enabled; no existing ACL is rewritten.
            if (!ConvertStringSecurityDescriptorToSecurityDescriptor("D:(A;CI;KA;;;BA)", 1,
                out descriptor, out descriptorSize)) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                byte[] parent;
                using (var key = root.OpenSubKey(path.Substring(0, path.LastIndexOf('\\')), RegistryKeyPermissionCheck.ReadSubTree,
                    System.Security.AccessControl.RegistryRights.ReadPermissions)) {
                    parent = key.GetAccessControl(System.Security.AccessControl.AccessControlSections.Access |
                        System.Security.AccessControl.AccessControlSections.Owner |
                        System.Security.AccessControl.AccessControlSections.Group).GetSecurityDescriptorBinaryForm();
                }
                if (!OpenProcessToken(GetCurrentProcess(), 0x8, out token)) throw new Win32Exception(Marshal.GetLastWin32Error());
                var mapping = new GenericMapping { Read = 0x20019, Write = 0x20006, Execute = 0x20019, All = 0xF003F };
                if (!CreatePrivateObjectSecurityEx(parent, descriptor, out inherited, IntPtr.Zero, true, 1, token, ref mapping))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                var attributes = new SecurityAttributes { Length = Marshal.SizeOf(typeof(SecurityAttributes)), Descriptor = inherited };
                int result = RegCreateContextKey(root.Handle, path, 0, null, 0, 0xF003F | 0x100,
                    ref attributes, out created, out disposition);
                if (result == 5 && allowRestorePrivilege) {
                    using (var privilege = new RestorePrivilege()) {
                        result = RegCreateContextKey(root.Handle, path, 0, null, 4, 0,
                            ref attributes, out created, out disposition);
                    }
                }
                Check(result);
                if (disposition != 1) throw new IOException("Context key appeared after planning.");
            } finally {
                if (created != IntPtr.Zero) RegCloseKey(created);
                if (inherited != IntPtr.Zero) DestroyPrivateObjectSecurity(ref inherited);
                if (token != IntPtr.Zero) CloseHandle(token);
                LocalFree(descriptor);
            }
            using (var key = root.OpenSubKey(path, false)) {
                if (key == null) throw new IOException("Created context key is missing.");
                key.Flush();
            }
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
        [StructLayout(LayoutKind.Sequential)] private struct SecurityAttributes {
            public int Length;
            public IntPtr Descriptor;
            public int Inherit;
        }
        [DllImport("advapi32.dll", EntryPoint = "RegCreateKeyExW", CharSet = CharSet.Unicode)]
        private static extern int RegCreateContextKey(SafeRegistryHandle root, string path, int reserved, string cls,
            int options, int access, ref SecurityAttributes security, out IntPtr key, out uint disposition);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string text, uint revision,
            out IntPtr descriptor, out uint size);
        [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr memory);
        [StructLayout(LayoutKind.Sequential)] private struct GenericMapping { public uint Read, Write, Execute, All; }
        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool CreatePrivateObjectSecurityEx(byte[] parent, IntPtr creator, out IntPtr descriptor,
            IntPtr objectType, bool container, uint flags, IntPtr token, ref GenericMapping mapping);
        [DllImport("advapi32.dll", SetLastError = true)] private static extern bool DestroyPrivateObjectSecurity(ref IntPtr descriptor);
        [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    }

    internal sealed class RestorePrivilege : IDisposable {
        [StructLayout(LayoutKind.Sequential, Pack = 4)] private struct Privileges {
            public uint Count;
            public long Luid;
            public uint Attributes;
        }
        private IntPtr token;
        private Privileges previous;
        public RestorePrivilege() {
            if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                long luid;
                if (!LookupPrivilegeValue(null, "SeRestorePrivilege", out luid)) throw new Win32Exception(Marshal.GetLastWin32Error());
                var requested = new Privileges { Count = 1, Luid = luid, Attributes = 2 };
                uint length;
                if (!AdjustTokenPrivileges(token, false, ref requested, (uint)Marshal.SizeOf(typeof(Privileges)), out previous, out length))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                int error = Marshal.GetLastWin32Error();
                if (error != 0) throw new Win32Exception(error);
            } catch { CloseHandle(token); token = IntPtr.Zero; throw; }
        }
        public void Dispose() {
            if (token == IntPtr.Zero) return;
            Privileges ignored; uint length;
            bool restored = AdjustTokenPrivileges(token, false, ref previous, (uint)Marshal.SizeOf(typeof(Privileges)), out ignored, out length);
            int error = Marshal.GetLastWin32Error();
            CloseHandle(token); token = IntPtr.Zero;
            if (!restored || error != 0) throw new Win32Exception(error);
        }
        [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool LookupPrivilegeValue(string system, string name, out long luid);
        [DllImport("advapi32.dll", SetLastError = true)] private static extern bool AdjustTokenPrivileges(IntPtr token, bool disable,
            ref Privileges state, uint size, out Privileges previous, out uint length);
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
                for (int attempt = 0; ; ++attempt) {
                    if (MoveFileEx(next, path, createNew ? 8 : 9)) break;
                    int error = Marshal.GetLastWin32Error();
                    // Readers and filesystem filters can briefly deny the
                    // rename. Retry only lock/access failures, using the same
                    // flushed file; never delete the destination to get past it.
                    if (attempt < 5 && (error == 5 || error == 32 || error == 33)) {
                        System.Threading.Thread.Sleep(20 << attempt);
                        continue;
                    }
                    throw new Win32Exception(error, "Cannot save journal '" + path + "': " + new Win32Exception(error).Message);
                }
            } finally { if (File.Exists(next)) File.Delete(next); }
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool MoveFileEx(string source, string destination, int flags);
    }
}
