// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/desktop/child.h>

#include <chrono>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bt::desktop {

std::string program_dir()
{
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n{GetModuleFileNameW(nullptr, buf, 32768)};
    // UTF-8, as the app's paths are (path::string() would be the ANSI code page's).
    const std::u8string u8{std::filesystem::path(std::wstring(buf, n)).parent_path().u8string()};
    return std::string{u8.begin(), u8.end()};
#else
    std::error_code ec;
    const auto p{std::filesystem::read_symlink("/proc/self/exe", ec)};
    return ec ? std::string{"."} : p.parent_path().string();
#endif
}

Child::~Child() { stop(5000); }

#ifdef _WIN32
//! A path of UTF-8 text (the app's paths are; path(std::string) would read them in the ANSI code page).
static std::filesystem::path utf8_path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

static std::wstring quote(const std::wstring& a)
{
    // CommandLineToArgvW's rules: backslashes before a quote doubled, quotes escaped.
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out{L"\""};
    size_t backslashes{0};
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
            continue;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        if (c != L'\\') out += c;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

bool Child::start(const std::string& program, const std::vector<std::string>& args, const std::string& log_file, std::string& err)
{
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read{nullptr}, in_write{nullptr};
    if (!CreatePipe(&in_read, &in_write, &sa, 0)) {
        err = "cannot make a pipe";
        return false;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    HANDLE log{CreateFileW(utf8_path(log_file).wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    std::wstring cmd{quote(utf8_path(program).wstring())};
    for (const auto& a : args) cmd += L" " + quote(utf8_path(a).wstring());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    // Only these handles go to the node (its stdin pipe and its log), never another inheritable one of the app (a
    // socket, a file): an explicit list instead of every inheritable handle.
    std::vector<HANDLE> inherit{in_read};
    if (log != INVALID_HANDLE_VALUE) inherit.push_back(log);
    SIZE_T attr_size{0};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs{reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data())};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(), inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        err = "cannot prepare the node's handles";
        return false;
    }
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    const BOOL ok{CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                 nullptr, &si.StartupInfo, &pi)};
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(in_read);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        CloseHandle(in_write);
        err = "cannot start " + program;
        return false;
    }
    if (job_) AssignProcessToJobObject(job_, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    stdin_write_ = in_write;
    pid_ = pi.dwProcessId;
    return true;
}

bool Child::running()
{
    if (!process_ || status_) return false;
    DWORD code{0};
    if (GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE) return true;
    status_ = static_cast<int>(code);
    return false;
}

bool Child::wait(int ms)
{
    if (!process_) return true;
    if (WaitForSingleObject(process_, static_cast<DWORD>(ms)) != WAIT_OBJECT_0) return false;
    running();
    return true;
}

void Child::stop(int grace_ms)
{
    if (!process_) return;
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (!wait(grace_ms)) {
        TerminateProcess(process_, 1);
        wait(5000);
    }
    CloseHandle(process_);
    process_ = nullptr;
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
}
#else
bool Child::start(const std::string& program, const std::vector<std::string>& args, const std::string& log_file, std::string& err)
{
    int in[2];
    if (::pipe2(in, O_CLOEXEC) != 0) {
        err = "cannot make a pipe";
        return false;
    }
    const int log{::open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600)};
    std::vector<std::string> argv_s{program};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argv_s) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t parent{::getpid()};
    const pid_t pid{::fork()};
    if (pid < 0) {
        ::close(in[0]);
        ::close(in[1]);
        if (log >= 0) ::close(log);
        err = "cannot start " + program;
        return false;
    }
    if (pid == 0) {
        // The child: its own process group (a Ctrl+C of a terminal is the app's), stopped with the app's thread that
        // made it, its input the pipe, its output the log; every other descriptor is closed on exec.
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent) ::_exit(127);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::dup2(in[0], 0);
        if (log >= 0) {
            ::dup2(log, 1);
            ::dup2(log, 2);
        }
        ::execv(program.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(in[0]);
    if (log >= 0) ::close(log);
    stdin_write_ = in[1];
    pid_ = pid;
    return true;
}

bool Child::running()
{
    if (pid_ <= 0 || status_) return false;
    int st{0};
    const pid_t r{::waitpid(static_cast<pid_t>(pid_), &st, WNOHANG)};
    if (r == 0) return true;
    status_ = r == pid_ ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0)) : -1;
    return false;
}

bool Child::wait(int ms)
{
    const auto end{std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)};
    while (running()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

void Child::stop(int grace_ms)
{
    if (pid_ <= 0) return;
    if (stdin_write_ >= 0) {
        ::close(stdin_write_);
        stdin_write_ = -1;
    }
    if (!wait(grace_ms)) {
        ::kill(static_cast<pid_t>(pid_), SIGTERM);
        if (!wait(30000)) {
            ::kill(static_cast<pid_t>(pid_), SIGKILL);
            wait(5000);
        }
    }
    pid_ = -1;
}
#endif

} // namespace bt::desktop
