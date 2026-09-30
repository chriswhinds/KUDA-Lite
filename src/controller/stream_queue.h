// One host stream on the controller: a FIFO of operations run in order on a dedicated thread.
// Different streams have different threads, so their work overlaps.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace cl {

class StreamQueue {
 public:
  StreamQueue() : thread_([this] { loop(); }) {}

  /// Stops after the operation currently running (if any); queued operations are dropped.
  ~StreamQueue() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  StreamQueue(const StreamQueue&) = delete;
  StreamQueue& operator=(const StreamQueue&) = delete;

  void push(std::function<void()> op) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      ops_.push_back(std::move(op));
    }
    cv_.notify_one();
  }

 private:
  void loop() {
    for (;;) {
      std::function<void()> op;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return stop_ || !ops_.empty(); });
        if (stop_) return;
        op = std::move(ops_.front());
        ops_.pop_front();
      }
      op();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> ops_;
  bool stop_ = false;
  std::thread thread_;  // declared last: starts after the members above are initialised
};

}  // namespace cl
