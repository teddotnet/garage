#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import struct
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

VID_MAGIC = 0x56494430  # 'VID0'
HDR_FMT = "<IIIIHHIIHH"
HDR_SIZE = struct.calcsize(HDR_FMT)


class FrameBuffer:
    def __init__(self, meta):
        self.meta = meta
        self.chunks = {}
        self.last_ts = time.time()

    def add_chunk(self, chunk_id, data):
        self.chunks[chunk_id] = data
        self.last_ts = time.time()

    def is_complete(self):
        return len(self.chunks) == self.meta["chunk_count"]

    def assemble(self):
        return b"".join(self.chunks[i] for i in range(self.meta["chunk_count"]))


def parse_args():
    ap = argparse.ArgumentParser(description="Receive frames and auto-render MP4 on control end.")
    ap.add_argument("--broker", required=True, help="Broker URI, e.g. mqtt://192.168.1.10:1883")
    ap.add_argument("--topic", default="cam/vid", help="MQTT topic for video chunks")
    ap.add_argument("--ctrl-topic", default="cam/ctl", help="MQTT control topic")
    ap.add_argument("--outdir", default="out", help="Output directory for frames")
    ap.add_argument("--fps", type=int, default=30, help="FPS for ffmpeg output")
    ap.add_argument("--idle-seconds", type=float, default=2.0, help="Render if idle after end")
    ap.add_argument("--keep-seconds", type=int, default=10, help="Drop incomplete frames after this time")
    ap.add_argument("--clean-on-start", action="store_true", help="Delete existing frames for a clip on start")
    return ap.parse_args()


def ensure_outdir(path):
    os.makedirs(path, exist_ok=True)


def decode_hdr(payload):
    if len(payload) < HDR_SIZE:
        return None, None
    fields = struct.unpack(HDR_FMT, payload[:HDR_SIZE])
    hdr = {
        "magic": fields[0],
        "clip_id": fields[1],
        "frame_id": fields[2],
        "ts_ms": fields[3],
        "chunk_id": fields[4],
        "chunk_count": fields[5],
        "frame_size": fields[6],
        "fourcc": fields[7],
        "width": fields[8],
        "height": fields[9],
    }
    return hdr, payload[HDR_SIZE:]


def run_ffmpeg(outdir, clip_id, fps):
    out_path = os.path.join(outdir, f"clip{clip_id}.mp4")
    cmd = [
        sys.executable,
        os.path.join(os.path.dirname(__file__), "frames_to_video.py"),
        "--outdir",
        outdir,
        "--clip-id",
        str(clip_id),
        "--fps",
        str(fps),
        "--out",
        out_path,
    ]
    print("running:", " ".join(cmd))
    return subprocess.run(cmd, check=False).returncode


def main():
    args = parse_args()
    ensure_outdir(args.outdir)

    frames = {}
    clip_state = {}
    rendered = set()

    def on_video(msg):
        hdr, body = decode_hdr(msg.payload)
        if not hdr or hdr["magic"] != VID_MAGIC:
            return

        key = (hdr["clip_id"], hdr["frame_id"])
        fb = frames.get(key)
        if fb is None:
            frames[key] = fb = FrameBuffer(hdr)

        fb.add_chunk(hdr["chunk_id"], body)

        if fb.is_complete():
            frame = fb.assemble()
            del frames[key]

            if len(frame) != hdr["frame_size"]:
                print(f"frame size mismatch clip={hdr['clip_id']} frame={hdr['frame_id']}")
                return

            ext = ".jpg" if hdr["fourcc"] == 0x47504A4D else ".bin"
            fname = f"clip{hdr['clip_id']}_frame{hdr['frame_id']}{ext}"
            out_path = os.path.join(args.outdir, fname)
            with open(out_path, "wb") as f:
                f.write(frame)

            md5 = hashlib.md5(frame).hexdigest()
            avg = sum(frame) / len(frame)
            print(
                f"saved {out_path} size={len(frame)} md5={md5} avg_byte={avg:.1f} "
                f"{hdr['width']}x{hdr['height']} ts={hdr['ts_ms']}ms"
            )

            st = clip_state.setdefault(hdr["clip_id"], {
                "expected": None,
                "done": False,
                "count": 0,
                "last_frame": time.time(),
            })
            st["count"] += 1
            st["last_frame"] = time.time()
            try_render(hdr["clip_id"])

    def try_render(clip_id):
        if clip_id in rendered:
            return
        st = clip_state.get(clip_id)
        if not st or not st.get("done"):
            return

        if st.get("expected"):
            if st["count"] < st["expected"]:
                return
        else:
            if time.time() - st.get("last_frame", 0) < args.idle_seconds:
                return

        rc = run_ffmpeg(args.outdir, clip_id, args.fps)
        if rc == 0:
            rendered.add(clip_id)
            print(f"rendered clip{clip_id}.mp4")
        else:
            print(f"ffmpeg failed for clip {clip_id}")

    def on_ctrl(msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            return

        event = payload.get("event")
        clip_id = payload.get("clip_id")
        if event not in ("start", "end") or clip_id is None:
            return

        clip_id = int(clip_id)
        if event == "start":
            if args.clean_on_start:
                for name in os.listdir(args.outdir):
                    if name.startswith(f"clip{clip_id}_frame") and name.endswith(".jpg"):
                        try:
                            os.remove(os.path.join(args.outdir, name))
                        except OSError:
                            pass
            clip_state[clip_id] = {
                "expected": None,
                "done": False,
                "count": 0,
                "last_frame": time.time(),
            }
            print(f"control start clip={clip_id}")
        elif event == "end":
            st = clip_state.setdefault(clip_id, {
                "expected": None,
                "done": False,
                "count": 0,
                "last_frame": time.time(),
            })
            st["done"] = True
            if "frames" in payload:
                try:
                    st["expected"] = int(payload["frames"])
                except (TypeError, ValueError):
                    pass
            print(f"control end clip={clip_id} expected={st.get('expected')}")
            try_render(clip_id)

    def cleanup_stale():
        now = time.time()
        stale = [k for k, fb in frames.items() if now - fb.last_ts > args.keep_seconds]
        for k in stale:
            del frames[k]
        for clip_id in list(clip_state.keys()):
            try_render(clip_id)

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)

    def on_connect(client, userdata, flags, reason_code, properties=None):
        if reason_code == 0:
            client.subscribe(args.topic, qos=0)
            client.subscribe(args.ctrl_topic, qos=0)
            print(f"subscribed to {args.topic} and {args.ctrl_topic}")
        else:
            print(f"connect failed: {reason_code}")

    def on_message(client, userdata, msg):
        if msg.topic == args.ctrl_topic:
            on_ctrl(msg)
        else:
            on_video(msg)

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
