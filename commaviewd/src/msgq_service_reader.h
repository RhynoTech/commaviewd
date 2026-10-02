#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "msgq/ipc.h"

namespace commaview::ipc {

struct ServiceReaderStats {
  // Subscribers created, each taking one reader slot in the queue's shared header. Stays at 1 for
  // the life of the process once a client has connected.
  uint64_t socket_creates = 0;
  uint64_t socket_create_failures = 0;
  uint64_t sessions = 0;
  // Messages published before a session began, read off and discarded when it began.
  uint64_t stale_messages_dropped = 0;
  // Messages replaced by a newer one before the session took them (latest wins, as with conflate).
  uint64_t superseded_messages = 0;
};

// One conflated msgq subscriber on one of openpilot's services, kept for the life of the process
// and lent to one client session at a time.
//
// msgq gives every subscriber a reader slot in the queue's shared header and never gives it back:
// deleting a SubSocket only unmaps the ring, and a dead process's slots stay taken as well. A queue
// has NUM_READERS (15) slots, and the subscriber that finds them all taken evicts every reader on
// the queue, openpilot's own included (loggerd on the encoder queues), which then lose whatever
// they had not read yet. A subscriber per client connection used up the slots after about 14
// reconnects in one drive. This takes one slot, when the first session begins, and keeps it.
//
// The subscriber is created and polled on this reader's own thread, which lives as long as the
// reader. msgq stores the subscribing thread's id with the slot and the publisher signals that
// thread on every message to end its poll early. A subscriber made on a client's thread would leave
// the publisher signalling a thread that has exited (and whose id the kernel may hand to another
// process), while the next client's polls waited out their full timeout.
class ServiceReader {
 public:
  ServiceReader(std::string service, size_t segment_size);
  // Stops and joins the reader thread. The bridge never destroys its readers.
  ~ServiceReader();
  ServiceReader(const ServiceReader&) = delete;
  ServiceReader& operator=(const ServiceReader&) = delete;

  // Starts a client session, or returns false while another is active. A session only ever sees
  // messages published after it began.
  bool begin_session();
  void end_session();

  // The newest message for the current session, waiting up to timeout_ms. Null on timeout or when
  // no session is active.
  std::unique_ptr<Message> next_message(int timeout_ms);

  ServiceReaderStats stats() const;
  const std::string& service() const { return service_; }

 private:
  void run();

  const std::string service_;
  const size_t segment_size_;

  mutable std::mutex mutex_;
  std::condition_variable state_cv_;
  std::condition_variable message_cv_;
  bool stopping_ = false;
  bool session_active_ = false;
  uint64_t generation_ = 0;
  std::unique_ptr<Message> pending_;
  ServiceReaderStats stats_;
  std::thread thread_;
};

}  // namespace commaview::ipc
