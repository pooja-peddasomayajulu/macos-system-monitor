System Monitor

A lightweight terminal-based system monitor for macOS written in C.

The project uses native macOS APIs to collect CPU, memory, disk, system, network, and process information, then displays the results in a continuously refreshing terminal interface.



Features

System-wide CPU usage

Number of logical CPUs

Memory usage and memory breakdown

Disk usage for the root filesystem (/)

System uptime

1, 5, and 15 minute load averages

Cumulative network bytes received and transmitted

Top processes by CPU usage

Top processes by memory usage

Configurable refresh interval

Configurable number of displayed processes

Ctrl+C terminal cleanup

Alternate terminal screen so the normal terminal history is preserved

ANSI-colored usage bars for easier visual scanning

Requirements

macOS

Clang / Xcode Command Line Tools

A terminal that supports ANSI escape sequences

This project uses macOS-specific APIs such as Mach, libproc, sysctl, statfs, and getifaddrs, so it is not intended to be portable to Linux or Windows without replacing those parts.

Build

Clone the repository and enter the project directory:

git clone <your-repository-url>
cd system-monitor

Build with the Makefile:

make

Or build directly with Clang:

clang -Wall -Wextra -Wpedantic -O2 main.c -o system-monitor

Run

Start the monitor with the default settings:

make run

Or:

./system-monitor

The default refresh interval is 1 second and the default process count is 10.

Press Ctrl+C to stop the monitor and return to the normal terminal screen.

Command-line options

Usage: ./system-monitor [options]

Options:
  -i, --interval SECONDS   Set refresh interval
  -t, --top NUMBER         Number of processes to show
  -h, --help               Show this help message

Examples:

./system-monitor --interval 2
./system-monitor --top 20
./system-monitor --interval 2 --top 20

Makefile commands

Command

Purpose

make

Build the program

make run

Build and run the monitor

make clean

Remove the compiled binary

make help

Show available Makefile commands

How it works

CPU

The monitor takes two system CPU snapshots separated by the configured sampling interval. It compares user, system, idle, and nice CPU tick counts to calculate overall CPU usage.

For each process, it takes two snapshots and matches the same process using its PID. The difference between the process's cumulative CPU-time counters gives the CPU time consumed during the sampling interval.

Memory

Memory statistics are collected with Mach host APIs. Page counts are converted to bytes using the system page size. In this project, inactive memory is treated as reclaimable when calculating the displayed used-memory value.

Disk

Disk statistics are collected for the root filesystem (/) using statfs().

System information

The monitor reads the boot time through kern.boottime and calculates uptime from the current wall-clock time. Load averages are obtained through getloadavg().

Network

Network interface statistics are collected with getifaddrs(). The monitor adds the received and transmitted byte counters from the available link-layer interfaces.

Processes

The process list is obtained with libproc. Each process record contains its PID, name, resident memory, cumulative CPU time, and calculated CPU percentage. Separate sorted views are used for CPU and memory.

Terminal UI

The monitor runs inside the terminal's alternate screen and redraws the display in place. Standard output is fully buffered so a complete frame is flushed together instead of being displayed line-by-line.

Usage bars change color based on utilization:

Green: below 60%

Yellow: 60% to below 80%

Red: 80% or higher

The program also restores the cursor and exits the alternate screen when Ctrl+C is pressed.

Project structure

system-monitor/
├── main.c
├── Makefile
├── README.md
├── .gitignore
└── assets/
    └── system-monitor.png

Current scope

This is a terminal system-monitoring project rather than a full replacement for macOS Activity Monitor. The focus is on learning how to collect operating-system statistics with C and present them in a continuously updating CLI interface.

License

No license has been selected yet. If this project will be distributed publicly, add an appropriate license file such as MIT before treating the repository as a reusable open-source project.
