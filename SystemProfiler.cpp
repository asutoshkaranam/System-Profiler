#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <csignal>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <vector>

using namespace std;

class SystemProfiler {
private:
    
	// Result Structure to store the Profiling system metrics in shared memory
    struct SystemMetrics {
        double cpuUsage;
        double memoryTotal;
        double memoryUsed;
        double diskTotal;
        double diskUsed;
        double diskUsedPercent;
        int runningProcesses;
        bool updated[4]; // Status Tracker to check which metrics have been updated
    };

    
    // Profiling Configuration members
    int updateInterval;
    bool logMode;
    string logFilePath;
    ofstream logFile;
    
    // Shared memory variables
    int shmid;
    SystemMetrics* sharedMetrics;
    key_t shmKey;
    
    // Maintain the List of All Processes created
    vector<pid_t> childPids;
    volatile static sig_atomic_t shouldExit;

    static const unsigned int GB = (1024 * 1024) * 1024;
	
	//Register for the SIGINT event which will be raised on CTRL-C
    void setupSignalHandling() {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handleSignal;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }
	//Reference - https://stackoverflow.com/questions/368927/call-a-function-when-the-program-is-finished-with-ctrl-c
    
    bool initializeSharedMemory() {
        shmKey = ftok("/tmp", 'S');
        shmid = shmget(shmKey, sizeof(SystemMetrics), IPC_CREAT | 0666);
        if (shmid == -1) {
            cerr << "Failed to create shared memory: " << strerror(errno) << endl;
            return false;
        }

        sharedMetrics = static_cast<SystemMetrics*>(shmat(shmid, nullptr, 0));
        if (sharedMetrics == (void*)-1) {
            cerr << "Failed to attach to shared memory: " << strerror(errno) << endl;
            shmctl(shmid, IPC_RMID, nullptr);
            return false;
        }

        // Initialize shared memory
        memset(sharedMetrics, 0, sizeof(SystemMetrics));
        return true;
    }
    
	bool startMonitorProcesses() {
		// CPU monitoring process
		pid_t cpuPid = fork();
		if (cpuPid == 0) {
			monitorCPU();
			exit(EXIT_SUCCESS);
		} else if (cpuPid < 0) {
			std::cerr << "Failed to fork CPU monitor process\n";
			return false;
		}
		childPids.push_back(cpuPid);
		std::cout << "Started CPU monitor with PID: " << cpuPid << std::endl;
		
		// Memory monitoring process
		pid_t memoryPid = fork();
		if (memoryPid == 0) {
			monitorMemory();
			exit(EXIT_SUCCESS);
		} else if (memoryPid < 0) {
			std::cerr << "Failed to fork memory monitor process\n";
			kill(cpuPid, SIGTERM);
			waitpid(cpuPid, nullptr, 0);
			return false;
		}
		childPids.push_back(memoryPid);
		std::cout << "Started memory monitor with PID: " << memoryPid << std::endl;
		
		// Disk monitoring process
		pid_t diskPid = fork();
		if (diskPid == 0) {
			monitorDisk();
			exit(EXIT_SUCCESS);
		} else if (diskPid < 0) {
			std::cerr << "Failed to fork disk monitor process\n";
			kill(cpuPid, SIGTERM);
			kill(memoryPid, SIGTERM);
			waitpid(cpuPid, nullptr, 0);
			waitpid(memoryPid, nullptr, 0);
			return false;
		}
		childPids.push_back(diskPid);
		std::cout << "Started disk monitor with PID: " << diskPid << std::endl;
		
		// Process monitoring process
		pid_t processesPid = fork();
		if (processesPid == 0) {
			monitorProcesses();
			exit(EXIT_SUCCESS);
		} else if (processesPid < 0) {
			std::cerr << "Failed to fork processes monitor process\n";
			kill(cpuPid, SIGTERM);
			kill(memoryPid, SIGTERM);
			kill(diskPid, SIGTERM);
			waitpid(cpuPid, nullptr, 0);
			waitpid(memoryPid, nullptr, 0);
			waitpid(diskPid, nullptr, 0);
			return false;
		}
		childPids.push_back(processesPid);
		std::cout << "Started process monitor with PID: " << processesPid << std::endl;
		
		return true;
	}
    
    void terminateChildProcesses() {
        for (pid_t pid : childPids) {
            kill(pid, SIGTERM);
        }
        
        for (pid_t pid : childPids) {
            waitpid(pid, nullptr, 0);
        }
        
        childPids.clear();
    }
    
