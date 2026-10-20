// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Owner-only permissions for data that must stay private (wallet seeds, stores, logs).
// POSIX: directories 0700, files 0600; looser existing modes are tightened with a warning.
// Windows: no-op, the files live under the per-user profile (%APPDATA%) whose ACLs grant
// access to that user (and administrators) only.
#pragma once

#include <string>

namespace bt::fsperm {

/** create_directories(path) and make every created or existing directory on it owner-only
 *  (only the leaf is tightened if it already existed). Returns false on error. */
bool private_dir(const std::string& path, bool warn = true);
/** Create path as an empty 0600 file if missing; tighten an existing file to 0600. */
bool private_file(const std::string& path, bool warn = true);
/** Mode bits of path (POSIX), or -1. For tests. */
int mode_of(const std::string& path);

} // namespace bt::fsperm
