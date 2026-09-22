# comma4 desk-bench validation: synchronous versus isolated UI exporter

**Date:** 2026-09-21

**Device:** comma4/MICI on a desk, WiFi + SSH only, no car or panda

**Purpose:** compare the same replay with no CommaView exporter, the old synchronous exporter, and the new isolated-worker exporter; then abuse build C without letting socket or network trouble stall the comma UI.

## What this can and cannot prove

This bench can prove whether CommaView socket/network failures visibly stall the UI process and can compare live `uiDebug` frame timings. It cannot prove that a real car will not disengage: replay publishes recorded `onroadEvents` and `managerState` messages and does not run the complete live driving stack. Treat new warnings relative to build A as suspicious, but reserve the final disengagement claim for a later in-car test.

The three builds intentionally keep the installed `commaviewd` binary, openpilot checkout, route, replay speed, and test duration fixed. Only the UI exporter changes:

- **A — baseline:** CommaView stopped; exporter removed from openpilot.
- **B — old:** commit `211269e`; JSON encoding and socket `sendall()` run synchronously in the UI thread.
- **C — new:** commit `1cc9a7e`; latest-value queue and socket work run in an isolated worker.

Do the builds in A → B → C order. Do not update openpilot, sunnypilot, AGNOS, or CommaView during the run.

## 1. Prerequisites

### On the comma4 screen

1. Plug the comma4 into stable desk power.
2. Join the same WiFi network used by the SSH client.
3. Open **Settings → Developer**, enable **Developer Mode**, then enable **SSH**. Menu wording can vary slightly by upstream build.
4. If the device asks for an SSH key/GitHub username, register the public key used by the `comma4-lan` alias through the normal comma connect SSH-key flow.
5. Confirm the device says **offroad**. Never run the switch helper while `IsOnroad=1`.

### On the gateway/workstation

Run these from `/home/rhyno/Development/commaviewd`:

```bash
cd /home/rhyno/Development/commaviewd
ssh -G comma4-lan | awk '/^(hostname|user|identityfile|identitiesonly) /{print}'
ssh comma4-lan 'printf "ssh ok: "; hostname; printf "IsOnroad="; tr -d "\000\r\n" < /data/params/d/IsOnroad 2>/dev/null || true; echo'
```

Expected: user `comma`, the dedicated comma4 key, successful hostname output, and `IsOnroad` not equal to `1`. If SSH fails, fix WiFi/IP/alias routing before continuing.

Install the three bench helpers onto the device from this checkout:

```bash
ssh comma4-lan 'mkdir -p /data/commaview-bench/bin /data/commaview-bench/results'
scp tools/bench/comma4-bench-switch.sh \
    tools/bench/comma4-bench-metrics.py \
    tools/bench/comma4-bench-snapshot.sh \
    comma4-lan:/data/commaview-bench/bin/
ssh comma4-lan 'chmod 755 /data/commaview-bench/bin/*'
```

Create or refresh the public source checkout used only to select B and C:

```bash
ssh comma4-lan '
  if [ -d /data/commaview-bench-src/.git ]; then
    git -C /data/commaview-bench-src fetch --prune origin
  else
    git clone https://github.com/RhynoTech/commaviewd.git /data/commaview-bench-src
  fi
  git -C /data/commaview-bench-src cat-file -e 211269e^{commit}
  git -C /data/commaview-bench-src cat-file -e 1cc9a7e^{commit}
'
```

### Pick one route and cache it before testing

The public openpilot demo route requires no comma account:

```text
5beb9b58bd12b691/0000010a--a51155e496
```

To use one of Rhyno's own routes instead, copy its route name from connect.comma.ai and authenticate once on the device:

```bash
ssh -t comma4-lan
cd /data/openpilot
if [ -d openpilot/tools ]; then OP_PREFIX=openpilot/; else OP_PREFIX=; fi
python3 "/data/openpilot/${OP_PREFIX}tools/lib/auth.py"
```

For every remaining on-device command, open one SSH shell and define the route/tool paths. Replace `ROUTE` only if using a private route:

