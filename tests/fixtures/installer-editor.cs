// Runs only from the isolated installer test directory. Exercises the real
// process/window closing path, including close-to-tray-independent shutdown.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

class InstallerEditor {
    static string control;
    delegate IntPtr WindowProc(IntPtr window, uint message, IntPtr w, IntPtr l);
    static readonly WindowProc callback = WndProc;
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WindowClass {
        public uint style; public WindowProc procedure; public int classExtra, windowExtra;
        public IntPtr instance, icon, cursor, background;
        public string menu, name;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern ushort RegisterClassW(ref WindowClass value);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style,
        int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr window, uint message, IntPtr w, IntPtr l);
    static IntPtr WndProc(IntPtr window, uint message, IntPtr w, IntPtr l) {
        if (message == 0x11) {
            if (File.Exists(Path.Combine(control, "editor-hung"))) System.Threading.Thread.Sleep(30000);
            return File.Exists(Path.Combine(control, "editor-busy")) ? IntPtr.Zero : new IntPtr(1);
        }
        if (message == 0x16 && w != IntPtr.Zero) {
            File.WriteAllText(Path.Combine(control, "editor-closed"), "recovery checkpoint");
            Application.ExitThread();
            return IntPtr.Zero;
        }
        return DefWindowProcW(window, message, w, l);
    }
    [STAThread] static void Main(string[] args) {
        control = args[0];
        var cls = new WindowClass { name = "ContourEditor", procedure = callback };
        if (RegisterClassW(ref cls) == 0) throw new System.ComponentModel.Win32Exception();
        var window = CreateWindowExW(0, cls.name, "Contour installer fixture", 0, 0, 0, 0, 0, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (window == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
        File.WriteAllText(Path.Combine(control, "editor-ready"), "ready");
        Application.Run();
    }
}
