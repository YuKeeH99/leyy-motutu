# Motutu LeRobot Bridge

This folder provides a minimal LeRobot-compatible bridge for the Motutu web API.

## What It Does

- Wraps the device web API as a LeRobot `Robot`
- Supports:
  - `connect()`
  - `get_observation()`
  - `send_action()`
  - `disconnect()`
- Uses the existing device endpoints:
  - `GET /api/dog/meta`
  - `GET /api/dog/status`
  - `POST /api/dog/sequence`
  - `GET /api/dog/show_page`

## Files

- `motutu_web_robot.py`
  - `MotutuWebRobotConfig`
  - `MotutuWebRobot`
- `demo_motutu_lerobot.py`
  - minimal local test script
- `record_motutu_episode.py`
  - execute a trajectory and save step data as JSONL or LeRobot dataset
- `replay_motutu_episode.py`
  - replay a recorded JSONL episode or a sequence JSON file
- `generate_motutu_sequence.py`
  - generate sequence JSON from a high-level template or natural-language prompt
- `run_motutu_policy.py`
  - load a pretrained LeRobot policy and run inference on the device
- `train_motutu_policy.py`
  - build or execute the official LeRobot training command for a recorded dataset
- `lerobot_dataset_bridge.py`
  - dataset/camera helpers shared by the record and policy scripts
- `trajectory_io.py`
  - helpers for sequence and JSONL I/O

## Recommended Local Workflow

1. Prefer using the device LAN URL directly.

For the current device:

```powershell
http://192.168.31.140:8080
```

`adb forward tcp:18080 tcp:8080` is not reliable on this platform. It can produce immediate connection close errors even when the web service is healthy.

2. If you still want to try forwarding:

```powershell
adb forward tcp:18080 tcp:8080
```

3. Use the installed LeRobot environment:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\demo_motutu_lerobot.py --base-url http://192.168.31.140:8080
```

4. Show the device control page if needed:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\demo_motutu_lerobot.py --base-url http://192.168.31.140:8080 --show-page
```

5. Print the robot schema and joint limits:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\demo_motutu_lerobot.py --base-url http://192.168.31.140:8080 --schema
```

## Action and Observation Format

This bridge uses a 4-servo schema.

Action keys:

- `lf`
- `rf`
- `lr`
- `rr`
- `move_ms`
- `pause_ms`

Observation keys:

- `ready`
- `hold_ms`
- `joint_count`
- `lf`
- `rf`
- `lr`
- `rr`
- `logical_lf`
- `logical_rf`
- `logical_lr`
- `logical_rr`

When cameras are configured, each camera name is also exposed as an observation key
with shape `(height, width, 3)`.

Example observation:

```json
{
  "lf": 130.0,
  "rf": 50.0,
  "lr": 130.0,
  "rr": 50.0
}
```

Example action:

```json
{
  "lf": 96.0,
  "rf": 84.0,
  "lr": 92.0,
  "rr": 88.0,
  "move_ms": 120.0,
  "pause_ms": 0.0
}
```

## Faster Than Manual Coordinates

You do not need to hand-write every frame.

Generate a sequence from a high-level template:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\generate_motutu_sequence.py --base-url http://192.168.31.140:8080 --kind walk --speed slow --steps 3 --amplitude 8 --append-home --out .\tmp\motutu-seq\walk-slow.json
```

Generate directly from natural language:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\generate_motutu_sequence.py --base-url http://192.168.31.140:8080 --prompt "前进3步 慢速 小摆幅" --append-home --out .\tmp\motutu-seq\walk-prompt.json
```

Generate and execute immediately:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\generate_motutu_sequence.py --base-url http://192.168.31.140:8080 --prompt "左转2步 慢速" --run
```

## Record and Replay Workflow

Record a small episode from inline poses:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\record_motutu_episode.py --base-url http://192.168.31.140:8080 --out .\tmp\motutu-episodes\episode-001.jsonl --pose 130,50,130,50 --pose 142,38,136,44 --pose 130,50,130,50 --move-ms 180 --pause-ms 40 --home-at-end
```

Replay that JSONL episode:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\replay_motutu_episode.py --base-url http://192.168.31.140:8080 --episode .\tmp\motutu-episodes\episode-001.jsonl --home-at-end
```

Record from a sequence JSON file:

```json
{
  "a": [
    {"s": {"lf": 130, "rf": 50, "lr": 130, "rr": 50}, "v": 150, "d": 30},
    {"s": {"lf": 142, "rf": 38, "lr": 136, "rr": 44}, "v": 180, "d": 40}
  ]
}
```

Then:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\record_motutu_episode.py --base-url http://192.168.31.140:8080 --sequence-file .\tmp\motutu_walk.json --out .\tmp\motutu-episodes\episode-walk.jsonl
```

Record directly into an official LeRobot dataset root:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\record_motutu_episode.py --base-url http://192.168.31.140:8080 --sequence-file .\tmp\motutu_walk.json --dataset-repo-id yourname/motutu-demo --dataset-root .\tmp\motutu-dataset --dataset-task "walk demo"
```

Attach an OpenCV camera while recording:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\record_motutu_episode.py --base-url http://192.168.31.140:8080 --pose 130,50,130,50 --dataset-repo-id yourname/motutu-cam --dataset-root .\tmp\motutu-cam-dataset --camera front=0,640,480,30
```

Run a pretrained policy on the robot:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\run_motutu_policy.py --base-url http://192.168.31.140:8080 --policy-path .\outputs\motutu-act\checkpoints\last\pretrained_model --task "stand still" --steps 10
```

Prepare or launch the official training command:

```powershell
.\tmp\lerobot-env-py313\Scripts\python .\tools\lerobot_motutu\train_motutu_policy.py --dataset-repo-id yourname/motutu-demo --dataset-root .\tmp\motutu-dataset --policy-path lerobot/act_koch_real --output-dir .\outputs\motutu-act --print-only
```

## What This Enables

- define a 4-servo action space
- read servo state from the current device web API
- save execution episodes as JSONL
- replay those episodes locally
- build a clean dataset before adding video or policy training

## Notes

- This is a minimal bridge, not a full LeRobot upstream integration.
- It keeps the custom robot code inside the repo instead of modifying the installed LeRobot package.
- The bridge clamps outgoing actions to the angle limits reported by `/api/dog/meta`.
- `home()` and default demo poses use each joint's `center_abs_deg` reported by `/api/dog/meta`, so structural reassembly only needs one calibration update on the device side.
- The current observation is based on the device web status, not hardware encoder feedback.
- The current local environment has `cv2`, but the `datasets` and `accelerate` extras may still need installation before official dataset writing or training can run end-to-end.
