
Import("env")

import subprocess
from pathlib import Path

BUILD_DIR = env.subst("$BUILD_DIR")
PROJECT_DIR = env.subst("$PROJECT_DIR")

MERGED_BIN = Path(BUILD_DIR) / "firmware.merged.bin"
NVS_BIN = Path(BUILD_DIR) / "nvs_data.bin"

# nvs-gen.py (loaded as a pre script) exports the CSV it generates from.
CSV_FILE = env.get("CSV_FILE", str(Path(PROJECT_DIR) / "nvs_data.csv"))


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

    # The NVS image is a source of this target, so SCons builds it (through
    # scripts/nvs-gen.py) before running this action. Fail loudly instead of
    # merging a stale/missing image if that dependency ever gets detached.
    if not NVS_BIN.is_file():
        raise RuntimeError(
            f"{NVS_BIN} is missing: merge_bin needs scripts/nvs-gen.py "
            "registered as a pre script so the image is generated from "
            f"{CSV_FILE}"
        )

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

merged_target = env.Command(
    target=str(MERGED_BIN),
    source=[
        str(app_bin),
        # NVS image: the node already carries the builder from nvs-gen.py, so
        # a changed nvs_data.csv regenerates the image before the merge.
        env.File(str(NVS_BIN)),
        # Depend on the CSV directly as well: any edit re-merges the image,
        # even when the generated NVS bytes happen to stay identical.
        env.File(CSV_FILE),
    ],
    action=merge_firmware,
)

env.Alias("merge_bin", merged_target)
