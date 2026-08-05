#!/usr/bin/env python3
"""
Live bar graph of the Every Watch's BLE channel-energy survey.

Reads raw RF dBm-per-channel readings over the RTT bridge started by
rtt.sh (see README.md) and plots all 40 BLE channels as a live-updating
bar chart. This is the antenna test documented on CONFIG_EW_BLE_CHANNEL_
SURVEY in Kconfig and survey_report() in src/ble/ble.c: a working antenna
shows clear humps at the three Wi-Fi channels (1/6/11, highlighted below);
a disconnected or badly-matched one shows flat thermal noise across the
whole sweep. Needs channel_survey.conf flashed (CONFIG_EW_BLE_CHANNEL_
SURVEY=y) — the RSSI-monitor and bring-up builds don't emit this line.

Usage:
    1. Flash channel_survey.conf, then in one terminal:
           sh /c/ewdev/rtt.sh
       (leave it running — this is what opens the RTT-to-TCP bridge)
    2. In another terminal:
           python tools/channel_survey_plot.py

Muted bars behind the live ones are peak hold — the strongest reading ever
seen on that channel, so a brief spike (e.g. while adjusting an antenna) is
still visible even if you looked away for it. Press 'r' with the plot window
focused to clear peak hold and start fresh, e.g. right before trying a new
antenna adjustment so old peaks don't linger and confuse the comparison.

Ctrl+C, or just close the plot window, to stop.
"""

import re
import socket
import threading
import time

import matplotlib.animation as animation
import matplotlib.pyplot as plt

RTT_HOST = "localhost"
RTT_PORT = 9090
NUM_CHANNELS = 40
RECONNECT_DELAY_S = 2
# Generous on purpose: rtt.sh has to connect OpenOCD to the probe, halt and
# examine the target, then scan target RAM for the "SEGGER RTT" control
# block before its TCP server is even listening — that can take a good few
# seconds on its own, on top of however long you take to start it relative
# to this script. This is only how long a single connection *attempt*
# waits; a refused/timed-out attempt just retries after RECONNECT_DELAY_S,
# so making this too short costs nothing but a slower first connect.
CONNECT_TIMEOUT_S = 20
# Bars grow upward from this floor rather than matplotlib's default bar()
# behaviour, which anchors at y=0 and — since these are all negative dBm
# values — draws every bar hanging *downward* from the top of the chart.
# -100 crops off the flat -106/-107 thermal noise floor we've actually
# measured on this board, so a bar only appears at all once there's a real
# reading to show, instead of every channel always rendering a near-full
# bar just from being negative.
BAR_BOTTOM_DBM = -100
FLASH_HOLD_FRAMES = 3  # ~3 animation frames (900ms at the 300ms interval)

# BLE channel index -> centre frequency (MHz). Matches the mapping documented
# on survey_report() in ble.c: indices 0-36 are data channels stepping 2 MHz
# up from 2404, while 37/38/39 are the three advertising channels, which sit
# at their own fixed spots (2402/2426/2480) chosen specifically to straddle
# Wi-Fi channels 1/6/11 rather than fall inside them — so they don't continue
# the linear sequence the way their index might suggest.
def channel_freq_mhz(i):
    if i == 37:
        return 2402
    if i == 38:
        return 2426
    if i == 39:
        return 2480
    return 2404 + 2 * i


FREQS = [channel_freq_mhz(i) for i in range(NUM_CHANNELS)]

# Wi-Fi's three 20 MHz-wide channels, centred on 2412/2437/2462 MHz. Any
# working antenna in a room with Wi-Fi in it should show energy humps here.
WIFI_BANDS = [(2401, 2423), (2426, 2448), (2451, 2473)]


def in_wifi_band(freq_mhz):
    return any(lo <= freq_mhz <= hi for lo, hi in WIFI_BANDS)


BAR_COLOR_WIFI = "#2a9d8f"
BAR_COLOR_OTHER = "#6c757d"
BAR_COLORS = [BAR_COLOR_WIFI if in_wifi_band(f) else BAR_COLOR_OTHER for f in FREQS]

# Matches survey_report()'s own LOG_INF format in ble.c: the value list is
# whitespace-separated, each entry either a signed dBm integer or ".." for
# an unmeasured channel (the 127 sentinel — survey work is scheduled at low
# priority, so gaps are normal, not a fault).
SURVEY_RE = re.compile(r"survey dBm by channel 0-39:(.+)")
SUMMARY_RE = re.compile(r"survey: (\d+)/(\d+) channels measured, strongest (-?\d+) dBm")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")  # log backend colours the level tag


