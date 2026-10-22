// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: the helpers of Bitcoin Core's util/fs_helpers.h that the template's webui host files use.
#pragma once

#include <util/fs.h>

/** Rename src to dest, replacing dest (atomically where the system can). */
bool RenameOver(fs::path src, fs::path dest);
/** Create the directory (and its parents); true when it was made. Throws fs::filesystem_error. */
bool TryCreateDirectories(const fs::path& p);
#ifdef _WIN32
/** A special folder of Windows (CSIDL_APPDATA, CSIDL_LOCAL_APPDATA...). */
fs::path GetSpecialFolderPath(int nFolder, bool fCreate = true);
#endif
