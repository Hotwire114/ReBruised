// Windows pieces of the launcher, kept apart from raylib (windows.h and raylib.h define the same names).
#include <cstdlib>
#include <string>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

// Open-file dialog for the player's disc image; empty when cancelled.
std::string bnbPickDiscImage()
{
    char file[MAX_PATH * 4] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "PS2 disc image (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = "Select your Black & Bruised disc image (PAL, SLES-51620)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameA(&ofn) ? std::string(file) : std::string();
}

// An error box (no window of our own: e.g. OpenGL could not start).
void bnbErrorBox(const std::string &text)
{
    MessageBoxA(nullptr, text.c_str(), "ReBruised", MB_ICONERROR | MB_OK);
}

// %LOCALAPPDATA% (per-user data: the files taken from the player's disc, saves)
std::string bnbLocalAppData()
{
    char path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
        return path;
    const char *e = std::getenv("LOCALAPPDATA");
    return e ? e : "";
}

std::string bnbExePath()
{
    char buf[MAX_PATH * 2] = {};
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return buf;
}

// Starts the game detached, without a console window, stdout/stderr into the given log files.
bool bnbLaunch(const std::string &exe, const std::string &args, const std::string &cwd, const std::string &logOut,
               const std::string &logErr)
{
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileA(logOut.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileA(logErr.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out != INVALID_HANDLE_VALUE ? out : nullptr;
    si.hStdError = err != INVALID_HANDLE_VALUE ? err : nullptr;
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + exe + "\" " + args;
    const BOOL ok = CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                                   nullptr, cwd.c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
    if (!ok)
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---- problem reports ----
// Opens a URL, file or folder with its default program.
void bnbOpenTarget(const std::string &target)
{
    ShellExecuteA(nullptr, "open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Explorer with the file selected.
void bnbShowInFolder(const std::string &file)
{
    const std::string args = "/select,\"" + file + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::string bnbDesktopDir()
{
    char path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path)))
        return path;
    return {};
}

bool bnbProcessAlive(unsigned long pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// Runs a command line without a window and waits (up to timeoutMs); true when it exited with 0.
bool bnbRunWait(const std::string &cmdLine, const std::string &cwd, unsigned long timeoutMs)
{
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdLine;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Starts a command line without a window, stdout+stderr into logFile; returns a handle for bnbPoll (null on failure).
void *bnbSpawn(const std::string &cmdLine, const std::string &cwd, const std::string &logFile)
{
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE log = CreateFileA(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log != INVALID_HANDLE_VALUE ? log : nullptr;
    si.hStdError = si.hStdOutput;
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdLine;
    // the build (python and every compiler it starts) ends with the launcher: a job that kills its processes on close
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectA(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (j) SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li));
        return j;
    }();
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok)
        return nullptr;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// -1 while the process runs; its exit code once it ended (the handle is then closed).
int bnbPoll(void *process)
{
    if (!process)
        return 1;
    if (WaitForSingleObject(static_cast<HANDLE>(process), 0) == WAIT_TIMEOUT)
        return -1;
    DWORD code = 1;
    GetExitCodeProcess(static_cast<HANDLE>(process), &code);
    CloseHandle(static_cast<HANDLE>(process));
    return static_cast<int>(code);
}

static std::string regString(HKEY root, const char *key, const char *value)
{
    char buf[512] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return {};
}

// Windows version, CPU and memory for the report.
std::string bnbSystemInfo()
{
    const char *nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string s = "Windows: " + regString(HKEY_LOCAL_MACHINE, nt, "ProductName") + " " +
                    regString(HKEY_LOCAL_MACHINE, nt, "DisplayVersion") + " (build " +
                    regString(HKEY_LOCAL_MACHINE, nt, "CurrentBuild") + ")\n";
    s += "CPU: " + regString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString") +
         " (" + std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) + " threads)\n";
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem))
        s += "Memory: " + std::to_string(mem.ullTotalPhys / (1024ull * 1024ull * 1024ull)) + " GB\n";
    return s;
}
#else
void bnbErrorBox(const std::string &) {}
void bnbOpenTarget(const std::string &) {}
void bnbShowInFolder(const std::string &) {}
std::string bnbDesktopDir() { return {}; }
bool bnbProcessAlive(unsigned long) { return false; }
bool bnbRunWait(const std::string &, const std::string &, unsigned long) { return false; }
void *bnbSpawn(const std::string &, const std::string &, const std::string &) { return nullptr; }
int bnbPoll(void *) { return 1; }
std::string bnbSystemInfo() { return {}; }
std::string bnbPickDiscImage() { return {}; }
std::string bnbLocalAppData() { return {}; }
std::string bnbExePath() { return {}; }
bool bnbLaunch(const std::string &, const std::string &, const std::string &, const std::string &, const std::string &) { return false; }
#endif
