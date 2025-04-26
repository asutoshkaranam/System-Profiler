# System-Profiler
A C++ application that monitors and displays key system metrics including CPU usage, memory usage, disk usage, and active processes. Utilizes fork() and inter-process communication (IPC) to delegate different monitoring tasks to separate processes, improving resource management, responsiveness, and system efficiency.

# Usage Guide
Building :
g++ SystemProfiler.cpp -o system_profiler

Usage :
./system_profiler [--interval <seconds>] [--log] [--logfile <path>]

Options :
--interval <seconds>: Set the update interval (default: 3 seconds)
--log: Enable logging to the default log file (system_profiler.log)

Examples:

Run with default settings (3-second interval, no text file logging):
./system_profiler

Run with a 5-second update interval:
./system_profiler --interval 5

Run with logging enabled:
./system_profiler --log
