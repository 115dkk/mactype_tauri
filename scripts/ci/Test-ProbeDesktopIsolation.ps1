#requires -Version 7.2
[CmdletBinding()]
param(
    [string] $ScratchDirectory = [IO.Path]::GetTempPath()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/ProbeDesktop.ps1')
if (-not ('ProbeDesktopContractHandles' -as [type])) {
    Add-Type @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ProbeDesktopContractHandles
{
    public static bool Exists(string name)
    {
        IntPtr desktop = OpenDesktop(name, 0, false, 1);
        if (desktop != IntPtr.Zero) { CloseDesktop(desktop); return true; }
        int error = Marshal.GetLastWin32Error();
        if (error != 2) throw new Win32Exception(error);
        return false;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenDesktop(string name, uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
}
'@
}

function Assert-ProbeContract {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}

function Assert-ProbeFailure {
    param([scriptblock] $Action, [string] $Expected)
    try { & $Action }
    catch {
        if ($_.ToString() -notlike "*$Expected*") { throw }
        return
    }
    throw "Expected failure: $Expected"
}

$directory = Join-Path $ScratchDirectory ("p ' " + [Guid]::NewGuid().ToString('N').Substring(0, 8))
[void] [IO.Directory]::CreateDirectory($directory)
$before = [MacType.ControlCenter.Ci.ProbeDesktop]::CurrentDesktopName
$inputBefore = [MacType.ControlCenter.Ci.ProbeDesktop]::InputDesktopName
$isolatedBefore = Test-ProbeDesktopContext
$oldEnvironment = $env:MACTYPE_PROBE_CONTRACT_VALUE
try {
    $env:MACTYPE_PROBE_CONTRACT_VALUE = 'inherited environment; no shell expansion'
    $fixtureSource = Join-Path $directory 'fixture.cs'
    $fixture = Join-Path $directory 'visible probe.exe'
    @'
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class ProbeFixture
{
    private static int paints;
    private static WindowProc callback = OnMessage;

    public static int Main(string[] arguments)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        bool inJob;
        if (GetConsoleWindow() != IntPtr.Zero || !IsProcessInJob(GetCurrentProcess(), IntPtr.Zero, out inJob) || !inJob) return 85;
        string desktop = Name(GetThreadDesktop(GetCurrentThreadId()));
        IntPtr input = OpenInputDesktop(0, false, 1);
        if (input == IntPtr.Zero) return 80;
        string inputName;
        try { inputName = Name(input); }
        finally { CloseDesktop(input); }
        if (!desktop.StartsWith("MacTypeProbe-", StringComparison.Ordinal) || desktop == inputName) return 81;
        string mode = arguments[0];
        if (mode == "args")
        {
            for (int index = 1; index < arguments.Length; index++)
                Console.WriteLine(Convert.ToBase64String(Encoding.UTF8.GetBytes(arguments[index])));
            Console.Error.Write("stderr sentinel");
            return 37;
        }
        if (mode == "bytes")
        {
            byte[] bytes = { 0, 128, 255, 10 };
            Console.OpenStandardOutput().Write(bytes, 0, bytes.Length);
            Console.OpenStandardError().Write(bytes, 0, bytes.Length);
            return 0;
        }
        if (mode == "flood")
        {
            while (true) { Console.Write(new string('o', 1024)); Console.Error.Write(new string('e', 1024)); }
        }
        if (mode == "tree" || mode == "orphan")
        {
            var start = new ProcessStartInfo(Process.GetCurrentProcess().MainModule.FileName);
            start.UseShellExecute = false;
            start.CreateNoWindow = true;
            start.Arguments = (mode == "tree" ? "linger" : "brief") + " \"" + arguments[1] + "\"";
            using (Process child = Process.Start(start))
            {
                if (mode == "orphan") return 23;
                File.WriteAllText(arguments[1] + ".parent", Process.GetCurrentProcess().Id.ToString());
                child.WaitForExit();
            }
            return 0;
        }
        // All window creation and activation is behind the actual non-input desktop check above.
        var windowClass = new WindowClass();
        windowClass.Procedure = Marshal.GetFunctionPointerForDelegate(callback);
        windowClass.Instance = GetModuleHandle(null);
        windowClass.Name = "MacTypeProbeFixture";
        if (RegisterClass(ref windowClass) == 0) return 82;
        IntPtr window = CreateWindowEx(0, windowClass.Name, "Private desktop paint contract",
            0x10CF0000, 10, 10, 240, 100, IntPtr.Zero, IntPtr.Zero, windowClass.Instance, IntPtr.Zero);
        if (window == IntPtr.Zero) return 83;
        try
        {
            ShowWindow(window, 5);
            SetActiveWindow(window);
            InvalidateRect(window, IntPtr.Zero, true);
            UpdateWindow(window);
            if (!IsWindowVisible(window) || GetActiveWindow() != window || paints == 0) return 84;
            if (mode == "linger" || mode == "brief")
            {
                File.WriteAllText(arguments[1], Process.GetCurrentProcess().Id.ToString());
                Thread.Sleep(mode == "linger" ? 120000 : 500);
                if (mode == "brief") File.WriteAllText(arguments[1] + ".done", desktop);
            }
            Console.WriteLine("visible=True; active=True; paints=" + paints + "; desktop=" + desktop);
            return 0;
        }
        finally { DestroyWindow(window); }
    }

    private static IntPtr OnMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam)
    {
        if (message == 0xF) paints++;
        return DefWindowProc(window, message, wparam, lparam);
    }

    private static string Name(IntPtr handle)
    {
        var value = new StringBuilder(512);
        uint needed;
        if (!GetUserObjectInformation(handle, 2, value, 1024, out needed)) throw new Exception("Object name failed.");
        return value.ToString();
    }

    private delegate IntPtr WindowProc(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass
    {
        public uint Style;
        public IntPtr Procedure;
        public int ClassExtra, WindowExtra;
        public IntPtr Instance, Icon, Cursor, Background;
        public string Menu, Name;
    }
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] private static extern IntPtr GetConsoleWindow();
    [DllImport("kernel32.dll")] private static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool inJob);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string name);
    [DllImport("user32.dll")] private static extern IntPtr GetThreadDesktop(uint thread);
    [DllImport("user32.dll")] private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetUserObjectInformation(IntPtr handle, int kind, StringBuilder text, uint length, out uint needed);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern ushort RegisterClass(ref WindowClass windowClass);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CreateWindowEx(uint extended, string name, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr DefWindowProc(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr window, int show);
    [DllImport("user32.dll")] private static extern IntPtr SetActiveWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetActiveWindow();
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool InvalidateRect(IntPtr window, IntPtr rectangle, bool erase);
    [DllImport("user32.dll")] private static extern bool UpdateWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);
}
'@ | Set-Content -LiteralPath $fixtureSource -Encoding utf8
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    $compileExit = Invoke-ProbeDesktopProcess -FilePath $compiler -Arguments @(
        '/nologo', '/target:exe', "/out:$fixture", $fixtureSource
    ) -TimeoutSeconds 30
    Assert-ProbeContract ($compileExit -eq 0) 'Fixture compilation failed.'
    Write-Host 'PASS: C# library and isolated native fixture compilation'

    $arguments = @('', 'plain', 'two words', 'quote"inside', 'slash\', 'two \\"quoted"', "line`nbreak", '한글', '$() ; & literal')
    $result = [MacType.ControlCenter.Ci.ProbeDesktop]::Run(
        $fixture, (@('args') + $arguments), $directory, 15000, 65536, $false
    )
    Assert-ProbeContract ($result.ExitCode -eq 37) 'Nonzero child exit code was lost.'
    $encoded = @($arguments | ForEach-Object { [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($_)) })
    Assert-ProbeContract ($result.StandardOutput -ceq (($encoded -join "`r`n") + "`r`n")) 'Native argument boundaries changed.'
    Assert-ProbeContract ($result.StandardError -ceq 'stderr sentinel') 'Stderr was not captured separately.'
    Write-Host 'PASS: native argv (empty, quotes, backslashes, whitespace, Unicode), stderr, exit 37'

    $visible = [MacType.ControlCenter.Ci.ProbeDesktop]::Run($fixture, @('visible'), $directory, 15000, 65536, $false)
    Assert-ProbeContract ($visible.ExitCode -eq 0) "Private visible/paint fixture failed: $($visible.ExitCode)"
    Assert-ProbeContract ($visible.StandardOutput -match 'visible=True; active=True; paints=[1-9]') 'Visible window did not paint or activate.'
    Assert-ProbeContract ($visible.DesktopName -ne $inputBefore) 'Child used the input desktop.'
    if ($isolatedBefore) {
        Assert-ProbeContract ($visible.DesktopName -eq $before) 'Already isolated desktop was not reused.'
    }
    else {
        Assert-ProbeContract ($visible.DesktopName -ne $before) 'Child used the caller desktop.'
        Assert-ProbeContract (-not [ProbeDesktopContractHandles]::Exists($visible.DesktopName)) 'Completed desktop handle leaked.'
    }
    Write-Host "PASS: $($visible.StandardOutput.Trim()); no console; job assigned; desktop ownership verified"

    $bytes = [MacType.ControlCenter.Ci.ProbeDesktop]::Run($fixture, @('bytes'), $directory, 15000, 65536, $false)
    Assert-ProbeContract ($bytes.ExitCode -eq 0 -and
        [Convert]::ToHexString($bytes.StandardOutputBytes) -eq '0080FF0A' -and
        [Convert]::ToHexString($bytes.StandardErrorBytes) -eq '0080FF0A') 'Raw output bytes changed.'
    Write-Host 'PASS: non-UTF8 stdout/stderr bytes preserved'

    $scriptFixture = Join-Path $directory "script ' with spaces.ps1"
    $receipt = Join-Path $directory 'script receipt.clixml'
    @'
param(
    [string] $Library, [string] $Receipt, [string] $ExpectedDirectory, [string] $NativeFixture,
    [bool] $Enabled, [int] $Count, [string[]] $Values,
    [switch] $On, [switch] $Off, [int] $ExitCode
)
$ErrorActionPreference = 'Stop'
. $Library
if (-not (Test-ProbeDesktopContext)) { throw 'Actual private desktop context missing.' }
if ((Get-Location).ProviderPath -ne $ExpectedDirectory) { throw 'PowerShell working directory changed.' }
if ([Environment]::CurrentDirectory -ne $ExpectedDirectory) { throw 'Native working directory changed.' }
if ($env:MACTYPE_PROBE_CONTRACT_VALUE -ne 'inherited environment; no shell expansion') { throw 'Environment changed.' }
$nested = [MacType.ControlCenter.Ci.ProbeDesktop]::Run($NativeFixture, @('visible'), $ExpectedDirectory, 15000, 65536, $false)
if ($nested.ExitCode -ne 0 -or $nested.DesktopName -ne [MacType.ControlCenter.Ci.ProbeDesktop]::CurrentDesktopName) {
    throw 'Nested native probe did not reuse the actual isolated desktop.'
}
$commandExit = Invoke-ProbeDesktopCommand -Executable $NativeFixture -Arguments @('visible') -WorkingDirectory $ExpectedDirectory -TimeoutSeconds 15
if ($commandExit -isnot [int] -or $commandExit -ne 0) { throw 'Command wrapper result changed.' }
@{ Enabled = $Enabled; Count = $Count; Values = $Values; On = $On.IsPresent; Off = $Off.IsPresent } |
    Export-Clixml -LiteralPath $Receipt
[Console]::Out.WriteLine('script stdout sentinel')
[Console]::Error.WriteLine('script stderr sentinel')
exit $ExitCode
'@ | Set-Content -LiteralPath $scriptFixture -Encoding utf8
    $parameters = @{
        Library = Join-Path $PSScriptRoot 'lib/ProbeDesktop.ps1'
        Receipt = $receipt
        NativeFixture = $fixture
        ExpectedDirectory = $directory
        Enabled = $false
        Count = 42
        Values = $arguments
        On = [System.Management.Automation.SwitchParameter]::new($true)
        Off = [System.Management.Automation.SwitchParameter]::new($false)
        ExitCode = 29
    }
    $scriptExit = Invoke-ProbeDesktopScript -ScriptPath $scriptFixture -Parameters $parameters `
        -TimeoutSeconds 30 -WorkingDirectory $directory -ScratchDirectory $directory
    Assert-ProbeContract ($scriptExit -is [int] -and $scriptExit -eq 29) 'Script exit result was not the single expected integer.'
    $restored = Import-Clixml -LiteralPath $receipt
    Assert-ProbeContract (-not $restored.Enabled -and $restored.Count -eq 42 -and $restored.On -and -not $restored.Off) 'Typed script parameters changed.'
    Assert-ProbeContract (($restored.Values | ConvertTo-Json -Compress) -ceq ($arguments | ConvertTo-Json -Compress)) 'Script array parameters changed.'
    Write-Host 'PASS: script path, boolean/array/integer/switch parameters, cwd, environment, context guard, exit 29'

    $handledFailure = Join-Path $directory 'handled native exit.ps1'
    @'
& $env:ComSpec /d /c exit 7
if ($LASTEXITCODE -ne 7) { throw 'Expected native exit 7.' }
Write-Host 'Handled native exit 7: PASS'
'@ | Set-Content -LiteralPath $handledFailure -Encoding utf8
    $handledExit = Invoke-ProbeDesktopScript -ScriptPath $handledFailure -TimeoutSeconds 30 `
        -WorkingDirectory $directory -ScratchDirectory $directory
    Assert-ProbeContract ($handledExit -is [int] -and $handledExit -eq 0) 'Handled native failure overrode successful script completion.'
    Write-Host 'PASS: successful script completion overrides a handled native nonzero exit'

    $terminatingFailure = Join-Path $directory 'terminating error.ps1'
    @'
$global:LASTEXITCODE = $null
throw 'Expected terminating fixture error.'
'@ | Set-Content -LiteralPath $terminatingFailure -Encoding utf8
    $terminatingExit = Invoke-ProbeDesktopScript -ScriptPath $terminatingFailure -TimeoutSeconds 30 `
        -WorkingDirectory $directory -ScratchDirectory $directory
    Assert-ProbeContract ($terminatingExit -is [int] -and $terminatingExit -eq 1) 'Terminating script error did not return failure.'
    Write-Host 'PASS: terminating script error returns 1 even without a native exit code'

    $orphanReceipt = Join-Path $directory 'orphan.pid'
    $orphan = [MacType.ControlCenter.Ci.ProbeDesktop]::Run($fixture, @('orphan', $orphanReceipt), $directory, 15000, 65536, $false)
    Assert-ProbeContract ($orphan.ExitCode -eq 23 -and (Test-Path -LiteralPath ($orphanReceipt + '.done'))) 'Runner returned before the descendant completed.'
    Write-Host 'PASS: desktop lifetime includes descendants after the root exits'

    $pidFile = Join-Path $directory 'timeout.pid'
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    Assert-ProbeFailure {
        [void] [MacType.ControlCenter.Ci.ProbeDesktop]::Run($fixture, @('tree', $pidFile), $directory, 3000, 65536, $false)
    } 'deadline'
    Assert-ProbeContract ($elapsed.Elapsed.TotalSeconds -lt 15) 'Timeout cleanup exceeded its bound.'
    foreach ($path in @($pidFile, ($pidFile + '.parent'))) {
        Assert-ProbeContract (Test-Path -LiteralPath $path) 'Timeout fixture did not start the full tree.'
        $processId = [int] (Get-Content -LiteralPath $path -Raw)
        Assert-ProbeContract ($null -eq (Get-Process -Id $processId -ErrorAction SilentlyContinue)) 'Timed-out process survived cleanup.'
    }
    Write-Host 'PASS: timeout kills root and visible descendant; inherited output handles do not hang'

    Assert-ProbeFailure {
        [void] [MacType.ControlCenter.Ci.ProbeDesktop]::Run($fixture, @('flood'), $directory, 15000, 4096, $false)
    } 'output limit'
    Write-Host 'PASS: bounded stdout/stderr capture terminates flooding tree'

    $badExecutable = Join-Path $directory 'not executable.exe'
    Set-Content -LiteralPath $badExecutable -Value 'not a PE image'
    Assert-ProbeFailure {
        [void] [MacType.ControlCenter.Ci.ProbeDesktop]::Run($badExecutable, @(), $directory, 15000, 4096, $false)
    } 'CreateProcessW'
    Assert-ProbeContract ([MacType.ControlCenter.Ci.ProbeDesktop]::CurrentDesktopName -eq $before) 'Caller thread desktop changed.'
    Assert-ProbeContract ([MacType.ControlCenter.Ci.ProbeDesktop]::InputDesktopName -eq $inputBefore) 'Input desktop changed.'
    Write-Host 'PASS: creation failure fails closed; caller thread and input desktop unchanged'
    Write-Host 'Probe desktop isolation contract passed. Interactive mode was not exercised.'
}
finally {
    $env:MACTYPE_PROBE_CONTRACT_VALUE = $oldEnvironment
    Remove-Item -LiteralPath $directory -Recurse -Force
}
