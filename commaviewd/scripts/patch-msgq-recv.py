#!/usr/bin/env python3
"""Build a private msgq consumer with bounded recovery from overwritten headers.

The producer and shared-memory format remain upstream-owned. Only commaviewd's
linked copy of msgq_msg_recv is changed; sunnypilot is never patched.
"""

import pathlib
import sys


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise SystemExit("unsupported upstream msgq receive layout")
    return source.replace(old, new, 1)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch-msgq-recv.py upstream.cc output.cc")
    source = pathlib.Path(sys.argv[1]).read_text()
    source = replace_once(
        source,
        "  char * p = q->data + read_pointer;\n\n  // Check if new message is available",
        """  // A racing producer can invalidate a reader while it samples the ring.
  // Never dereference an out-of-range pointer in this client process.
  if (read_pointer > q->size - sizeof(int64_t) || write_pointer >= q->size) {
    msgq_reset_reader(q);
    msg->size = 0;
    return 0;
  }
  char * p = q->data + read_pointer;

  // Check if new message is available""",
    )
    source = replace_once(
        source,
        """  // crashing is better than passing garbage data to the consumer
  // the size will have weird value if it was overwritten by data accidentally
  assert((uint64_t)size < q->size);
  assert(size > 0);""",
        """  // Discard an overwritten/invalid header without ever delivering garbage.
  // An assertion here killed the entire bridge during tablet reconnects.
  if (size <= 0 || static_cast<uint64_t>(size) >= q->size ||
      static_cast<uint64_t>(size) > q->size - read_pointer - sizeof(int64_t)) {
    static thread_local unsigned int invalid_headers = 0;
    if (++invalid_headers <= 5 || invalid_headers % 1000 == 0) {
      fprintf(stderr, "[commaviewd] msgq invalid header endpoint=%s size=%lld ring=%zu read=%u write=%u count=%u\\n",
              q->endpoint.c_str(), static_cast<long long>(size), q->size,
              read_pointer, write_pointer, invalid_headers);
    }
    msgq_reset_reader(q);
    msg->size = 0;
    return 0;
  }""",
    )
    pathlib.Path(sys.argv[2]).write_text(source)


if __name__ == "__main__":
    main()
