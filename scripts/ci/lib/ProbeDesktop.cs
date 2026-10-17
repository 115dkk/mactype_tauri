using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace MacType.ControlCenter.Ci
{
    public sealed class ProbeDesktopResult
    {
        internal ProbeDesktopResult(int exitCode, byte[] output, byte[] error, string desktop)
        {
            ExitCode = exitCode;
            StandardOutputBytes = output;
            StandardErrorBytes = error;
            DesktopName = desktop;
        }

        public int ExitCode { get; }
        public byte[] StandardOutputBytes { get; }
        public byte[] StandardErrorBytes { get; }
        public string StandardOutput => Encoding.UTF8.GetString(StandardOutputBytes);
        public string StandardError => Encoding.UTF8.GetString(StandardErrorBytes);
        public string DesktopName { get; }
    }

    public static class ProbeDesktop
    {
        private const string DesktopPrefix = "MacTypeProbe-";
        private const int CleanupMilliseconds = 10000;
        private const uint KillOnJobClose = 0x2000;

        public static string CurrentDesktopName => ObjectName(GetThreadDesktop(GetCurrentThreadId()));

        public static string InputDesktopName
        {
            get
            {
                IntPtr desktop = OpenInputDesktop(0, false, 1);
                if (desktop == IntPtr.Zero) throw LastError("OpenInputDesktop");
                try { return ObjectName(desktop); }
                finally { CloseDesktop(desktop); }
            }
        }

        public static bool IsIsolatedContext()
        {
            string name = CurrentDesktopName;
            Guid identity;
            if (!name.StartsWith(DesktopPrefix, StringComparison.Ordinal) ||
                !Guid.TryParseExact(name.Substring(DesktopPrefix.Length), "N", out identity)) return false;
            // A marker alone cannot authorize windows on the user's input desktop.
            return !String.Equals(name, InputDesktopName, StringComparison.OrdinalIgnoreCase);
        }

        public static ProbeDesktopResult Run(
            string executable, string[] arguments, string workingDirectory,
            int timeoutMilliseconds, int maximumOutputBytes, bool interactive)
        {
            if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Probe desktops require Windows.");
            if (!Path.IsPathFullyQualified(executable) || !File.Exists(executable))
                throw new ArgumentException("The probe executable must be an existing absolute path.", nameof(executable));
            if (!Path.IsPathFullyQualified(workingDirectory) || !Directory.Exists(workingDirectory))
                throw new ArgumentException("The working directory must be an existing absolute path.", nameof(workingDirectory));
            if (timeoutMilliseconds < 1 || timeoutMilliseconds > 1800000)
                throw new ArgumentOutOfRangeException(nameof(timeoutMilliseconds));
            if (maximumOutputBytes < 1 || maximumOutputBytes > 16777216)
                throw new ArgumentOutOfRangeException(nameof(maximumOutputBytes));
            if (arguments == null) throw new ArgumentNullException(nameof(arguments));
            var command = new StringBuilder(QuoteArgument(executable));
            foreach (string argument in arguments) command.Append(' ').Append(QuoteArgument(argument));
            if (command.Length >= 32767) throw new ArgumentException("The probe command line exceeds the Windows limit.");

            string station = ObjectName(GetProcessWindowStation());
            bool useCurrentDesktop = interactive || IsIsolatedContext();
            string desktopName = useCurrentDesktop ? CurrentDesktopName : DesktopPrefix + Guid.NewGuid().ToString("N");
            ProbeDesktopResult result = null;
            Exception failure = null;
            // Desktop creation stays off the caller's thread; no caller desktop switch is needed.
            var worker = new Thread(() =>
            {
                try { result = RunCore(executable, command, workingDirectory, station, desktopName,
                    timeoutMilliseconds, maximumOutputBytes, useCurrentDesktop); }
                catch (Exception error) { failure = error; }
            });
            worker.IsBackground = true;
            worker.Start();
            worker.Join();
            if (failure != null) ExceptionDispatchInfo.Capture(failure).Throw();
            return result;
        }

        private static ProbeDesktopResult RunCore(
            string executable, StringBuilder command, string workingDirectory, string station,
            string desktopName, int timeoutMilliseconds, int maximumOutputBytes, bool useCurrentDesktop)
        {
            IntPtr desktop = IntPtr.Zero, job = IntPtr.Zero, input = IntPtr.Zero;
            IntPtr outputRead = IntPtr.Zero, outputWrite = IntPtr.Zero;
            IntPtr errorRead = IntPtr.Zero, errorWrite = IntPtr.Zero;
            IntPtr attributes = IntPtr.Zero, handleList = IntPtr.Zero;
            bool attributesInitialized = false, assigned = false, treeEmpty = false;
            var process = new ProcessInformation();
            var output = new MemoryStream();
            var error = new MemoryStream();
            var timer = Stopwatch.StartNew();
            try
            {
                if (!useCurrentDesktop)
                {
                    // No SWITCHDESKTOP right or inherited desktop handle is requested.
                    desktop = CreateDesktopW(desktopName, IntPtr.Zero, IntPtr.Zero, 0, 0x83, IntPtr.Zero);
                    if (desktop == IntPtr.Zero) throw LastError("CreateDesktopW");
                    if (String.Equals(desktopName, InputDesktopName, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("The probe desktop must not be the input desktop.");
                }
                job = CreateJobObjectW(IntPtr.Zero, null);
                if (job == IntPtr.Zero) throw LastError("CreateJobObjectW");
                var limits = new JobExtendedLimits();
                limits.Basic.LimitFlags = KillOnJobClose;
                if (!SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf<JobExtendedLimits>()))
                    throw LastError("SetInformationJobObject");

                var security = new SecurityAttributes { Length = Marshal.SizeOf<SecurityAttributes>(), InheritHandle = true };
                if (!CreatePipe(out outputRead, out outputWrite, ref security, 0)) throw LastError("CreatePipe(stdout)");
                if (!SetHandleInformation(outputRead, 1, 0)) throw LastError("SetHandleInformation(stdout)");
                if (!CreatePipe(out errorRead, out errorWrite, ref security, 0)) throw LastError("CreatePipe(stderr)");
                if (!SetHandleInformation(errorRead, 1, 0)) throw LastError("SetHandleInformation(stderr)");
                input = CreateFileW("NUL", 0x80000000, 3, ref security, 3, 0, IntPtr.Zero);
                if (input == new IntPtr(-1)) throw LastError("CreateFileW(NUL)");

                IntPtr size = IntPtr.Zero;
                InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
                if (size == IntPtr.Zero) throw LastError("InitializeProcThreadAttributeList(size)");
                attributes = Marshal.AllocHGlobal(size);
                if (!InitializeProcThreadAttributeList(attributes, 1, 0, ref size))
                    throw LastError("InitializeProcThreadAttributeList");
                attributesInitialized = true;
                handleList = Marshal.AllocHGlobal(3 * IntPtr.Size);
                Marshal.Copy(new[] { input, outputWrite, errorWrite }, 0, handleList, 3);
                if (!UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20002), handleList,
                    new IntPtr(3 * IntPtr.Size), IntPtr.Zero, IntPtr.Zero)) throw LastError("UpdateProcThreadAttribute");
                var startup = new StartupInfoEx();
                startup.Startup.Size = Marshal.SizeOf<StartupInfoEx>();
                startup.Startup.Desktop = station + "\\" + desktopName;
                startup.Startup.Flags = 0x100; // STARTF_USESTDHANDLES, not STARTF_USESHOWWINDOW.
                startup.Startup.Input = input;
                startup.Startup.Output = outputWrite;
                startup.Startup.Error = errorWrite;
                startup.Attributes = attributes;
                // CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT.
                // The handle allowlist excludes all caller desktop and job handles.
                if (!CreateProcessW(executable, command, IntPtr.Zero, IntPtr.Zero, true,
                    0x08080004, IntPtr.Zero, workingDirectory, ref startup, out process))
                    throw LastError("CreateProcessW");
                if (!AssignProcessToJobObject(job, process.Process))
                    throw LastError("AssignProcessToJobObject (probe remains suspended)");
                assigned = true;
                Close(ref outputWrite);
                Close(ref errorWrite);
                Close(ref input);
                if (ResumeThread(process.Thread) != 1) throw LastError("ResumeThread");
                Close(ref process.Thread);

                byte[] buffer = new byte[8192];
                while (true)
                {
                    DrainAvailable(outputRead, output, buffer, maximumOutputBytes, "stdout");
                    DrainAvailable(errorRead, error, buffer, maximumOutputBytes, "stderr");
                    if (ActiveProcesses(job) == 0)
                    {
                        treeEmpty = true;
                        // Only our job inherited the write ends; no blocking EOF wait is needed.
                        while (DrainAvailable(outputRead, output, buffer, maximumOutputBytes, "stdout")) { }
                        while (DrainAvailable(errorRead, error, buffer, maximumOutputBytes, "stderr")) { }
                        uint exitCode;
                        if (!GetExitCodeProcess(process.Process, out exitCode)) throw LastError("GetExitCodeProcess");
                        return new ProbeDesktopResult(unchecked((int)exitCode),
                            output.ToArray(), error.ToArray(), desktopName);
                    }
                    if (timer.ElapsedMilliseconds >= timeoutMilliseconds)
                        throw new TimeoutException("Probe process tree exceeded its deadline.");
                    Thread.Sleep(5);
                }
            }
            catch (Exception failure)
            {
                failure.Data["ProbeStandardOutput"] = output.ToArray();
                failure.Data["ProbeStandardError"] = error.ToArray();
                throw;
            }
            finally
            {
                try
                {
                    if (process.Process != IntPtr.Zero && !treeEmpty)
                    {
                        if (assigned)
                        {
                            try
                            {
                                bool terminated = TerminateJobObject(job, 1);
                                int terminationError = Marshal.GetLastWin32Error();
                                treeEmpty = WaitForEmpty(job, CleanupMilliseconds);
                                if (!treeEmpty) throw new InvalidOperationException("Probe tree did not terminate.",
                                    terminated ? null : new Win32Exception(terminationError));
                            }
                            catch (Exception cleanupFailure)
                            {
                                // A failed liveness query is not proof that the desktop is unused.
                                RetainUntilEmpty(job, desktop);
                                job = desktop = IntPtr.Zero;
                                throw new InvalidOperationException("Probe tree cleanup did not finish; desktop retained.", cleanupFailure);
                            }
                        }
                        else
                        {
                            // The primary thread has never run, so no descendants can exist here.
                            TerminateProcess(process.Process, 1);
                            if (WaitForSingleObject(process.Process, CleanupMilliseconds) != 0)
                            {
                                RetainUntilExit(process.Process, desktop);
                                process.Process = desktop = IntPtr.Zero;
                                throw new InvalidOperationException("Suspended probe cleanup did not finish; desktop retained.");
                            }
                        }
                    }
                }
                finally
                {
                    Close(ref process.Thread);
                    Close(ref process.Process);
                    Close(ref job);
                    Close(ref input);
                    Close(ref outputRead);
                    Close(ref outputWrite);
                    Close(ref errorRead);
                    Close(ref errorWrite);
                    if (attributesInitialized) DeleteProcThreadAttributeList(attributes);
                    if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
                    if (handleList != IntPtr.Zero) Marshal.FreeHGlobal(handleList);
                    if (desktop != IntPtr.Zero) CloseDesktop(desktop);
                    output.Dispose();
                    error.Dispose();
                }
            }
        }

        private static void RetainUntilEmpty(IntPtr job, IntPtr desktop)
        {
            var reaper = new Thread(() =>
            {
                try
                {
                    while (!WaitForEmpty(job, 1000)) TerminateJobObject(job, 1);
                    CloseHandle(job);
                    if (desktop != IntPtr.Zero) CloseDesktop(desktop);
                }
                catch { /* Unknown liveness keeps the desktop handle until process exit. */ }
            });
            reaper.IsBackground = true;
            reaper.Start();
        }

        private static void RetainUntilExit(IntPtr process, IntPtr desktop)
        {
            var reaper = new Thread(() =>
            {
                while (true)
                {
                    uint wait = WaitForSingleObject(process, 1000);
                    if (wait == 0) break;
                    if (wait != 0x102) return; // Unknown liveness retains both handles until process exit.
                    TerminateProcess(process, 1);
                }
                CloseHandle(process);
                if (desktop != IntPtr.Zero) CloseDesktop(desktop);
            });
            reaper.IsBackground = true;
            reaper.Start();
        }

        private static bool WaitForEmpty(IntPtr job, int timeout)
        {
            var elapsed = Stopwatch.StartNew();
            do
            {
                if (ActiveProcesses(job) == 0) return true;
                Thread.Sleep(10);
            } while (elapsed.ElapsedMilliseconds < timeout);
            return ActiveProcesses(job) == 0;
        }

        private static uint ActiveProcesses(IntPtr job)
        {
            JobAccounting accounting;
            if (!QueryInformationJobObject(job, 1, out accounting, (uint)Marshal.SizeOf<JobAccounting>(), IntPtr.Zero))
                throw LastError("QueryInformationJobObject");
            return accounting.ActiveProcesses;
        }

        private static bool DrainAvailable(IntPtr pipe, MemoryStream target, byte[] buffer, int limit, string name)
        {
            uint available, read;
            if (!PeekNamedPipe(pipe, IntPtr.Zero, 0, IntPtr.Zero, out available, IntPtr.Zero))
            {
                if (Marshal.GetLastWin32Error() == 109) return false; // ERROR_BROKEN_PIPE
                throw LastError("PeekNamedPipe");
            }
            if (available == 0) return false;
            // The sole reader consumes only bytes already buffered, never waiting on descendants.
            if (!ReadFile(pipe, buffer, Math.Min((uint)buffer.Length, available), out read, IntPtr.Zero))
                throw LastError("ReadFile");
            if (target.Length + read > limit) throw new IOException("Probe " + name + " exceeded its output limit.");
            target.Write(buffer, 0, (int)read);
            return read != 0;
        }

        private static string QuoteArgument(string value)
        {
            if (value == null || value.IndexOf('\0') >= 0) throw new ArgumentException("Probe arguments cannot be null or contain NUL.");
            var quoted = new StringBuilder("\"");
            int slashes = 0;
            foreach (char character in value)
            {
                if (character == '\\') { slashes++; continue; }
                quoted.Append('\\', character == '"' ? slashes * 2 + 1 : slashes);
                quoted.Append(character);
                slashes = 0;
            }
            return quoted.Append('\\', slashes * 2).Append('"').ToString();
        }

        private static string ObjectName(IntPtr handle)
        {
            if (handle == IntPtr.Zero) throw LastError("Get desktop/window station handle");
            var name = new StringBuilder(512);
            uint needed;
            if (!GetUserObjectInformationW(handle, 2, name, (uint)(name.Capacity * 2), out needed))
                throw LastError("GetUserObjectInformationW");
            return name.ToString();
        }

        private static Win32Exception LastError(string operation) => new Win32Exception(Marshal.GetLastWin32Error(), operation);

        private static void Close(ref IntPtr handle)
        {
            if (handle != IntPtr.Zero && handle != new IntPtr(-1)) CloseHandle(handle);
            handle = IntPtr.Zero;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes
        {
            public int Length;
            public IntPtr Descriptor;
            [MarshalAs(UnmanagedType.Bool)] public bool InheritHandle;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public int Size;
            public string Reserved, Desktop, Title;
            public uint X, Y, XSize, YSize, XChars, YChars, Fill, Flags;
            public ushort ShowWindow, ReservedSize;
            public IntPtr ReservedBytes, Input, Output, Error;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct StartupInfoEx { public StartupInfo Startup; public IntPtr Attributes; }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }

        [StructLayout(LayoutKind.Sequential)]
        private struct JobBasicLimits
        {
            public long ProcessTime, JobTime;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JobExtendedLimits
        {
            public JobBasicLimits Basic;
            public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
            public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JobAccounting
        {
            public long UserTime, KernelTime, PeriodUserTime, PeriodKernelTime;
            public uint PageFaults, TotalProcesses, ActiveProcesses, TerminatedProcesses;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, uint flags, uint access, IntPtr security);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool CloseDesktop(IntPtr desktop);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr GetThreadDesktop(uint thread);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr GetProcessWindowStation();
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool GetUserObjectInformationW(IntPtr handle, int index, StringBuilder value, uint length, out uint needed);
        [DllImport("kernel32.dll")]
        private static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcessW(string executable, StringBuilder command, IntPtr processSecurity,
            IntPtr threadSecurity, bool inherit, uint flags, IntPtr environment, string directory,
            ref StartupInfoEx startup, out ProcessInformation process);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(IntPtr thread);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(IntPtr process, uint exitCode);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, int milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObjectW(IntPtr security, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(IntPtr job, int kind, ref JobExtendedLimits limits, uint length);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool QueryInformationJobObject(IntPtr job, int kind, out JobAccounting accounting, uint length, IntPtr returned);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateJobObject(IntPtr job, uint exitCode);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CreatePipe(out IntPtr read, out IntPtr write, ref SecurityAttributes security, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFileW(string file, uint access, uint sharing, ref SecurityAttributes security,
            uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool PeekNamedPipe(IntPtr pipe, IntPtr buffer, uint size, IntPtr read, out uint available, IntPtr remaining);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadFile(IntPtr file, byte[] buffer, uint size, out uint read, IntPtr overlapped);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref IntPtr size);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute,
            IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
        [DllImport("kernel32.dll")]
        private static extern void DeleteProcThreadAttributeList(IntPtr list);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
