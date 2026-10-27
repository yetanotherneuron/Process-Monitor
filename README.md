# Process Monitor

Process Monitor is a native Windows terminal application for viewing running processes and their resource usage. It uses the Win32 console APIs and native process-information APIs; no terminal emulation framework or third-party runtime is required.

## Features

- Live process table with name, PID, private memory, and sampled CPU usage.
- Type-to-search filtering by process name or PID.
- Background process sampling every two seconds, with an immediate refresh on F5.
- Colored title, search/sort line, table headings, alternating data rows, and status text.
- Bordered table with aligned columns.
- Incremental screen updates: unchanged rows and interface lines are not rewritten.
- Sort by name, PID, memory, or CPU.
- Export the visible, filtered rows to UTF-8 CSV.
- Show unavailable metrics when Windows denies access or a process exits.

## Requirements

- Windows 10 or Windows 11, x64
- Windows Terminal, PowerShell, Command Prompt, or another Windows console host
- Visual Studio with the MSVC C++ toolchain and Windows SDK
- CMake 3.20 or newer

## Build

Run these commands from a Visual Studio Developer PowerShell in the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

The executable is created at `build\Release\ProcessMonitor.exe`. Launch it from a terminal:

```powershell
.\build\Release\ProcessMonitor.exe
```

When launched from an existing terminal session, the program opens a separate terminal window for the monitor and leaves the original terminal available. The program requires an attached Windows console.

## Controls

- Type to search by process name or PID.
- **Escape** clears the search.
- **F5** requests an immediate process sample.
- **Ctrl+F** displays a search hint; type to filter.
- **Ctrl+E** prompts for a CSV file path and exports the visible rows.
- **F1** displays metric information in the status line.
- **1**, **2**, **3**, **4** select name, PID, memory, or CPU sorting.
- **Page Up** and **Page Down** navigate process pages.
- **Ctrl+C** exits cleanly.

The screen refreshes every two seconds. Only the table rows or interface lines whose displayed text changed are written again, avoiding a full-screen clear/redraw on every sample.

## Metrics and limitations

- **Private memory** is the process's private committed bytes (`PrivateUsage`), not working set or total system memory. Protected processes may not expose this metric.
- **CPU** is calculated from changes in process kernel and user CPU times between samples and normalized across the machine's active logical processors. A first sample, inaccessible CPU-time data, and new processes do not have a reading.
- Process enumeration may race with process start/exit. Unavailable per-process data is shown as `Unavailable`; a system-level enumeration error is shown in the status line.

## Release packaging

The application icon is embedded in the executable. To create a clean package:

```powershell
$package = "build\package"
if (Test-Path $package) { Remove-Item -LiteralPath $package -Recurse -Force }
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item "build\Release\ProcessMonitor.exe" "$package\ProcessMonitor.exe" -Force
Copy-Item "README.md" "$package\README.md" -Force
Compress-Archive -Path "$package\*" -DestinationPath "build\ProcessMonitor.zip" -Force
```