```bash
ssh -t comma4-lan
export ROUTE='5beb9b58bd12b691/0000010a--a51155e496'
export OP_ROOT=/data/openpilot
if [ -d "$OP_ROOT/openpilot/tools" ]; then export OP_PREFIX=openpilot/; else export OP_PREFIX=; fi
export REPLAY="$OP_ROOT/${OP_PREFIX}tools/replay/replay"
export UI="$OP_ROOT/${OP_PREFIX}selfdrive/ui/ui.py"
export BENCH=/data/commaview-bench
```

Build replay only if the binary is missing:

```bash
if [ ! -x "$REPLAY" ]; then
  cd "$OP_ROOT"
  scons -u -j4 "${OP_PREFIX}tools/replay/replay"
fi
"$REPLAY" --help | head
```

Stop the stock manager/UI so it cannot compete with the replay UI. This lasts until reboot:

```bash
tmux send-keys -t comma C-c 2>/dev/null || pkill -INT -f '[s]elfdrive.manager.manager' || true
sleep 8
pkill -INT -f '[s]elfdrive.ui.ui|[s]elfdrive/ui/ui.py' 2>/dev/null || true
pgrep -af '[s]elfdrive.manager.manager|[s]elfdrive/ui/ui.py' || echo 'stock manager/UI stopped'
```

Pre-cache the selected route, including the road video. This matters because the WiFi fault must not turn into a route-download test:

```bash
tmux kill-session -t cv-cache 2>/dev/null || true
tmux new-session -d -s cv-cache \
  "cd '$OP_ROOT' && '$REPLAY' '$ROUTE' --no-loop -x 3 2>&1 | tee '$BENCH/results/cache-replay.log'"
tmux attach -t cv-cache
```

Let it reach the end. Detach with `Ctrl-b`, then `d`. Confirm it exited without a download error:

```bash
tail -n 30 "$BENCH/results/cache-replay.log"
```

## 2. Standard A/B/C comparison

Use the same three-minute window for every build. Start each run near the beginning of the route (`--start 0` is explicit), do not touch the screen during the metrics window, and take one physical photo of the comma screen around the 60-second mark.

### Reusable run block

After switching a build, set its label and run this whole block:

```bash
# Set exactly one label before each run: A-baseline, B-old, or C-new
: "${LABEL:?set LABEL first}"

tmux kill-session -t cv-replay 2>/dev/null || true
tmux kill-session -t cv-ui 2>/dev/null || true
tmux kill-session -t cv-metrics 2>/dev/null || true

tmux new-session -d -s cv-replay \
  "cd '$OP_ROOT' && '$REPLAY' '$ROUTE' --start 0 2>&1 | tee '$BENCH/results/${LABEL}-replay.log'"
sleep 5
tmux new-session -d -s cv-ui \
  "cd '$OP_ROOT' && PYTHONPATH='$OP_ROOT' python3 '$UI' 2>&1 | tee '$BENCH/results/${LABEL}-ui.log'"
sleep 20
tmux new-session -d -s cv-metrics \
  "PYTHONPATH='$OP_ROOT' python3 '$BENCH/bin/comma4-bench-metrics.py' --duration 180 --label '$LABEL'"

while tmux has-session -t cv-metrics 2>/dev/null; do sleep 5; done
cat "$BENCH/results/${LABEL}-ui-summary.json"
"$BENCH/bin/comma4-bench-snapshot.sh" "$LABEL"
```

If `uiSamples` is zero, that run is invalid. Check `tmux capture-pane -p -S -200 -t cv-ui` and repeat it.

### A — baseline, CommaView absent

```bash
"$BENCH/bin/comma4-bench-switch.sh" baseline
export LABEL=A-baseline
```

Run the reusable block. Confirm the UI looks smooth and record whether its alert banner changes. This is the route's control behavior.

### B — old synchronous exporter

```bash
"$BENCH/bin/comma4-bench-switch.sh" old
export LABEL=B-old
```

Expected identity output includes `sourceRef=211269e...` and `workerMarker=absent`. Run the reusable block. When complete, stop its replay/UI before switching:

```bash
tmux kill-session -t cv-replay 2>/dev/null || true
tmux kill-session -t cv-ui 2>/dev/null || true
```

### C — new isolated-worker exporter

