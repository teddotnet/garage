#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import subprocess
import sys


FRAME_RE = re.compile(r"^clip(\d+)_frame(\d+)\.jpg$")


def parse_args():
    ap = argparse.ArgumentParser(description="Build an MP4 from saved MJPEG frames.")
    ap.add_argument("--outdir", default="out", help="Directory containing clip*_frame*.jpg")
    ap.add_argument("--clip-id", type=int, default=None, help="Clip id to render (default: auto)")
    ap.add_argument("--fps", type=int, default=10, help="Frames per second (default: 10)")
    ap.add_argument("--out", default=None, help="Output mp4 path (default: out/clip<ID>.mp4)")
    return ap.parse_args()


def scan_frames(outdir):
    clips = {}
    try:
        entries = os.listdir(outdir)
    except FileNotFoundError:
        return clips

    for name in entries:
        m = FRAME_RE.match(name)
        if not m:
            continue
        clip_id = int(m.group(1))
        frame_id = int(m.group(2))
        clips.setdefault(clip_id, []).append(frame_id)
    return clips


def select_clip(clips, wanted):
    if not clips:
        return None, None
    if wanted is not None:
        return wanted, clips.get(wanted, [])
    if len(clips) == 1:
        clip_id = next(iter(clips.keys()))
        return clip_id, clips[clip_id]
    clip_id = max(clips.keys())
    return clip_id, clips[clip_id]


def main():
    args = parse_args()

    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg not found in PATH.")

    clips = scan_frames(args.outdir)
    clip_id, frames = select_clip(clips, args.clip_id)
    if clip_id is None:
        raise SystemExit(f"No frames found in {args.outdir}")
    if not frames:
        raise SystemExit(f"No frames found for clip {clip_id}")

    start_number = min(frames)
    out_path = args.out or os.path.join(args.outdir, f"clip{clip_id}.mp4")
    pattern = os.path.join(args.outdir, f"clip{clip_id}_frame%d.jpg")

    cmd = [
        "ffmpeg",
        "-y",
        "-framerate",
        str(args.fps),
        "-start_number",
        str(start_number),
        "-i",
        pattern,
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        out_path,
    ]

    print("Running:", " ".join(cmd))
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"ffmpeg failed with code {exc.returncode}") from exc

    print(f"Wrote {out_path}")


if __name__ == "__main__":
    sys.exit(main())
