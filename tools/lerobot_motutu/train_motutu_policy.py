from __future__ import annotations

import argparse
import importlib.util
import shlex
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def has_package(name: str) -> bool:
    return importlib.util.find_spec(name) is not None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build or run a LeRobot training command for Motutu.")
    parser.add_argument("--dataset-repo-id", required=True, help="Recorded LeRobot dataset repo id")
    parser.add_argument("--dataset-root", required=True, help="Local LeRobot dataset root")
    parser.add_argument("--policy-path", required=True, help="Base pretrained policy path or Hub repo id")
    parser.add_argument("--output-dir", required=True, help="Training output directory")
    parser.add_argument("--steps", type=int, default=20000, help="Training steps")
    parser.add_argument("--batch-size", type=int, default=8, help="Training batch size")
    parser.add_argument("--eval-split", type=float, default=0.1, help="Held-out eval split")
    parser.add_argument("--device", default="cpu", help="Training device, e.g. cpu or cuda")
    parser.add_argument("--save-freq", type=int, default=5000, help="Checkpoint save frequency")
    parser.add_argument("--job-name", default="motutu-policy", help="Training job name")
    parser.add_argument("--print-only", action="store_true", help="Only print the resolved training command")
    parser.add_argument("--run", action="store_true", help="Run the official LeRobot training script immediately")
    return parser.parse_args()


def build_command(args: argparse.Namespace) -> list[str]:
    train_script = REPO_ROOT / "tmp" / "lerobot-env-py313" / "Lib" / "site-packages" / "lerobot" / "scripts" / "lerobot_train.py"
    return [
        str(REPO_ROOT / "tmp" / "lerobot-env-py313" / "Scripts" / "python.exe"),
        str(train_script),
        f"--dataset.repo_id={args.dataset_repo_id}",
        f"--dataset.root={Path(args.dataset_root).resolve()}",
        f"--dataset.eval_split={args.eval_split}",
        f"--policy.path={args.policy_path}",
        f"--output_dir={Path(args.output_dir).resolve()}",
        f"--job_name={args.job_name}",
        f"--steps={args.steps}",
        f"--batch_size={args.batch_size}",
        f"--save_freq={args.save_freq}",
        "--wandb.enable=false",
        f"--policy.device={args.device}",
    ]


def main() -> int:
    args = parse_args()
    command = build_command(args)
    pretty = " ".join(shlex.quote(part) for part in command)
    print(pretty)

    if args.print_only or not args.run:
        missing = [name for name in ("datasets", "accelerate") if not has_package(name)]
        if missing:
            print(
                "提示: 当前环境还缺少训练依赖 -> "
                + ", ".join(missing)
                + "。要真正执行训练，请先安装 lerobot[training]。"
            )
        return 0

    if not has_package("datasets") or not has_package("accelerate"):
        raise RuntimeError("当前环境还不能直接训练，请先安装 lerobot[training]")

    return subprocess.call(command, cwd=REPO_ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
