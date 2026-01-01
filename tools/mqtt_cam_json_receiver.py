#!/usr/bin/env python3
import argparse
import base64
import json
import os
import subprocess
import sys
import time
from urllib.parse import urlparse

try:
    import paho.mqtt.client as mqtt
except ImportError as exc:
    raise SystemExit(
        "Missing dependency: paho-mqtt. Install with:\n"
        "  python3 -m pip install -r requirements.txt\n"
        "If you are not in a virtualenv, you can also use:\n"
        "  python3 -m pip install --user paho-mqtt"
    ) from exc


def parse_args():
    ap = argparse.ArgumentParser(description="Receive JSON frames over MQTT and render MP4.")
    ap.add_argument("--broker", required=True, help="Broker URI, e.g. mqtt://192.168.1.10:1883")
    ap.add_argument("--topic", default="cam/vid", help="MQTT topic to subscribe to")
    ap.add_argument("--outdir", default="out", help="Output directory for frames and mp4")
    ap.add_argument("--fps", type=int, default=30, help="FPS for ffmpeg output")
    ap.add_argument("--idle-seconds", type=float, default=2.0, help="Render if idle after end")
    ap.add_argument("--keep-seconds", type=int, default=30, help="Drop stale incomplete clips after this time")
    return ap.parse_args()


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def safe_name(value):
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in value)


def run_ffmpeg(outdir, serial, ts, fps):
    clip_dir = os.path.join(outdir, serial, ts)
    out_path = os.path.join(outdir, serial, f"{ts}.mp4")
    pattern = os.path.join(clip_dir, "frame%06d.jpg")
    cmd = [
        "ffmpeg",
        "-y",
        "-framerate",
        str(fps),
        "-i",
        pattern,
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        out_path,
    ]
    print("running:", " ".join(cmd))
    return subprocess.run(cmd, check=False).returncode


def main():
    args = parse_args()
    ensure_dir(args.outdir)

    captures = {}
    rendered = set()

    def clip_key(serial, ts):
        return (serial, ts)

    def ensure_clip(serial, ts):
        key = clip_key(serial, ts)
        if key not in captures:
            captures[key] = {
                "outof": None,
                "frames": {},
                "last": time.time(),
            }
        return captures[key]

    def try_render(serial, ts):
        key = clip_key(serial, ts)
        if key in rendered:
            return
        st = captures.get(key)
        if not st:
            return
        outof = st.get("outof")
        if outof is None:
            return
        if len(st["frames"]) < outof:
            return

        serial_dir = os.path.join(args.outdir, serial)
        clip_dir = os.path.join(serial_dir, ts)
        ensure_dir(clip_dir)

        for idx, data in sorted(st["frames"].items()):
            fname = os.path.join(clip_dir, f"frame{idx:06d}.jpg")
            with open(fname, "wb") as f:
                f.write(data)

        rc = run_ffmpeg(args.outdir, serial, ts, args.fps)
        if rc == 0:
            rendered.add(key)
            print(f"rendered {os.path.join(args.outdir, serial, ts + '.mp4')}")
        else:
            print(f"ffmpeg failed for {serial} {ts}")

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            return

        serial = safe_name(str(payload.get("serial", "unknown")))
        ts = safe_name(str(payload.get("timestamp", "unknown")))
        index = payload.get("index")
        outof = payload.get("outof")
        frame = payload.get("frame")
        if index is None or outof is None or not frame:
            return

        try:
            index = int(index)
            outof = int(outof)
        except (TypeError, ValueError):
            return

        try:
            data = base64.b64decode(frame)
        except Exception:
            return

        st = ensure_clip(serial, ts)
        st["outof"] = outof
        st["frames"][index] = data
        st["last"] = time.time()
        print(f"recv serial={serial} ts={ts} index={index} outof={outof}")
        try_render(serial, ts)

    def cleanup_stale():
        now = time.time()
        stale = []
        for key, st in captures.items():
            if now - st.get("last", now) > args.keep_seconds:
                stale.append(key)
        for key in stale:
            captures.pop(key, None)

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)

    def on_connect(client, userdata, flags, reason_code, properties=None):
        if reason_code == 0:
            client.subscribe(args.topic, qos=0)
            print(f"subscribed to {args.topic}")
        else:
            print(f"connect failed: {reason_code}")

    client.on_connect = on_connect
    client.on_message = on_message

    url = urlparse(args.broker)
    host = url.hostname or args.broker
    port = url.port or 1883
    try:
        client.connect(host, port, 60)
    except Exception as exc:
        raise SystemExit(f"MQTT connect failed to {host}:{port} ({exc})") from exc

    try:
        while True:
            client.loop(timeout=0.2)
            cleanup_stale()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