```bash
"$BENCH/bin/comma4-bench-switch.sh" new
export LABEL=C-new
```

Expected identity output includes `sourceRef=1cc9a7e...` and `workerMarker=present`. Run the reusable block. Leave the C replay and UI sessions running for fault injection.

## 3. Fault injection on build C

For every fault, start a separate metrics window, wait 15 seconds for a clean before-period, inject the fault, and take a physical photo during the fault. Compare the screen and summary with `A-baseline`; recorded route warnings common to A are not regressions.

Helper to start a 90-second fault window:

```bash
start_fault_metrics() {
  fault_label="$1"
  tmux kill-session -t cv-metrics 2>/dev/null || true
  tmux new-session -d -s cv-metrics \
    "PYTHONPATH='$OP_ROOT' python3 '$BENCH/bin/comma4-bench-metrics.py' --duration 90 --label '$fault_label'"
}
```

### C1 — stop reading the Unix socket without closing it

This is the most important test. `SIGSTOP` freezes the bridge process, including its Unix-socket reader, while leaving the socket connection open. Build B can be tested the same way later for contrast.

```bash
start_fault_metrics C-stalled-socket
sleep 15
bridge_pid="$(cat /data/commaview/run/bridge.pid)"
kill -0 "$bridge_pid"
tmux new-session -d -s cv-stall \
  "kill -STOP '$bridge_pid'; sleep 45; kill -CONT '$bridge_pid'"
while tmux has-session -t cv-metrics 2>/dev/null; do sleep 5; done
kill -CONT "$bridge_pid" 2>/dev/null || true
cat "$BENCH/results/C-stalled-socket-ui-summary.json"
"$BENCH/bin/comma4-bench-snapshot.sh" C-stalled-socket
```

Watch for: UI animation/FPS stays normal; replay clock/video does not visibly pause; no new persistent alert compared with A. The bridge's phone/video stream is expected to freeze while stopped.

Optional B contrast: stop replay/UI, run `comma4-bench-switch.sh old`, start the standard run block long enough to restore replay/UI, then repeat this fault as `B-stalled-socket`. Restore C afterward before doing C2–C4.

### C2 — hard-kill and restart commaviewd

```bash
start_fault_metrics C-runtime-restart
sleep 15
bridge_pid="$(cat /data/commaview/run/bridge.pid)"
control_pid="$(cat /data/commaview/run/control.pid)"
kill -KILL "$bridge_pid" "$control_pid"
sleep 20
COMMAVIEWD_RESTART_REASON=comma4-bench-hard-restart bash /data/commaview/start.sh
while tmux has-session -t cv-metrics 2>/dev/null; do sleep 5; done
cat "$BENCH/results/C-runtime-restart-ui-summary.json"
"$BENCH/bin/comma4-bench-snapshot.sh" C-runtime-restart
```

Watch for: UI remains smooth while the socket disappears; commaviewd comes back; CommaView can reconnect; replay/UI processes never restart or pause.

### C3 — disconnect only the phone/tablet app

1. Open CommaView and connect it to the comma4.
2. Confirm a connection exists on ports 8200–8203:

   ```bash
   ss -tnp | grep -E ':(8200|8201|8202|8203)[[:space:]]' || echo 'no app stream connected'
   ```

3. Start metrics, wait 15 seconds, then force-stop the app or disable WiFi on the phone/tablet for 30 seconds. Do **not** disable comma4 WiFi in this step.

   ```bash
   start_fault_metrics C-app-disconnect
   ```

4. Reopen/reconnect the app before the window ends, then collect:

   ```bash
   while tmux has-session -t cv-metrics 2>/dev/null; do sleep 5; done
   cat "$BENCH/results/C-app-disconnect-ui-summary.json"
   "$BENCH/bin/comma4-bench-snapshot.sh" C-app-disconnect
   ```

Watch for: comma UI and replay stay smooth; app disconnect/reconnect is visible only in commaviewd logs; no new persistent warning appears.

### C4 — turn comma4 WiFi off mid-replay

This intentionally drops SSH. The route must already be cached and all work must be in tmux. First verify NetworkManager is available:

```bash
command -v nmcli
```

