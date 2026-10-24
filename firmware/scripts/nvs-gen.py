
Import("env")

import os
import csv
import subprocess
from pathlib import Path

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
BUILD_DIR = Path(env.subst("$BUILD_DIR"))

CSV_FILE = PROJECT_DIR / "nvs_data.csv"
PARTITION_FILE = PROJECT_DIR / env.GetProjectOption(
    "board_build.partitions",
    "partitions.csv",
)

NVS_BIN = BUILD_DIR / "nvs_data.bin"


def get_nvs_partition():
    with PARTITION_FILE.open(newline="") as file:
        for row in csv.reader(file):
            if not row or row[0].strip().startswith("#"):
                continue

            row = [item.strip() for item in row]

            if len(row) < 5:
                continue

            name, ptype, subtype, offset, size = row[:5]

            if name == "nvs" and ptype == "data" and subtype == "nvs":
                if not offset:
                    raise RuntimeError("NVS offset must be explicit")

                return int(offset, 0), int(size, 0)

    raise RuntimeError("NVS partition not found")


def find_nvs_generator():
    candidates = []

    idf_path = os.environ.get("IDF_PATH")

    if idf_path:
        candidates.append(
            Path(idf_path)
            / "components/nvs_flash/nvs_partition_generator"
            / "nvs_partition_gen.py"
        )

    packages = Path(env.subst("$PROJECT_PACKAGES_DIR"))

    candidates.append(
        packages
        / "framework-espidf/components/nvs_flash"
        / "nvs_partition_generator/nvs_partition_gen.py"
    )

    for path in candidates:
        if path.is_file():
            return path

    raise RuntimeError("nvs_partition_gen.py not found")


def generate_nvs(source, target, env):
    generator = find_nvs_generator()
    _, partition_size = get_nvs_partition()

    command = [
        env.subst("$PYTHONEXE"),
        str(generator),
        "generate",
        str(CSV_FILE),
        str(NVS_BIN),
        hex(partition_size),
    ]

    subprocess.run(command, check=True)


def upload_nvs(source, target, env):
    offset, _ = get_nvs_partition()

    esptool = Path(
        env.PioPlatform().get_package_dir("tool-esptoolpy")
    ) / "esptool.py"

    command = [
        env.subst("$PYTHONEXE"),
        str(esptool),
        "--chip",
        env.BoardConfig().get("build.mcu", "esp32"),
        "--port",
        env.subst("$UPLOAD_PORT"),
        "--baud",
        env.subst("$UPLOAD_SPEED"),
        "write_flash",
        hex(offset),
        str(NVS_BIN),
    ]

    subprocess.run(command, check=True)


nvs_target = env.Command(
    str(NVS_BIN),
    str(CSV_FILE),
    generate_nvs,
)

env.Alias("generate_nvs_bin", nvs_target)

env.AddCustomTarget(
    name="upload_nvs_bin",
    dependencies=nvs_target,
    actions=[upload_nvs],
    title="Upload NVS",
    description="Generate and flash NVS partition",
)

env["NVS_BIN"] = str(NVS_BIN)
