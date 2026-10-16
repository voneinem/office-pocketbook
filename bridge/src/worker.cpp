#include "worker.h"

Worker::Worker() : thread_([this] { run(); }) {}

Worker::~Worker() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();   // the loop runs the remaining jobs first
}

void Worker::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> l(mu_);
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void Worker::wait_idle() {
    std::unique_lock<std::mutex> l(mu_);
    idle_.wait(l, [this] { return queue_.empty() && !running_job_; });
}

void Worker::run() {
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
        wake_.wait(l, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
            if (stop_) return;
            continue;
        }
        std::function<void()> job = std::move(queue_.front());
        queue_.pop_front();
        running_job_ = true;
        l.unlock();
        job();
        l.lock();
        running_job_ = false;
        idle_.notify_all();
    }
}
