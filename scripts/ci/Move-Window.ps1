[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [int] $ProcessId,
    [ValidateSet('push', 'drag')]
    [string] $Mode = 'push',
    [int] $Steps = 240,
    [int] $IntervalMs = 8,
    [int] $Amplitude = 120,
    # Rows below the top edge where the drag grabs the window: past the
    # sizing zone, inside the title strip.
    [int] $GrabOffsetY = 20
)

# Moves one top-level window the way a drag does and reports how the window
# kept up. `push` calls SetWindowPos from this process: the call returns only
# after the window's thread has handled WM_WINDOWPOSCHANGING/CHANGED, so its
# duration is the target thread's cost per move step. `drag` presses the
# mouse on the window's title strip through SendInput and moves the pointer,
# so Windows runs its own modal move loop on the window's thread; the window's
# distance behind the pointer after each step is the visible lag. Output is
# one JSON object on stdout.

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public static class WindowMover
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

    [DllImport("user32.dll", SetLastError = true)] static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT point);
    [DllImport("user32.dll")] static extern uint SendInput(uint count, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern IntPtr SetProcessDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("winmm.dll")] static extern uint timeBeginPeriod(uint period);
    [DllImport("winmm.dll")] static extern uint timeEndPeriod(uint period);

    const uint SWP_NOSIZE = 0x0001, SWP_NOZORDER = 0x0004, SWP_NOACTIVATE = 0x0010;
    const uint MOUSEEVENTF_MOVE = 0x0001, MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004, MOUSEEVENTF_ABSOLUTE = 0x8000;
    const int SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79, SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77;

    public static void PrepareDpi()
    {
        try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch (Exception) { }
    }

    static void Pace(Stopwatch clock, double deadlineMs)
    {
        while (clock.Elapsed.TotalMilliseconds < deadlineMs)
        {
            double remaining = deadlineMs - clock.Elapsed.TotalMilliseconds;
            if (remaining > 2) Thread.Sleep(1); else Thread.SpinWait(200);
        }
    }

    static void Offset(int step, int steps, int amplitude, out int dx, out int dy)
    {
        double angle = 2 * Math.PI * step / steps;
        dx = (int)Math.Round(amplitude * Math.Sin(angle));
        dy = (int)Math.Round(amplitude * 0.5 * (1 - Math.Cos(angle)));
    }

    public static double[] Push(IntPtr hwnd, int steps, int intervalMs, int amplitude)
    {
        RECT origin;
        GetWindowRect(hwnd, out origin);
        var latencies = new double[steps];
        var call = new Stopwatch();
        var clock = Stopwatch.StartNew();
        timeBeginPeriod(1);
        try
        {
            for (int i = 0; i < steps; i++)
            {
                int dx, dy;
                Offset(i + 1, steps, amplitude, out dx, out dy);
                call.Restart();
                SetWindowPos(hwnd, IntPtr.Zero, origin.Left + dx, origin.Top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                latencies[i] = call.Elapsed.TotalMilliseconds;
                Pace(clock, (i + 1) * (double)intervalMs);
            }
        }
        finally
        {
            timeEndPeriod(1);
            SetWindowPos(hwnd, IntPtr.Zero, origin.Left, origin.Top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return latencies;
    }

    static INPUT MouseAt(int x, int y, uint flags)
    {
        int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        var input = new INPUT();
        input.type = 0;
        input.mi.dx = (int)Math.Round((x - left) * 65535.0 / (width - 1));
        input.mi.dy = (int)Math.Round((y - top) * 65535.0 / (height - 1));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | flags;
        return input;
    }

    static bool Send(INPUT input)
    {
        return SendInput(1, new INPUT[] { input }, Marshal.SizeOf(typeof(INPUT))) == 1;
    }

    // Returns rows of {step, cursorX, cursorY, windowLeft, windowTop} plus the grab point.
    public static List<int[]> Drag(IntPtr hwnd, int steps, int intervalMs, int amplitude, int grabOffsetX, int grabOffsetY, out int[] grab, out bool inputAccepted)
    {
        RECT origin;
        GetWindowRect(hwnd, out origin);
        int grabX = origin.Left + grabOffsetX, grabY = origin.Top + grabOffsetY;
        grab = new int[] { grabX, grabY, origin.Left, origin.Top };
        var rows = new List<int[]>();
        inputAccepted = true;
        var clock = Stopwatch.StartNew();
        timeBeginPeriod(1);
        try
        {
            inputAccepted &= Send(MouseAt(grabX, grabY, 0));
            Thread.Sleep(60);
            inputAccepted &= Send(MouseAt(grabX, grabY, MOUSEEVENTF_LEFTDOWN));
            Thread.Sleep(120);
            // A first move past the drag threshold commits the drag before the
            // measured path starts; the lag is measured from the position the
            // window reports once the modal loop has picked it up.
            inputAccepted &= Send(MouseAt(grabX + 12, grabY + 12, 0));
            Thread.Sleep(150);
            inputAccepted &= Send(MouseAt(grabX, grabY, 0));
            Thread.Sleep(150);
            clock.Restart();
            for (int i = 0; i < steps; i++)
            {
                int dx, dy;
                Offset(i + 1, steps, amplitude, out dx, out dy);
                inputAccepted &= Send(MouseAt(grabX + dx, grabY + dy, 0));
                Pace(clock, (i + 1) * (double)intervalMs);
                POINT cursor; RECT now;
                GetCursorPos(out cursor);
                GetWindowRect(hwnd, out now);
                rows.Add(new int[] { i + 1, cursor.X, cursor.Y, now.Left, now.Top, now.Right - now.Left, now.Bottom - now.Top });
            }
        }
        finally
        {
            Send(MouseAt(grabX, grabY, 0));
            Thread.Sleep(60);
            Send(MouseAt(grabX, grabY, MOUSEEVENTF_LEFTUP));
            Thread.Sleep(120);
            timeEndPeriod(1);
            SetWindowPos(hwnd, IntPtr.Zero, origin.Left, origin.Top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return rows;
    }

    public static int[] Rect(IntPtr hwnd)
    {
        RECT rect;
        GetWindowRect(hwnd, out rect);
        return new int[] { rect.Left, rect.Top, rect.Right, rect.Bottom };
    }
}
"@

[WindowMover]::PrepareDpi()
$process = Get-Process -Id $ProcessId
$deadline = [DateTime]::UtcNow.AddSeconds(30)
while ($process.MainWindowHandle -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 200
    $process.Refresh()
}
$hwnd = $process.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw "Process $ProcessId has no main window." }

function Percentile([double[]] $values, [double] $fraction) {
    if ($values.Count -eq 0) { return 0 }
    $sorted = $values | Sort-Object
    $index = [Math]::Min($sorted.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($fraction * $sorted.Count) - 1))
    return [Math]::Round($sorted[$index], 3)
}

if ($Mode -eq 'push') {
    $latencies = [WindowMover]::Push($hwnd, $Steps, $IntervalMs, $Amplitude)
    $mean = ($latencies | Measure-Object -Average).Average
    [pscustomobject]@{
        mode = 'push'
        steps = $Steps
        intervalMs = $IntervalMs
        amplitude = $Amplitude
        meanMs = [Math]::Round($mean, 3)
        p50Ms = Percentile $latencies 0.5
        p95Ms = Percentile $latencies 0.95
        maxMs = [Math]::Round(($latencies | Measure-Object -Maximum).Maximum, 3)
        over16Ms = @($latencies | Where-Object { $_ -gt 16 }).Count
        latenciesMs = @($latencies | ForEach-Object { [Math]::Round($_, 2) })
    } | ConvertTo-Json -Compress -Depth 4
    exit 0
}

$rect = [WindowMover]::Rect($hwnd)
$grabOffsetX = [Math]::Max(80, [int](($rect[2] - $rect[0]) * 0.45))
$grabOffsetY = $GrabOffsetY
$grab = New-Object 'int[]' 4
$accepted = $false
$rows = [WindowMover]::Drag($hwnd, $Steps, $IntervalMs, $Amplitude, $grabOffsetX, $grabOffsetY, [ref] $grab, [ref] $accepted)
$lags = New-Object System.Collections.Generic.List[double]
$positions = New-Object System.Collections.Generic.HashSet[string]
# The window's offset from the pointer once the modal loop holds it is the
# reference; a window that then falls behind the pointer shows as lag.
$reference = $rows[0]
$offsetLeft = $reference[3] - $reference[1]
$offsetTop = $reference[4] - $reference[2]
foreach ($row in $rows) {
    $expectedLeft = $row[1] + $offsetLeft
    $expectedTop = $row[2] + $offsetTop
    $lag = [Math]::Sqrt([Math]::Pow($expectedLeft - $row[3], 2) + [Math]::Pow($expectedTop - $row[4], 2))
    $lags.Add($lag)
    [void] $positions.Add("$($row[3]),$($row[4])")
}
$moved = ($positions.Count -gt 1)
$sizes = New-Object System.Collections.Generic.HashSet[string]
foreach ($row in $rows) { [void] $sizes.Add("$($row[5])x$($row[6])") }
$resized = ($sizes.Count -gt 1)
[pscustomobject]@{
    mode = 'drag'
    steps = $Steps
    intervalMs = $IntervalMs
    amplitude = $Amplitude
    inputAccepted = $accepted
    windowMoved = ($moved -and -not $resized)
    windowResized = $resized
    distinctPositions = $positions.Count
    meanLagPx = [Math]::Round(($lags | Measure-Object -Average).Average, 2)
    p95LagPx = Percentile ([double[]] $lags.ToArray()) 0.95
    maxLagPx = [Math]::Round(($lags | Measure-Object -Maximum).Maximum, 2)
    stepsOver8Px = @($lags | Where-Object { $_ -gt 8 }).Count
    grab = $grab
    rows = @($rows | ForEach-Object { ,@($_) })
} | ConvertTo-Json -Compress -Depth 5
