#!/usr/bin/env bash
set -euo pipefail

PYTHONUNBUFFERED=1 /home/bjorn/.espressif/python_env/idf5.4_py3.10_env/bin/python -u \
  /home/bjorn/projects/p4_mqtt_cam/tools/mqtt_cam_receiver.py \
  --broker mqtt://192.168.7.1:1883 \
  --topic cam/vid \
  --outdir out \
  --fps 30