class SurveyReader:
    """Owns the RTT TCP connection on a background thread and hands the
    latest parsed reading to the plot's animation callback. Reconnects on
    its own if rtt.sh hasn't been started yet or the link drops — this is
    meant to be left running unattended while wire length is adjusted by
    hand, not babysat."""

    def __init__(self):
        self.values = [None] * NUM_CHANNELS
        self.peaks = [None] * NUM_CHANNELS
        self.strongest = None
        self.lock = threading.Lock()
        self.connected = False
        # seq increments once per *actual* new reading, independent of the
        # animation's own fixed redraw timer — that timer fires every
        # 300ms whether or not fresh data has actually arrived, which is
        # exactly why "is this frozen or just quiet" was hard to tell.
        # last_update_mono backs the "Xs ago" readout; monotonic rather
        # than wall-clock so it can't jump from a system clock adjustment.
        self.seq = 0
        self.last_update_mono = None

    def _parse_line(self, line):
        m = SURVEY_RE.search(line)
        if m:
            vals = []
            for tok in m.group(1).split():
                if tok == "..":
                    vals.append(None)
                else:
                    try:
                        vals.append(int(tok))
                    except ValueError:
                        return  # malformed line — drop it, keep the old reading
            if len(vals) == NUM_CHANNELS:
                with self.lock:
                    self.values = vals
                    self.seq += 1
                    self.last_update_mono = time.monotonic()
                    for i, v in enumerate(vals):
                        if v is not None and (self.peaks[i] is None or v > self.peaks[i]):
                            self.peaks[i] = v
            return

        m = SUMMARY_RE.search(line)
        if m:
            with self.lock:
                self.strongest = int(m.group(3))

    def reset_peaks(self):
        with self.lock:
            self.peaks = [None] * NUM_CHANNELS

    def run(self):
        while True:
            try:
                with socket.create_connection((RTT_HOST, RTT_PORT), timeout=CONNECT_TIMEOUT_S) as sock:
                    # create_connection()'s timeout otherwise stays the
                    # socket's default for every later recv() too — a quiet
                    # stretch between survey prints (routine; the target
                    # isn't always mid-line) would then raise a timeout,
                    # get caught below as if the bridge had dropped, and
                    # force a reconnect. Once actually connected there's no
                    # reason to time out at all: block indefinitely and let
                    # a real dead connection surface as a recv() returning
                    # empty (peer closed) instead.
                    sock.settimeout(None)
                    self.connected = True
                    print(f"connected to RTT bridge at {RTT_HOST}:{RTT_PORT}")
                    buf = b""
                    while True:
                        data = sock.recv(4096)
                        if not data:
                            break
                        buf += data
                        while b"\n" in buf:
                            raw, buf = buf.split(b"\n", 1)
                            text = ANSI_RE.sub("", raw.decode("utf-8", errors="ignore"))
                            self._parse_line(text)
            except OSError as exc:
                self.connected = False
                print(f"RTT bridge not reachable ({exc}) — "
                      f"is 'sh rtt.sh' running? retrying in {RECONNECT_DELAY_S}s")
            time.sleep(RECONNECT_DELAY_S)

    def snapshot(self):
        with self.lock:
            return (list(self.values), list(self.peaks), self.strongest,
                    self.seq, self.last_update_mono)


