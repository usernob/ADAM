
Import("env")

import subprocess
from pathlib import Path

BUILD_DIR = env.subst("$BUILD_DIR")
PROJECT_DIR = env.subst("$PROJECT_DIR")

MERGED_BIN = Path(BUILD_DIR) / "firmware.merged.bin"
NVS_BIN = Path(BUILD_DIR) / "nvs_data.bin"


def get_nvs_offset():
    import csv

    partition_file = str(Path(PROJECT_DIR) /
        env.GetProjectOption(
            "board_build.partitions",
            "partitions.csv",
        ),
    )

    with open(partition_file, newline="") as file:
        for row in csv.reader(file):
            if not row or row[0].strip().startswith("#"):
                continue

            row = [item.strip() for item in row]

            if len(row) >= 5:
                name, ptype, subtype, offset, size = row[:5]

                if name == "nvs" and ptype == "data" and subtype == "nvs":
                    if not offset:
                        raise RuntimeError(
                            "NVS partition needs explicit offset"
                        )

                    return int(offset, 0)

    raise RuntimeError("NVS partition not found")


def merge_firmware(source, target, env):
    board = env.BoardConfig()

    esptool = Path(env.PioPlatform().get_package_dir("tool-esptoolpy")) / "esptool.py"

    # Bootloader and partition table
    images = [
        (offset, env.subst(path))
        for offset, path in env.get("FLASH_EXTRA_IMAGES", [])
    ]

    # Application binary
    app_bin = env.subst(
        "$BUILD_DIR/${PROGNAME}.bin"
    )

    images.append((
        env.subst("$ESP32_APP_OFFSET"),
        app_bin,
    ))

    # NVS binary
    images.append((
        hex(get_nvs_offset()),
        str(NVS_BIN),
    ))

    command = [
        env.subst("$PYTHONEXE"),
        esptool,
        "--chip",
        board.get("build.mcu", "esp32"),
        "merge_bin",
        "-o",
        str(MERGED_BIN),
        "--flash_mode",
        board.get("build.flash_mode", "dio"),
        "--flash_size",
        board.get("upload.flash_size", "4MB"),
    ]

    for offset, path in images:
        command.extend([
            str(offset) if isinstance(offset, int) else offset,
            path,
        ])

    print("Merging firmware images...")

    subprocess.run(command, check=True)


app_bin = Path(BUILD_DIR) / env.subst("${PROGNAME}.bin")

nvs_target = env.Alias(
    "nvs_dependency",
    env.File(NVS_BIN),
)

merged_target = env.Command(
    target=MERGED_BIN,
    source=[
        str(app_bin),
        str(NVS_BIN),
    ],
    action=merge_firmware,
)

env.Alias("merge_bin", merged_target)
