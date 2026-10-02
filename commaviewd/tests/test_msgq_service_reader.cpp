// Runs against real msgq (commaviewd's linked copy) on private queues in /dev/shm.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "msgq/ipc.h"
#include "msgq/msgq.h"
#include "msgq_service_reader.h"

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      std::abort();                                                      \
    }                                                                    \
  } while (0)

using commaview::ipc::ServiceReader;

namespace {

constexpr size_t kRingSize = 1024 * 1024;

struct TestQueue {
  std::string endpoint;
  explicit TestQueue(const char* name) {
    static int count = 0;
    endpoint = std::string("commaview_reader_") + name + "_" + std::to_string(getpid()) + "_" +
               std::to_string(count++);
  }
  ~TestQueue() { unlink(("/dev/shm/msgq_" + endpoint).c_str()); }
};

// The queue's shared header, mapped without subscribing.
class Header {
 public:
  explicit Header(const std::string& endpoint) {
    CHECK(msgq_new_queue(&q_, endpoint.c_str(), kRingSize) == 0);
  }
  ~Header() { msgq_close_queue(&q_); }
  uint64_t num_readers() const { return *q_.num_readers; }
  uint64_t uid(size_t slot) const { return *q_.read_uids[slot]; }

 private:
  msgq_queue_t q_{};
};

std::unique_ptr<PubSocket> make_publisher(Context* ctx, const std::string& endpoint) {
  std::unique_ptr<PubSocket> pub(PubSocket::create(ctx, endpoint, true, kRingSize));
  CHECK(pub != nullptr);
  return pub;
}

// Subscribes the way loggerd does on the encoder queues: not conflated.
std::unique_ptr<SubSocket> make_loggerd(Context* ctx, const std::string& endpoint) {
  std::unique_ptr<SubSocket> sock(SubSocket::create(ctx, endpoint, "127.0.0.1", false, true, kRingSize));
  CHECK(sock != nullptr);
  return sock;
}

void publish(PubSocket* pub, std::string text) {
  CHECK(pub->send(text.data(), text.size()) == static_cast<int>(text.size()));
}

std::string text_of(Message& msg) {
  return std::string(msg.getData(), msg.getSize());
}

std::vector<std::string> drain(SubSocket* sock) {
  std::vector<std::string> out;
  while (true) {
    std::unique_ptr<Message> msg(sock->receive(true));
    if (msg == nullptr) break;
    out.push_back(text_of(*msg));
  }
  return out;
}

// Publishes prefix0, prefix1, ... until the session receives one. The first may land before the
// reader thread has started the session, in which case it is (rightly) never delivered.
std::string publish_until_received(ServiceReader& reader, PubSocket* pub, const std::string& prefix,
                                   std::vector<std::string>* published = nullptr) {
  for (int k = 0; k < 200; ++k) {
    const std::string text = prefix + std::to_string(k);
    publish(pub, text);
    if (published != nullptr) published->push_back(text);
    std::unique_ptr<Message> msg = reader.next_message(20);
    if (msg != nullptr) return text_of(*msg);
  }
  CHECK(false);
  return {};
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

// What the bridge used to do on every client connection: create a subscriber, stream, delete it.
// msgq gives nothing back on delete, so the slots run out and the next subscriber evicts loggerd.
void test_per_connection_subscribers_evict_loggerd() {
  TestQueue queue("evict");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  auto loggerd = make_loggerd(ctx.get(), queue.endpoint);
  Header header(queue.endpoint);
  const uint64_t loggerd_uid = header.uid(0);
  CHECK(header.num_readers() == 1);

  for (int connection = 1; connection < NUM_READERS; ++connection) {
    delete SubSocket::create(ctx.get(), queue.endpoint, "127.0.0.1", true, true, kRingSize);
    CHECK(header.num_readers() == static_cast<uint64_t>(connection) + 1);
  }
  CHECK(header.uid(0) == loggerd_uid);

  publish(pub.get(), "frame-a");
  publish(pub.get(), "frame-b");
  delete SubSocket::create(ctx.get(), queue.endpoint, "127.0.0.1", true, true, kRingSize);
  CHECK(header.uid(0) != loggerd_uid);
  // loggerd quietly re-subscribes at the write pointer: the frames it hadn't read are gone.
  CHECK(drain(loggerd.get()).empty());
}

void test_reader_keeps_one_slot_across_client_sessions() {
  TestQueue queue("reuse");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  auto loggerd = make_loggerd(ctx.get(), queue.endpoint);
  Header header(queue.endpoint);
  const uint64_t loggerd_uid = header.uid(0);

  ServiceReader reader(queue.endpoint, kRingSize);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(header.num_readers() == 1);  // no slot until a client connects

  const int sessions = 3 * NUM_READERS;
  std::vector<std::string> published;
  std::vector<std::string> logged;
  for (int session = 0; session < sessions; ++session) {
    CHECK(reader.begin_session());
    const std::string prefix = "s" + std::to_string(session) + "-";
    CHECK(starts_with(publish_until_received(reader, pub.get(), prefix, &published), prefix));
    reader.end_session();
    for (auto& text : drain(loggerd.get())) logged.push_back(text);
  }

  CHECK(header.num_readers() == 2);
  CHECK(header.uid(0) == loggerd_uid);
  CHECK(logged == published);  // never evicted, so loggerd missed nothing
  const auto stats = reader.stats();
  CHECK(stats.socket_creates == 1);
  CHECK(stats.socket_create_failures == 0);
  CHECK(stats.sessions == static_cast<uint64_t>(sessions));
}

void test_new_session_never_sees_older_messages() {
  TestQueue queue("stale");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  ServiceReader reader(queue.endpoint, kRingSize);

  CHECK(reader.begin_session());
  CHECK(starts_with(publish_until_received(reader, pub.get(), "first-"), "first-"));
  reader.end_session();
  // Let the reader finish its last poll and go idle, so these wait in the ring for the drain.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Published while no client is connected.
  publish(pub.get(), "stale-1");
  publish(pub.get(), "stale-2");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  CHECK(reader.begin_session());
  CHECK(reader.next_message(100) == nullptr);
  CHECK(starts_with(publish_until_received(reader, pub.get(), "fresh-"), "fresh-"));
  CHECK(reader.stats().stale_messages_dropped >= 1);
  reader.end_session();
}

void test_unclaimed_message_does_not_carry_over() {
  TestQueue queue("handoff");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  ServiceReader reader(queue.endpoint, kRingSize);

  CHECK(reader.begin_session());
  publish_until_received(reader, pub.get(), "live-");
  publish(pub.get(), "unclaimed");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // read into the hand-off
  reader.end_session();

  CHECK(reader.begin_session());
  CHECK(reader.next_message(100) == nullptr);
  reader.end_session();
}

void test_one_session_at_a_time() {
  TestQueue queue("exclusive");
  ServiceReader reader(queue.endpoint, kRingSize);
  CHECK(reader.next_message(10) == nullptr);
  CHECK(reader.begin_session());
  CHECK(!reader.begin_session());
  reader.end_session();
  CHECK(reader.next_message(10) == nullptr);
  CHECK(reader.begin_session());
  reader.end_session();
}

// msgq signals the thread that subscribed. That must still be a live thread once the client that
// started the first session has gone.
void test_publisher_signals_a_live_thread_after_the_client_leaves() {
  TestQueue queue("tid");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  ServiceReader reader(queue.endpoint, kRingSize);

  std::thread client([&] {
    CHECK(reader.begin_session());
    publish_until_received(reader, pub.get(), "client-");
    reader.end_session();
  });
  client.join();

  Header header(queue.endpoint);
  CHECK(header.num_readers() == 1);
  const uint64_t tid = header.uid(0) & 0xFFFFFFFFu;
  struct stat st {};
  CHECK(stat(("/proc/self/task/" + std::to_string(tid)).c_str(), &st) == 0);
}

// encoderd starts with every drive, and a publisher starting clears every reader slot.
void test_reader_follows_a_publisher_restart() {
  TestQueue queue("restart");
  std::unique_ptr<Context> ctx(Context::create());
  auto pub = make_publisher(ctx.get(), queue.endpoint);
  ServiceReader reader(queue.endpoint, kRingSize);

  CHECK(reader.begin_session());
  publish_until_received(reader, pub.get(), "before-");

  pub.reset();
  pub = make_publisher(ctx.get(), queue.endpoint);
  CHECK(starts_with(publish_until_received(reader, pub.get(), "after-"), "after-"));

  Header header(queue.endpoint);
  CHECK(header.num_readers() == 1);
  CHECK(reader.stats().socket_creates == 1);
  // Destroyed with the session still open: must stop without hanging.
}

}  // namespace

int main() {
  unsetenv("OPENPILOT_PREFIX");
  test_per_connection_subscribers_evict_loggerd();
  test_reader_keeps_one_slot_across_client_sessions();
  test_new_session_never_sees_older_messages();
  test_unclaimed_message_does_not_carry_over();
  test_one_session_at_a_time();
  test_publisher_signals_a_live_thread_after_the_client_leaves();
  test_reader_follows_a_publisher_restart();
  printf("PASS: msgq service reader tests\n");
  return 0;
}
