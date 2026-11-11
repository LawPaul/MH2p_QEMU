#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare an opt-in CarPlay contact-limit configuration, never a live install."""
import argparse
import hashlib
import json
from pathlib import Path

from prepare_hmi_development import original_json


GUEST_CONFIG_DIRECTORY = "/tmp"


def carplay_launcher(source):
    config = original_json(source)
    if not isinstance(config, dict) or not isinstance(config.get("children"), dict):
        raise ValueError("Expected original smartphone_integrator children configuration")
    carplay = config["children"].get("carplay")
    if not isinstance(carplay, dict) or carplay.get("exec") != "dio_manager":
        raise ValueError("Expected original CarPlay dio_manager launch entry")
    envs = carplay.get("envs")
    old = "IPL_CONFIG_DIR_DIO_MANAGER=/etc/eso/production"
    if (not isinstance(envs, list) or not all(isinstance(item, str) for item in envs) or
            [item for item in envs if item.startswith("IPL_CONFIG_DIR_DIO_MANAGER=")] != [old]):
        raise ValueError("Expected one original CarPlay configuration directory")
    envs[envs.index(old)] = f"IPL_CONFIG_DIR_DIO_MANAGER={GUEST_CONFIG_DIRECTORY}"
    return config


def multitouch_config(source, fingers=2):
    if type(fingers) is not int or not 2 <= fingers <= 4:
        raise ValueError("CarPlay contact limit must be an integer from 2 through 4")
    config = original_json(source)
    if not isinstance(config, dict) or not isinstance(config.get("screen"), dict):
        raise ValueError("Expected original dio_manager screen configuration")
    old = config["screen"].get("fingers")
    if type(old) is not int or not 1 <= old <= 4:
        raise ValueError("Expected an original integer screen.fingers limit from 1 through 4")
    config["screen"]["fingers"] = fingers
    return config, old


def prepare(source, output, fingers=2, smartphone_source=None):
    raw = source.read_bytes()
    config, old = multitouch_config(raw.decode("utf-8"), fingers)
    modified = (json.dumps(config, indent=4) + "\n").encode("utf-8")
    launcher_raw = smartphone_source.read_bytes() if smartphone_source else None
    launcher = (carplay_launcher(launcher_raw.decode("utf-8"))
                if launcher_raw is not None else None)
    metadata = {
        "mod": "carplay-screen-contact-limit",
        "source": str(source.resolve()),
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "output_sha256": hashlib.sha256(modified).hexdigest(),
        "change": {"key": "screen.fingers", "before": old, "after": fingers},
        "configuration_only": True,
        "installed": False,
        "coding_or_licenses_changed": False,
        "hmi_delivery_verified": False,
        "iphone_session_verified": False,
        "guest_config_directory": GUEST_CONFIG_DIRECTORY if launcher is not None else None,
        "note": (
            "The original native descriptor and report builders share this limit. "
            "Restart the original projection service before negotiating a new "
            "phone session. This does not start the service, provision the unit, "
            "or establish native HMI contact delivery."),
    }
    if launcher is not None:
        launcher_output = (json.dumps(launcher, indent=4) + "\n").encode("utf-8")
        metadata["smartphone_launcher"] = {
            "source_sha256": hashlib.sha256(launcher_raw).hexdigest(),
            "output_sha256": hashlib.sha256(launcher_output).hexdigest(),
            "change": "children.carplay.envs IPL_CONFIG_DIR_DIO_MANAGER only",
        }
    output.mkdir(parents=True)
    with (output / "dio_manager.json").open("xb") as stream:
        stream.write(modified)
    if launcher is not None:
        with (output / "smartphone_integrator.json").open("xb") as stream:
            stream.write(launcher_output)
    with (output / "manifest.json").open("x") as stream:
        json.dump(metadata, stream, indent=2)
        stream.write("\n")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True,
                        help="Original dio_manager.json, including hash comments")
    parser.add_argument("--output", type=Path, required=True,
                        help="New isolated output directory; must not exist")
    parser.add_argument("--fingers", type=int, choices=(2, 3, 4), default=2)
    parser.add_argument("--smartphone-config", type=Path,
                        help="Also prepare the original backend launch environment")
    args = parser.parse_args()
    try:
        metadata = prepare(args.config, args.output, args.fingers, args.smartphone_config)
    except (OSError, UnicodeError, ValueError) as exc:
        parser.exit(1, f"CarPlay multi-touch preparation failed: {exc}\n")
    print(json.dumps(metadata, indent=2))


if __name__ == "__main__":
    main()
