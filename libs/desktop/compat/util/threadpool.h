// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: the ThreadPool of Bitcoin Core (util/threadpool.h) as the template's bridge uses it: Start, Submit
// (false when the pool takes no more work), Stop (waits for the queued and running tasks).
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <util/threadnames.h>

class ThreadPool
{
public:
    explicit ThreadPool(std::string name) : m_name(std::move(name)) {}
    ~ThreadPool() { Stop(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void Start(int workers)
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_stopping = false;
        for (int i{0}; i < workers; ++i) {
            m_workers.emplace_back([this, i] {
                util::ThreadRename(m_name + "." + std::to_string(i));
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                        if (m_queue.empty()) return;
                        task = std::move(m_queue.front());
                        m_queue.pop_front();
                    }
                    task();
                }
            });
        }
    }

    template <typename F>
    [[nodiscard]] bool Submit(F&& fn)
    {
        {
            std::lock_guard<std::mutex> l(m_mutex);
            if (m_stopping || m_workers.empty()) return false;
            m_queue.emplace_back(std::forward<F>(fn));
        }
        m_cv.notify_one();
        return true;
    }

    void Stop()
    {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_stopping = true;
            workers.swap(m_workers);
        }
        m_cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

private:
    std::string m_name;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_workers;
    bool m_stopping{false};
};