def main():
    reader = SurveyReader()
    threading.Thread(target=reader.run, daemon=True).start()

    fig, ax = plt.subplots(figsize=(14, 6))
    x = range(NUM_CHANNELS)

    # Peak-hold bars drawn first (lower zorder), full channel width, in a
    # single muted colour regardless of Wi-Fi band — they only need to read
    # as "the ghost of a past spike", not repeat the band colour-coding.
    # The live bars drawn on top fully cover them up to the current value,
    # so a peak only becomes visible where it's *higher* than what's
    # showing right now — which is exactly the point of a peak hold.
    #
    # bottom=BAR_BOTTOM_DBM anchors every bar's *base* there once, up front;
    # only the height (set each frame in update(), always as a value-minus-
    # floor delta, never the raw dBm) changes afterwards, so bars grow
    # upward from a fixed floor instead of hanging down from y=0.
    peak_bars = ax.bar(
        x, [0] * NUM_CHANNELS, bottom=BAR_BOTTOM_DBM,
        color="#e76f51", alpha=0.55, zorder=1,
    )
    bars = ax.bar(x, [0] * NUM_CHANNELS, bottom=BAR_BOTTOM_DBM, color=BAR_COLORS, zorder=2)

    ax.set_ylim(BAR_BOTTOM_DBM, 0)
    ax.set_xlim(-1, NUM_CHANNELS)
    ax.set_xlabel("BLE channel index (0-39)")
    ax.set_ylabel("energy (dBm)")
    ax.set_title("Every Watch — raw RF channel survey (live)  —  press 'r' to reset peak hold")
    ax.set_xticks(range(0, NUM_CHANNELS, 2))
    ax.grid(axis="y", alpha=0.3, zorder=0)

    status_text = ax.text(
        0.01, 0.97, "", transform=ax.transAxes, va="top", ha="left", fontsize=10,
        bbox=dict(boxstyle="round", fc="white", alpha=0.85),
    )
    # New-data indicator: a dot that flashes bright green the instant a
    # fresh reading actually lands, then fades back to idle grey — the
    # animation timer redraws every 300ms regardless of whether anything
    # new has arrived, so without this there's no way to tell "live" from
    # "frozen on the last reading". FLASH_HOLD_FRAMES is separate from the
    # colour fade purely so a single-frame flash is never too quick to
    # register.
    flash_indicator = ax.text(
        0.995, 0.97, "●", transform=ax.transAxes, va="top", ha="right",
        fontsize=16, color="#adb5bd",
    )
    ax.legend(
        handles=[
            plt.Rectangle((0, 0), 1, 1, color=BAR_COLOR_WIFI, label="Wi-Fi 1/6/11 band"),
            plt.Rectangle((0, 0), 1, 1, color=BAR_COLOR_OTHER, label="other"),
            plt.Rectangle((0, 0), 1, 1, color="#e76f51", alpha=0.55, label="peak hold"),
        ],
        loc="upper right",
    )

    def on_key(event):
        if event.key == "r":
            reader.reset_peaks()

    fig.canvas.mpl_connect("key_press_event", on_key)

    # Closure state for the new-data indicator — plain mutable list cells
    # since `update()` needs to write them and Python closures can't
    # rebind an outer int/None with plain `=`. Starts at 0, matching
    # SurveyReader.seq's own initial value — anything else (e.g. -1) reads
    # as "different" on the very first frame, before any real data has
    # arrived, and flashes green for nothing.
    last_seen_seq = [0]
    flash_frames_left = [0]

    def update(_frame):
        values, peaks, strongest, seq, last_update_mono = reader.snapshot()
        measured = 0
        for bar, v in zip(bars, values):
            if v is None:
                bar.set_height(0)
            else:
                # Height is the delta above the floor, not the raw dBm
                # value — bottom was fixed at BAR_BOTTOM_DBM when the bars
                # were created, so this is what makes them grow upward from
                # it. Clamped at 0: a reading below the floor (routine —
                # that's the whole point of the floor) draws nothing rather
                # than a bar poking out below the axis.
                bar.set_height(max(0, v - BAR_BOTTOM_DBM))
                measured += 1
        for peak_bar, p in zip(peak_bars, peaks):
            peak_bar.set_height(max(0, p - BAR_BOTTOM_DBM) if p is not None else 0)

        # seq only increments when a genuinely new reading was parsed — see
        # SurveyReader._parse_line() — so this fires once per real update,
        # not once per redraw. Re-arm the hold on every new reading rather
        # than just checking "> 0" so back-to-back fresh readings keep it
        # solid green instead of flickering between calls.
        if seq != last_seen_seq[0]:
            last_seen_seq[0] = seq
            flash_frames_left[0] = FLASH_HOLD_FRAMES

        if flash_frames_left[0] > 0:
            flash_indicator.set_color("#2ecc71")
            flash_frames_left[0] -= 1
        else:
            flash_indicator.set_color("#adb5bd")

        if last_update_mono is None:
            age_str = "no data yet"
        else:
            age_str = f"{time.monotonic() - last_update_mono:.1f}s ago"

        status_text.set_text(
            f"{'connected' if reader.connected else 'reconnecting...'}  |  "
            f"{measured}/{NUM_CHANNELS} channels measured  |  "
            f"strongest: {strongest if strongest is not None else '--'} dBm  |  "
            f"last update: {age_str}"
        )
        return list(bars) + list(peak_bars) + [status_text, flash_indicator]

    # blit=False: bar heights change every frame anyway, and False is more
    # forgiving of the text bbox redraw than trying to blit around it.
    _ani = animation.FuncAnimation(fig, update, interval=300, blit=False)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
