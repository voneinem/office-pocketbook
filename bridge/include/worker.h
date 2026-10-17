#pragma once
// The one worker thread of the app (contracts/bridge-qml.md): sync, connection tests and other long operations run
// here, never on the UI thread. Jobs run in the order they were posted. A job that wants to tell the UI something
// hops back with QMetaObject::invokeMethod(object, functor, Qt::QueuedConnection).
//
// Plain C++ (no Qt event loop): it keeps working while the application object is being torn down.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class Worker {
public:
    Worker();
    ~Worker();   // finishes the queued jobs, then stops the thread
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> job);
    // Blocks the calling thread until every job posted so far has run (used by destructors and tests).
    void wait_idle();

private:
    void run();

    std::mutex mu_;
    std::condition_variable wake_, idle_;
    std::deque<std::function<void()>> queue_;
    bool running_job_ = false;
    bool stop_ = false;
    std::thread thread_;
};
