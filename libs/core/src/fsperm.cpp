// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/fsperm.h>
#include <bt/log.h>

#include <filesystem>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace bt::fsperm {

#ifdef _WIN32
bool private_dir(const std::string& path, bool)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}
bool private_file(const std::string&, bool) { return true; }
int mode_of(const std::string&) { return -1; }
#else
int mode_of(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return -1;
    return int(st.st_mode & 07777);
}

static void tighten(const std::string& path, mode_t want, bool warn)
{
    int m = mode_of(path);
    if (m < 0 || (m & 077) == 0) return;
    if (warn) LOG_WARN("%s was readable by other users (mode %03o); setting it to %03o", path.c_str(), m, unsigned(want));
    ::chmod(path.c_str(), want);
}

bool private_dir(const std::string& path, bool warn)
{
    if (path.empty()) return false;
    std::filesystem::path p(path);
    // create missing components one by one with 0700
    std::filesystem::path cur;
    for (auto& part : p) {
        cur /= part;
        if (cur.empty() || cur == cur.root_path()) continue;
        if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    tighten(path, 0700, warn);
    return mode_of(path) >= 0;
}

bool private_file(const std::string& path, bool warn)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    ::close(fd);
    tighten(path, 0600, warn);
    return true;
}
#endif

} // namespace bt::fsperm