	// Monitor methods that run in separate processes
    void monitorCPU() {
        ifstream statFile;
        string line;
        unsigned long long prevIdle = 0, prevTotal = 0;
        unsigned long long idle, total;
        bool skipOnce = true;

        while (!shouldExit) {
            statFile.open("/proc/stat");
            if (statFile.is_open()) {
                getline(statFile, line);
                statFile.close();

                istringstream iss(line);
                string cpu;
                unsigned long long user, nice, system, idle_time, iowait, irq, softirq, steal, guest, guest_nice;
                
                iss >> cpu >> user >> nice >> system >> idle_time >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;
                
                idle = idle_time + iowait;
                total = user + nice + system + idle + iowait + irq + softirq + steal;

                if (skipOnce) {
                    prevIdle = idle;
                    prevTotal = total;
                    skipOnce = false;
                }
                else {
                    unsigned long long totalDiff = total - prevTotal;
                    unsigned long long idleDiff = idle - prevIdle;
                    
                    if (totalDiff > 0) {
                        sharedMetrics->cpuUsage = 100.0 * (1.0 - static_cast<double>(idleDiff) / totalDiff);
                        sharedMetrics->updated[0] = true;
                    }
                    prevIdle = idle;
                    prevTotal = total;
                }
            }
            sleep(1);
        }
    }
	////reference - https://stackoverflow.com/questions/23367857/accurate-calculation-of-cpu-usage-given-in-percentage-in-linux

    // Get memory usage - Reading File - Can also use SysInfo System Call
	void monitorMemory() {
        ifstream memFile;
        string line;
        unordered_map<string, unsigned long> memInfo;

        while (!shouldExit) {
            memFile.open("/proc/meminfo");
            if (memFile.is_open()) {
                while (getline(memFile, line)) {
                    istringstream iss(line);
                    string key;
                    unsigned long value;
                    string unit;

                    iss >> key >> value >> unit;
                    // Delete the trailing ':' in the keys
                    if (key.back() == ':') {
                        key.pop_back();
                    }
                    // Save in Map
                    memInfo[key] = value;
                }
                memFile.close();

                sharedMetrics->memoryTotal = memInfo["MemTotal"] / 1024.0 / 1024.0;
                double memFree = (memInfo["MemFree"] + memInfo["Buffers"] + memInfo["Cached"]) / 1024.0 / 1024.0;
                sharedMetrics->memoryUsed = sharedMetrics->memoryTotal - memFree;
                sharedMetrics->updated[1] = true;
            }
            sleep(1);
        }
    }
	//Reference - https://stackoverflow.com/questions/41224738/how-to-calculate-system-memory-usage-from-proc-meminfo-like-htop , https://stackoverflow.com/questions/1460248/on-linux-how-should-i-calculate-the-amount-of-free-memory-from-the-information
	
    void monitorDisk() {
        struct statvfs diskStat;

        while (!shouldExit) {
            if (statvfs("/", &diskStat) == 0) {
                double total = (double)(diskStat.f_blocks * diskStat.f_frsize) / GB;
                double available = (double)(diskStat.f_bfree * diskStat.f_frsize) / GB;
                double used = total - available;
                double usedPercentage = (used / total) * 100.0;

                sharedMetrics->diskTotal = total;
                sharedMetrics->diskUsed = used;
                sharedMetrics->diskUsedPercent = usedPercentage;
                sharedMetrics->updated[2] = true;
            }
            sleep(1);
        }
    }
	//Reference - https://gist.github.com/vgerak/8539104

	// Count the # of running processes
    void monitorProcesses() {
        DIR* procDir;
        struct dirent* entry;
        int count;
        char path[256];
        char cmdline[256];

        while (!shouldExit) {
            procDir = opendir("/proc");
            if (procDir) {
                count = 0;
                while ((entry = readdir(procDir)) != nullptr) {
                    // Check if the directory name is a number which is the PID
                    if (entry->d_type == DT_DIR) {
                        bool isPid = true;
                        for (char* p = entry->d_name; *p; ++p) {
                            if (!isdigit(*p)) {
                                isPid = false;
                                break;
                            }
                        }
                        if (isPid) {
                            // Check if this is really a process
                            snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
                            FILE* cmdlineFile = fopen(path, "r");
                            if (cmdlineFile) {
                                if (fgets(cmdline, sizeof(cmdline), cmdlineFile) != nullptr) {
                                    count++;  // If we can read cmdline, it's likely a real process
                                }
                                fclose(cmdlineFile);
                            }
                        }
                    }
                }
                closedir(procDir);
                
                sharedMetrics->runningProcesses = count;
                sharedMetrics->updated[3] = true;
            }
            sleep(1);
        }
    }
	//References - https://stackoverflow.com/questions/29991182/programmatically-read-all-the-processes-status-from-proc
	
    void clearScreen() {
        cout << "\033[2J\033[1;1H"; // ANSI escape code to clear screen
    }
    
	//// Print formatted system metrics
    void printMetrics() {
        clearScreen();
        
        cout << "System Profiler" << endl;
        cout << "----------------------------------------------" << endl;
        
        cout << "CPU Usage: " << fixed << setprecision(1) 
                  << sharedMetrics->cpuUsage << "%" << endl;
        
        cout << "Memory Usage: " << fixed << setprecision(1) 
                  << sharedMetrics->memoryUsed << " GB / " 
                  << sharedMetrics->memoryTotal << " GB (" 
                  << (sharedMetrics->memoryUsed / sharedMetrics->memoryTotal * 100.0) << "%)" << endl;
        
        cout << "Disk Usage: " << fixed << setprecision(1) 
                  << sharedMetrics->diskUsed << " GB / " 
                  << sharedMetrics->diskTotal << " GB (" 
                  << sharedMetrics->diskUsedPercent << "%)" << endl;
        
        cout << "Running Processes: " << sharedMetrics->runningProcesses << endl;
        
        cout << "----------------------------------------------" << endl;
    }
    
