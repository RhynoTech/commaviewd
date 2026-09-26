#!/usr/bin/env python3
"""Exercise private, bounded source archive reads through the real HTTP server."""

import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def request(url, token=None):
  headers = {"X-CommaView-Token": token} if token else {}
  try:
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=5) as reply:
      return reply.status, reply.read(), dict(reply.headers)
  except urllib.error.HTTPError as failure:
    return failure.code, failure.read(), dict(failure.headers)


def main():
  root = Path(__file__).resolve().parents[2]
  binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN", root / "dist/commaviewd-host"))
  assert binary.is_file(), binary
  with tempfile.TemporaryDirectory(prefix="commaview-archive-api-") as temp:
    temp = Path(temp)
    route = "000004b4--75e1f0ba8f"
    segment = temp / "realdata" / f"{route}--0"
    segment.mkdir(parents=True)
    (segment / "fcamera.hevc").write_bytes(b"0123456789")
    recipes = temp / "recipes"
    recipes.mkdir()
    (recipes / "ui-source-1.jsonl").write_text(
        f'{{"routeId":"{route}","sequence":1}}\n', encoding="ascii")
    with socket.socket() as probe:
      probe.bind(("127.0.0.1", 0))
      port = probe.getsockname()[1]
    env = os.environ.copy()
    env.update(COMMAVIEWD_API_TOKEN="source-test-token",
               COMMAVIEWD_SOURCE_ARCHIVE_ROOT=str(temp / "realdata"),
               COMMAVIEWD_RECIPE_DIR=str(recipes))
    process = subprocess.Popen([str(binary), "control", "--port", str(port)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
      base = f"http://127.0.0.1:{port}"
      for _ in range(50):
        try:
          if request(base + "/commaview/version")[0] == 200:
            break
        except (OSError, TimeoutError):
          time.sleep(0.1)
      else:
        raise AssertionError("control server did not start")
      media = (base + "/commaview/source-recording/range?route=" + route +
               "&segment=0&kind=road&offset=2&length=4")
      assert request(media)[0] == 401
      assert request(media, "wrong-token")[0] == 401
      status, body, headers = request(media, "source-test-token")
      assert (status, body, headers["X-Source-Size"]) == (200, b"2345", "10")
      status, body, _ = request(base + "/commaview/source-recording/recipe?route=" + route,
                                "source-test-token")
      assert status == 200 and b'"sequence":1' in body
      assert request(media.replace("length=4", "length=262145"), "source-test-token")[0] == 400
      print("PASS: authenticated source archive HTTP range and recipe")
    finally:
      process.terminate()
      process.wait(timeout=5)


if __name__ == "__main__":
  main()
