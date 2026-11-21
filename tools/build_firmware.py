"""Build and package an ESP32-S3 StopWatch image without provisioning it."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from prepare import PIN, ROOT, prepare

BOARD = "m5stack-stopwatch"
IMAGE_NAMES = {
    "watch": "stopwatch-token-monitor",
    "personal": "stopwatch-hermes-token-monitor",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=IMAGE_NAMES, default="watch")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    build = (args.build_dir or ROOT / "build" / args.profile).resolve()
    out = (args.out or ROOT / "dist" / args.profile).resolve()
    prepare(build, args.profile)
    project = build / "firmware" / "esp32"
    subprocess.run([sys.executable, "-m", "platformio", "run", "-d", str(project), "-e", BOARD], check=True)
    package = project / "tools" / "package_release.py"
    name = IMAGE_NAMES[args.profile]
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="stopwatch-package-") as tmp:
        subprocess.run(
            [sys.executable, str(package), "--project", str(project), "--board", BOARD, "--out", tmp],
            check=True,
        )
        [app] = Path(tmp).glob("hermes-gadget-m5stack-stopwatch-*-app.bin")
        merged = app.with_name(app.name.removesuffix("-app.bin") + ".bin")
        files = {}
        for suffix, original in ((".bin", merged), ("-app.bin", app)):
            destination = out / (name + suffix)
            data = original.read_bytes()
            destination.write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            files[destination.name] = {"sha256": digest, "bytes": len(data)}
        for notice in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
            shutil.copyfile(build / notice, out / ("hermes-gadget-" + notice))
        licenses = out / "LICENSES"
        licenses.mkdir(exist_ok=True)
        for font_license in ("Fusion-Pixel-OFL.txt", "Ark-Pixel-OFL.txt", "Cubic-11-License.txt", "Galmuri-OFL.txt"):
            shutil.copyfile(build / "LICENSES" / font_license, licenses / font_license)
        shutil.copyfile(ROOT / "LICENSE", out / "LICENSE")
        receipt = {
            "board": "M5Stack StopWatch C152",
            "profile": args.profile,
            "hermes_gadget_revision": PIN,
            "monitor_revision": subprocess.check_output(
                ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True
            ).strip(),
            "monitor_dirty": bool(subprocess.check_output(
                ["git", "-C", str(ROOT), "status", "--porcelain"], text=True
            ).strip()),
            "merged_image_flash_offset": "0x0",
            "files": files,
        }
        (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"Firmware packaged in {out}")


if __name__ == "__main__":
    main()