    void logMetricsToFile() {
        auto now = chrono::system_clock::now();
        auto time = chrono::system_clock::to_time_t(now);
        
        logFile << "[" << put_time(localtime(&time), "%Y-%m-%d %H:%M:%S") << "] "
                
				<< "CPU: " << fixed << setprecision(1) << sharedMetrics->cpuUsage << "%, "
                
				<< "Memory: " << fixed << setprecision(1)<< sharedMetrics->memoryUsed << "GB/" << sharedMetrics->memoryTotal << "GB, "
                
				<< "Disk: " << fixed << setprecision(1)<< sharedMetrics->diskUsed << "GB/" << sharedMetrics->diskTotal << "GB, "
                
				<< "Processes: " << sharedMetrics->runningProcesses << endl;
    }
	//Reference - https://stackoverflow.com/questions/17223096/outputting-date-and-time-in-c-using-stdchrono
    
    void cleanupSharedMemory() {
        if (sharedMetrics != nullptr) {
            shmdt(sharedMetrics);
            sharedMetrics = nullptr;
        }
        
        if (shmid != -1) {
            shmctl(shmid, IPC_RMID, nullptr);
            shmid = -1;
        }
    }
    
    bool openLogFile() {
        if (logMode) {
            logFile.open(logFilePath, ios::out | ios::app);
            if (!logFile.is_open()) {
                cerr << "Failed to open log file: " << logFilePath << endl;
                return false;
            }
        }
        return true;
    }
    
    void closeLogFile() {
        if (logMode && logFile.is_open()) {
            logFile.close();
        }
    }
    
    void parseCommandLineArgs(int argc, char* argv[]) {
        for (int i = 1; i < argc; ++i) {
            string arg = argv[i];
            
            if (arg == "--interval") {
                if (i + 1 < argc) {
                    try {
                        updateInterval = stoi(argv[++i]);
                    } catch (const exception&) {
                        cerr << "Usage: " << argv[0] << " [--interval <seconds>] [--log]\n";
                        exit(EXIT_FAILURE);
                    }
                } else {
                    cerr << "Usage: " << argv[0] << " [--interval <seconds>] [--log]\n";
                    exit(EXIT_FAILURE);
                }
            } else if (arg == "--log") {
                logMode = true;
            } else {
                cerr << "Usage: " << argv[0] << " [--interval <seconds>] [--log]\n";
                exit(EXIT_FAILURE);
            }
        }
    }
    
    static void handleSignal(int signum) {
        shouldExit = 1;
    }

public:
    SystemProfiler() : 
        updateInterval(3),  // Default interval - 3 Seconds
        logMode(false),
        logFilePath("system_profiler.log"),
        shmid(-1),
        sharedMetrics(nullptr) {
    }
    
    ~SystemProfiler() {
        shutdown();
    }
    
    bool initialize(int argc, char* argv[]) {
        // Read the command line arguments and update data members
        parseCommandLineArgs(argc, argv);
        
        // Regsiter for signal handling
        setupSignalHandling();
        
        // Initialize IPC - shared memory
        if (!initializeSharedMemory()) {
            return false;
        }
        
        if (logMode && !openLogFile()) {
            cleanupSharedMemory();
            return false;
        }
        
        return true;
    }
    
    bool start() {
        if (!startMonitorProcesses()) {
            cleanupSharedMemory();
            closeLogFile();
            return false;
        }
        
        cout << "System Profiler started. Press Ctrl+C to exit.\n";
        cout << "Updating every " << updateInterval << " seconds...\n";
        
        while (!shouldExit) {
            // Wait for all metrics to be updated at least once
            bool allUpdated = sharedMetrics->updated[0] && 
                            sharedMetrics->updated[1] && 
                            sharedMetrics->updated[2] && 
                            sharedMetrics->updated[3];
            
            if (allUpdated) {
                printMetrics();
                
                if (logMode) {
                    logMetricsToFile();
                }
                
                cout << "(Updating every " << updateInterval << " seconds... Press Ctrl+C to exit)\n";
                sleep(updateInterval);
            } else {
                sleep(1);
            }
        }
        
        return true;
    }
    
    void shutdown() {
        terminateChildProcesses();
        
        cout << "\nShutting down...\n";
        
        closeLogFile();
        
        cleanupSharedMemory();
    }
};

//variable to signal main thread status/terminated
//volatile to not allow compiler optimizations
volatile sig_atomic_t SystemProfiler::shouldExit = 0;

int main(int argc, char* argv[]) {
    SystemProfiler profiler;
    
    if (!profiler.initialize(argc, argv)) {
        return EXIT_FAILURE;
    }
    
    if (!profiler.start()) {
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
}