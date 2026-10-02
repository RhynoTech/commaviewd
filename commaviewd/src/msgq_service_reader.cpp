#include "msgq_service_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace commaview::ipc {
namespace {

constexpr int kPollMs = 20;
// A conflated subscriber is at most one message behind after a receive; the bound only stops a
// publisher that outruns the drain from holding the reader thread.
constexpr int kMaxStaleDrain = 64;

}  // namespace

ServiceReader::ServiceReader(std::string service, size_t segment_size)
    : service_(std::move(service)), segment_size_(segment_size) {
  thread_ = std::thread([this] { run(); });
}

ServiceReader::~ServiceReader() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    session_active_ = false;
    pending_.reset();
  }
  state_cv_.notify_all();
  message_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool ServiceReader::begin_session() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || session_active_) return false;
    session_active_ = true;
    generation_ += 1;
    pending_.reset();
    stats_.sessions += 1;
  }
  state_cv_.notify_all();
  return true;
}

void ServiceReader::end_session() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    session_active_ = false;
    pending_.reset();
  }
  message_cv_.notify_all();
}

std::unique_ptr<Message> ServiceReader::next_message(int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  message_cv_.wait_for(lock, std::chrono::milliseconds(std::max(timeout_ms, 0)), [&] {
    return pending_ != nullptr || !session_active_;
  });
  if (!session_active_) return nullptr;
  return std::move(pending_);
}

ServiceReaderStats ServiceReader::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void ServiceReader::run() {
  std::unique_ptr<Context> context;
  std::unique_ptr<SubSocket> socket;
  std::unique_ptr<Poller> poller;
  uint64_t served_generation = 0;
  uint64_t create_attempt_generation = 0;

  while (true) {
    uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Idle between sessions: the slot stays registered but nothing is read, so a publisher
      // that laps this reader just marks it invalid, and the next receive restarts at the newest.
      state_cv_.wait(lock, [&] { return stopping_ || session_active_; });
      if (stopping_) break;
      generation = generation_;
    }

    if (socket == nullptr) {
      if (create_attempt_generation == generation) {
        // Connecting failed for this session. A failed connect takes no slot, but like the
        // per-connection code this tries once per session rather than spinning.
        std::unique_lock<std::mutex> lock(mutex_);
        state_cv_.wait(lock, [&] { return stopping_ || generation_ != generation; });
        continue;
      }
      create_attempt_generation = generation;
      if (context == nullptr) context.reset(Context::create());
      // conflate=true: a receive returns the newest message and skips the rest.
      socket.reset(SubSocket::create(context.get(), service_, "127.0.0.1", true, true, segment_size_));
      uint64_t creates = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (socket == nullptr) {
          stats_.socket_create_failures += 1;
          continue;
        }
        stats_.socket_creates += 1;
        creates = stats_.socket_creates;
      }
      poller.reset(Poller::create());
      poller->registerSocket(socket.get());
      printf("[%s] msgq subscriber created; kept for the life of the process (creates=%llu)\n",
             service_.c_str(),
             static_cast<unsigned long long>(creates));
      fflush(stdout);
    }

    if (served_generation != generation) {
      // Start the session where a brand-new subscriber would: at the write pointer. Whatever was
      // published while no one was watching, or during an earlier session, is discarded.
      uint64_t dropped = 0;
      for (int i = 0; i < kMaxStaleDrain; ++i) {
        std::unique_ptr<Message> stale(socket->receive(true));
        if (stale == nullptr) break;
        dropped += 1;
      }
      served_generation = generation;
      std::lock_guard<std::mutex> lock(mutex_);
      stats_.stale_messages_dropped += dropped;
    }

    for (SubSocket* ready : poller->poll(kPollMs)) {
      std::unique_ptr<Message> message(ready->receive(true));
      if (message == nullptr) continue;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        // The session ended or was replaced while this was being read: it isn't the new one's.
        if (!session_active_ || generation_ != served_generation) continue;
        if (pending_ != nullptr) stats_.superseded_messages += 1;
        pending_ = std::move(message);
      }
      message_cv_.notify_all();
    }
  }
}

}  // namespace commaview::ipc
