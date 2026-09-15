# Donating a drive to CommaView

Thanks for helping. This page is for you, the person donating — not for developers.

CommaView needs real drive data to check that its overlays line up correctly across
different openpilot and sunnypilot builds. What it does **not** need is anything about
you, your car's identity, or where you drove.

So there is a tool that strips a drive down to just the parts CommaView reads, and
shows you a receipt of exactly what's left before you send anything.

## What you need

A comma device with openpilot or sunnypilot on it. Nothing to install, no comma
account, no login, nothing uploaded to comma's servers. The tool runs on your device
and writes a folder. You decide whether to send it.

## Step 1 — find a drive

On the device, your drives live in `/data/media/0/realdata`. Each drive is split into
one-minute segments that look like `000001ad--4b4217fb46--117`.

```bash
ls /data/media/0/realdata | tail -20
```

## Step 2 — run the tool

Copy `sanitize_route_for_contribution.py` onto the device, then:

```bash
python3 sanitize_route_for_contribution.py \
  /data/media/0/realdata \
  --route-name 000001ad--4b4217fb46 \
  --segments 118-121 \
  -o /data/media/0 \
  --tar
```

That's it. You'll get a folder (and a `.tar.gz`) plus a receipt printed to the screen.

If you'd rather do it on a computer that has an openpilot checkout, it works there too:

```bash
OPENPILOT_ROOT=/path/to/openpilot python3 sanitize_route_for_contribution.py /path/to/route -o .
```

**Use the openpilot/sunnypilot checkout that matches your build.** Reading a sunnypilot
drive with a plain openpilot checkout will silently throw away the sunnypilot-specific
data. On the device this is automatic.

## Step 3 — read the receipt

The tool prints every service it kept, every service it threw away, how many messages
each had, and the total size. Read it. If something in the "KEPT" list bothers you,
don't send the bundle.

## Step 4 — send it

Send the `.tar.gz`. Done.

---

## The options you might want

| Flag | What it does |
| --- | --- |
| `--segments 118-121` | Pick specific minutes of the drive. Default is the whole thing. |
| `--include-video road,wide,qcamera` | Add camera video. **Off by default.** See the warning below. |
| `--include-driver-monitoring` | Add the driver-attention data. **Off by default.** |
| `--allow-endpoints` | Allow the first and last minute of the drive. **Blocked by default.** |
| `--tar` | Also produce a `.tar.gz` to send. |

### Why the first and last segments are blocked

A drive usually starts where you live and ends where you were going. Those two minutes
are the ones that identify you, even without GPS, because the car's motion is enough to
match a driveway. The tool refuses them unless you pass `--allow-endpoints`. Only do
that if you know the start and end of that particular drive are somewhere you don't mind
sharing.

### Why driver monitoring is off by default

`driverStateV2` and `driverMonitoringState` are derived from the camera pointed at your
face — head position, eye state, whether you looked away. That's data about you, not
about the car. It's excluded unless you explicitly turn it on.

The interior camera **video** (`dcamera.hevc`) is never included. There's no flag for it.
It is not copied under any circumstances.

---

## Honest statement of what is and isn't removed

**Removed.** The tool works from a whitelist: it keeps a short, fixed list of data
CommaView actually reads, and deletes everything else rather than trying to scrub it.
That means GPS and all location messages, the navigation destination and route, the
Kalman position filter, raw GNSS, all log and crash text, the device's startup record,
the driver-facing camera and everything derived from it, and your device's dongle ID
are not written into the bundle at all. Your car's VIN is kept but the last six
characters — the serial number that identifies your individual car — are replaced with
`X`. The bundle records which openpilot/sunnypilot build the drive came from (remote,
branch, commit, version, device model) because that's the whole point, but it never
records your dongle ID, device serial, or any account identifier. Because new data
types are excluded by default rather than included, a future openpilot release adding a
new message can't quietly leak through.

**Not removed.** The driving data itself is real and it is yours: steering, speed,
acceleration, the model's predicted path, radar targets, and the car's calibration.
That is a detailed record of how a specific car was driven for those minutes. And if
you choose `--include-video`, understand this clearly: **the road and wide camera video
shows exactly where you drove — the streets, the buildings, the license plates of cars
around you, possibly your own street. That cannot be anonymized, and this tool does not
try.** If you include video, you are sharing your location, whatever the rest of the
bundle says. Only include video for a stretch of road you're comfortable showing to
someone.

If you're unsure, send it without video first. That's the default for a reason.
