// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: the part of Bitcoin Core's sync.h the sidechain template's webui host files use, so that
// they compile unchanged outside Core (libs/desktop, doc/app.md "The template is the library").
// Locks are plain std::mutex; Core's lock-order and thread-safety annotations are not checked here
// (they are in the template's own build, where these same files are compiled against Core).
#pragma once

#include <condition_variable>
#include <mutex>
#include <type_traits>

#ifndef GUARDED_BY
#define GUARDED_BY(x)
#define PT_GUARDED_BY(x)
#define EXCLUSIVE_LOCKS_REQUIRED(...)
#define SHARED_LOCKS_REQUIRED(...)
#define LOCKS_EXCLUDED(...)
#define LOCK_RETURNED(x)
#define NO_THREAD_SAFETY_ANALYSIS
#endif

using Mutex = std::mutex;
using GlobalMutex = std::mutex;
using RecursiveMutex = std::recursive_mutex;

#define BT_COMPAT_PASTE2(a, b) a##b
#define BT_COMPAT_PASTE(a, b) BT_COMPAT_PASTE2(a, b)
#define LOCK(cs) std::lock_guard<std::remove_cvref_t<decltype(cs)>> BT_COMPAT_PASTE(bt_compat_lock_, __COUNTER__)(cs)
#define LOCK2(cs1, cs2) std::scoped_lock BT_COMPAT_PASTE(bt_compat_lock_, __COUNTER__)(cs1, cs2)
#define WAIT_LOCK(cs, name) std::unique_lock<std::remove_cvref_t<decltype(cs)>> name(cs)
#define WITH_LOCK(cs, code) ([&]() -> decltype(auto) { LOCK(cs); code; }())
#define AssertLockHeld(cs) ((void)0)
#define AssertLockNotHeld(cs) ((void)0)