Then schedule WiFi off for 30 seconds with automatic recovery:

```bash
start_fault_metrics C-wifi-loss
tmux new-session -d -s cv-wifi-fault \
  "sleep 15; { nmcli radio wifi off || sudo -n nmcli radio wifi off; }; sleep 30; { nmcli radio wifi on || sudo -n nmcli radio wifi on; } >'$BENCH/results/C-wifi-fault.log' 2>&1"
```

SSH should disconnect. Watch the physical comma screen during the outage. Wait at least one minute, reconnect through `comma4-lan` (update its IP only if DHCP changed), redefine the variables from section 1, then collect:

```bash
cat "$BENCH/results/C-wifi-loss-ui-summary.json"
"$BENCH/bin/comma4-bench-snapshot.sh" C-wifi-loss
```

Watch for: cached replay and UI continue; loss of external networking does not freeze UI; after WiFi returns, SSH and the app can reconnect. If `nmcli` is missing, use the comma screen's WiFi toggle and turn it back on after 30 seconds.

## 4. Plain-language pass/fail checklist

Tick these without interpreting raw logs:

### Setup and comparison

- [ ] The same route ID was used for A, B, and C.
- [ ] A said exporter absent and CommaView stopped.
- [ ] B said `workerMarker=absent`.
- [ ] C said `workerMarker=present`.
- [ ] All three summary files have more than zero `uiSamples`.
- [ ] Build C looked at least as smooth as baseline A.
- [ ] Build C did not introduce an alert that was absent in A.

### Build C faults

- [ ] UI stayed smooth while the Unix socket reader was stalled: **yes / no**.
- [ ] Replay video/clock kept moving while the socket reader was stalled: **yes / no**.
- [ ] UI stayed smooth while commaviewd was killed and restarted: **yes / no**.
- [ ] commaviewd restarted and the app could reconnect: **yes / no**.
- [ ] UI stayed smooth when the app was disconnected: **yes / no**.
- [ ] UI stayed smooth while comma4 WiFi was off: **yes / no**.
- [ ] Replay continued from cache while WiFi was off: **yes / no**.
- [ ] No new persistent `commIssue`/process warning appeared beyond build A: **yes / no**.
- [ ] Every fault has a screen photo and a `.tar.gz` evidence bundle: **yes / no**.

**Bench pass:** every build identity check is correct, every metrics file contains samples, and every C fault answer above is **yes**. Any visible UI freeze, replay pause, zero-sample window, wrong build identity, or new persistent warning is a fail/retest—not something to explain away.

## 5. Cleanup and what to send back

Restore C, stop the manual replay/UI, and reboot to restore the normal manager:

```bash
"$BENCH/bin/comma4-bench-switch.sh" new
tmux kill-session -t cv-replay 2>/dev/null || true
tmux kill-session -t cv-ui 2>/dev/null || true
tmux kill-session -t cv-metrics 2>/dev/null || true
sudo -n reboot || reboot
```

After the device reconnects, copy all evidence to the gateway/workstation:

```bash
mkdir -p /home/rhyno/Development/commaviewd/device-results/2026-09-21-comma4-bench
scp -r comma4-lan:/data/commaview-bench/results/. \
  /home/rhyno/Development/commaviewd/device-results/2026-09-21-comma4-bench/
```

Do not commit `device-results/`. Send back:

1. The completed checklist above.
2. Screen photos for A, B, C and each C fault, labeled with the matching run label.
3. These summaries: `A-baseline`, `B-old`, `C-new`, `C-stalled-socket`, `C-runtime-restart`, `C-app-disconnect`, and `C-wifi-loss` `*-ui-summary.json` files.
4. Every matching evidence `.tar.gz` plus `.sha256`.
5. The three identity files (`baseline-identity.txt`, `old-identity.txt`, and `new-identity.txt`) and the exact route ID.
6. A one-line note for anything unexpected: what was visible, which label, and roughly when in the 90/180-second window it happened.

The final report should quote p50/p95/p99/max frame time, median/minimum FPS, frames over 50 ms, visible result, and evidence bundle checksum for each run. Do not claim real-drive disengagement safety from this desk bench.
