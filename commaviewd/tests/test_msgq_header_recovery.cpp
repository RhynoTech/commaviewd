#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>

#include "msgq/msgq.h"

int main() {
  const std::string endpoint = "commaview_msgq_recovery_" + std::to_string(getpid());
  const std::string shm_path = "/dev/shm/msgq_" + endpoint;
  constexpr size_t ring_size = 1024 * 1024;
  msgq_queue_t publisher{}, subscriber{};
  assert(msgq_new_queue(&publisher, endpoint.c_str(), ring_size) == 0);
  msgq_init_publisher(&publisher);
  assert(msgq_new_queue(&subscriber, endpoint.c_str(), ring_size) == 0);
  msgq_init_subscriber(&subscriber);

  // A stale/overwritten header previously asserted and killed the bridge.
  *publisher.write_pointer = 8;
  *subscriber.read_pointers[subscriber.reader_id] = 0;
  *subscriber.read_valids[subscriber.reader_id] = true;
  *reinterpret_cast<int64_t*>(subscriber.data) = ring_size + 1;
  msgq_msg_t received{};
  assert(msgq_msg_recv(&received, &subscriber) == 0);
  assert(received.size == 0);
  assert(*subscriber.read_pointers[subscriber.reader_id] == *publisher.write_pointer);

  // Recovery must not poison the next real frame.
  char payload[] = {'o', 'k'};
  msgq_msg_t outgoing{sizeof(payload), payload};
  assert(msgq_msg_send(&outgoing, &publisher) == sizeof(payload));
  assert(msgq_msg_recv(&received, &subscriber) == sizeof(payload));
  assert(std::memcmp(received.data, payload, sizeof(payload)) == 0);
  msgq_msg_close(&received);
  msgq_close_queue(&subscriber);
  msgq_close_queue(&publisher);
  unlink(shm_path.c_str());
}
