#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare isolated local-persistence/developer-input HMI configuration copies."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
from zipfile import BadZipFile, ZipFile

from hmi_diagnostic_input import CLASS_NAME, diagnostic_release_jar

EARLY_INPUT_BUNDLES = ("TcpLog", "FwSwDiag", "TcpKeyPanel", "RemoveKeyFilter")
EARLY_DIAGNOSIS_BUNDLES = ("TcpLog", "FwSwDiag")
PRODUCTION_PROPERTY = "-DIS_PRODUCTION_MODE=true"
STARTUP_PROPERTY = "-Dipl.config.dir.hmi_startup=$LSD_DIR"
HOST_GRAPHICS_PROCESSES = {"browser": "browserProc", "navigation": "navigationProc"}
SMARTPHONE_STARTUP_CHANNELS = (
    "Media_SmartphoneIntegrator_Main", "Media_SmartphoneIntegrator_Base",
    "Media_SmartphoneIntegrator_ServiceManagement",
    "Media_SmartphoneIntegrator_LifeCycle", "Media_SmartphoneIntegrator_DSI",
    "Media_SmartphoneIntegrator_Configuration",
    "Media_Base_Persistency", "Media_Base_USB", "Media_Base_USB_ClassInfo",
    "Media_Base_USB_ProductInfo", "Media_Base_OOC_ApplicationImpl",
)
INPUT_ROUTING_CHANNELS = {
    "hidcontroller": ("HID_Tracing_Main",),
    "hmi": ("hmi_Fw_Kbd_HMI", "hmi_Fw_Kbd_Input",
            "hmi_Fw_Widgets_Event_Touch", "hmi_Fw_Widgets_PriorityEventDispatcher",
            "hmi_App_TerminalMode_DSI", "hmi_App_TerminalMode_HMI",
            "hmi_App_TerminalMode_Main"),
}
HID_DISPLAY_GATE_PATH = "/tmp/mh2p-hid-after-display"
HID_DISPLAY_GATE_SCRIPT = """#!/bin/sh
if ! waitfor /dev/name/local/ipte-dm 18; then
    echo "MH2P_HID_DISPLAY_GATE: DisplayManager endpoint unavailable" >&2
    exit 1
fi
echo "MH2P_HID_DISPLAY_GATE: original DisplayManager endpoint ready" >&2
exec /run/enforcer/eso/bin/apps/hidcontroller "$@"
"""


def original_json(source):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate original JSON entry: {key}")
            result[key] = value
        return result

    # Original production JSON uses full-line hash comments.
    return json.loads("\n".join(
        line for line in source.splitlines()
        if not line.lstrip().startswith("#")), object_pairs_hook=unique_object)


def local_persistence(source):
    config = original_json(source)
    if not isinstance(config, dict) or not isinstance(
            config.get("PersistenceA"), dict):
        raise ValueError("Expected PersistenceA configuration")
    persistence = config["PersistenceA"]
    backends = persistence.get("backends")
    if (not isinstance(backends, dict) or
            not isinstance(backends.get("database"), list) or
            backends["database"][:1] != ["SQLite"] or
            len(backends["database"]) < 2 or
            not isinstance(backends["database"][1], str) or
            not backends["database"][1]):
        raise ValueError("Expected the original local SQLite backend")
    mapping = persistence.get("mapping")
    if not isinstance(mapping, list) or not all(
            isinstance(item, dict) for item in mapping):
        raise ValueError("Expected persistence namespace mappings")
    selected = [item for item in mapping
                if item.get("backend") == "rccpersistencebackend"]
    if len(selected) != 1 or not isinstance(
            selected[0].get("partitions"), list) or (
                678364556 not in selected[0]["partitions"]):
        raise ValueError("Expected one original RCC initial-profile mapping")
    selected[0]["backend"] = "database"
    return config


def host_graphics(source, apps):
    config = original_json(source)
    if (not isinstance(config, dict) or
            not isinstance(config.get("procs"), list) or
            not all(isinstance(proc, dict) and isinstance(proc.get("name"), str)
                    for proc in config["procs"])):
        raise ValueError("Expected original framework process list")
    if (not apps or len(set(apps)) != len(apps) or
            any(app not in HOST_GRAPHICS_PROCESSES for app in apps)):
        raise ValueError("Expected distinct browser/navigation graphics apps")
    for app in apps:
        matches = [proc for proc in config["procs"]
                   if proc["name"] == HOST_GRAPHICS_PROCESSES[app]]
        if len(matches) != 1 or matches[0].get("node") != "mmx":
            raise ValueError(f"Expected one original MMX {app} process")
        environment = matches[0].get("environment")
        if (not isinstance(environment, dict) or
                not all(isinstance(value, str) for value in environment.values()) or
                any(key in environment for key in (
                    "GLBRIDGE_W", "GLBRIDGE_H", "GLBRIDGE_SHARED_TRANSPORT"))):
            raise ValueError(f"Unexpected original {app} graphics environment")
        expected = "/navigation/libtcmalloc_minimal.so" if app == "navigation" else None
        if environment.get("LD_PRELOAD") != expected:
            raise ValueError(f"Unexpected original {app} preload")
        environment["LD_PRELOAD"] = (
            "/lib/libmh2p-screen.so" + (":" + expected if expected else ""))
        environment.update(GLBRIDGE_W="1920", GLBRIDGE_H="720",
                           GLBRIDGE_SHARED_TRANSPORT="1")
    return config


def development_service_startup(manager_source, startup_source, apps):
    manager = original_json(manager_source)
    startup = original_json(startup_source)
    if not isinstance(manager, dict) or manager.get("startup") != []:
        raise ValueError("Expected an empty original service-manager startup")
    if not isinstance(startup, dict) or not isinstance(
            startup.get("on_startup_normal"), list):
        raise ValueError("Expected original normal-startup jobs")
    if not apps or any(not isinstance(app, str) or not app for app in apps):
        raise ValueError("Expected explicit original startup application names")
    if len(set(apps)) != len(apps):
        raise ValueError("Duplicate startup application")
    selected = set(apps)
    lifecycle = {app: [] for app in apps}

    def select_jobs(jobs):
        if not isinstance(jobs, list) or not all(
                isinstance(job, dict) and isinstance(job.get("cmd"), str)
                for job in jobs):
            raise ValueError("Invalid original startup jobs")
        result = []
        found = False
        for job in jobs:
            if "app" in job and not isinstance(job["app"], str):
                raise ValueError("Invalid original startup application")
            if job["cmd"] == "fork":
                commands = select_jobs(job.get("commands"))
                if commands:
                    result.append(dict(copy.deepcopy(job), commands=commands))
                    found = True
            elif job.get("app") in selected:
                if job["cmd"] not in ("init", "run", "set_app_runmode"):
                    raise ValueError("Unsupported selected application job")
                result.append(copy.deepcopy(job))
                found = True
                if job["cmd"] in ("init", "run"):
                    lifecycle[job["app"]].append(job["cmd"])
            elif job["cmd"] == "sleep":
                result.append(copy.deepcopy(job))
        return result if found else []

    manager["startup"] = select_jobs(startup["on_startup_normal"])
    if any(commands != ["init", "run"] for commands in lifecycle.values()):
        raise ValueError("Expected one original init/run pair per startup app")
    return manager


def hid_display_gate(source):
    config = original_json(source)
    if not isinstance(config, dict) or not isinstance(config.get("procs"), list):
        raise ValueError("Expected original framework process list")
    matches = [proc for proc in config["procs"]
               if isinstance(proc, dict) and proc.get("name") == "hidcontroller"]
    if (len(matches) != 1 or matches[0].get("node") != "mmx" or
            matches[0].get("exec") != "/run/enforcer/eso/bin/apps/hidcontroller" or
            matches[0].get("id") != 366):
        raise ValueError("Expected one unchanged original MMX HID executable")
    matches[0]["exec"] = HID_DISPLAY_GATE_PATH
    return config


def hid_display_startup(source):
    config = original_json(source)
    if not isinstance(config, dict):
        raise ValueError("Expected original service-manager configuration")
    selected = []

    def visit(jobs):
        if not isinstance(jobs, list) or not all(
                isinstance(job, dict) and isinstance(job.get("cmd"), str)
                for job in jobs):
            raise ValueError("Expected original service-manager startup jobs")
        for job in jobs:
            if job["cmd"] == "fork":
                visit(job.get("commands"))
            elif job.get("app") == "hidcontroller":
                selected.append(job)

    visit(config.get("startup"))
    expected = [
        {"cmd": "init", "app": "hidcontroller", "apprunmode": 0,
         "ps_access": "PS_ACC_READ_WRITE", "timeout": 20000, "blocking": False},
        {"cmd": "run", "app": "hidcontroller", "timeout": 20000, "blocking": False},
    ]
    if selected != expected:
        raise ValueError("Expected one original nonblocking HID init/run pair")
    selected[0]["blocking"] = True
    return config


def startup_debug_tracing(source):
    config = original_json(source)
    try:
        clients = config["client"]
        console = config["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]
        channels = {
            name: clients[name]["levels"]["channel"]
            for name in ("servicemgr", "connectivity_launcher", "honcho",
                         "navigationProc")
        }
    except (KeyError, TypeError) as exc:
        raise ValueError("Expected original startup tracing configuration") from exc
    for levels in [console, *channels.values()]:
        if (not isinstance(levels, dict) or not levels or
                not all(isinstance(key, str) and isinstance(value, str)
                        for key, value in levels.items())):
            raise ValueError("Expected original startup trace channel levels")
    for channel in list(channels["servicemgr"]):
        if channel.startswith(("MIB_SM", "FW_SM")):
            channels["servicemgr"][channel] = "debug"
    channels["servicemgr"].update(MIB_SM="debug", FW_SM="debug")
    channels["connectivity_launcher"]["CON_LAUNCHER"] = "debug"
    channels["honcho"]["SYS"] = "debug"
    for channel in list(channels["navigationProc"]):
        if channel.startswith("HERE_"):
            channels["navigationProc"][channel] = "info"
    console.update(MIB_SM="debug", FW_SM="debug", CON_LAUNCHER="debug",
                   SYS="debug", HERE="info")
    return config


def telephone_debug_tracing(source):
    config = original_json(source)
    selected = {
        "bluetoothProc": ("CON_BT",),
        "connectivityProc": ("CON_BTSTACK",),
        "telephoneProc": ("CON_TEL",),
        "connectionmanager": ("CON_CONMAN",),
    }
    try:
        console = config["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]
        channels = {name: config["client"][name]["levels"]["channel"]
                    for name in selected}
    except (KeyError, TypeError) as exc:
        raise ValueError("Expected original telephone tracing configuration") from exc
    for levels in [console, *channels.values()]:
        if (not isinstance(levels, dict) or not levels or
                not all(isinstance(key, str) and isinstance(value, str)
                        for key, value in levels.items())):
            raise ValueError("Expected original telephone trace channel levels")
    for name, prefixes in selected.items():
        for prefix in prefixes:
            for channel in list(channels[name]):
                if channel == prefix or channel.startswith(prefix + "_"):
                    if "HCICAPTURE" not in channel and channels[name][channel] != "trace":
                        channels[name][channel] = "debug"
            channels[name][prefix] = "debug"
            console[prefix] = "debug"
    console["CON_BTSTACK_HCICAPTURE"] = "off"
    return config


def smartphone_debug_tracing(source):
    config = original_json(source)
    try:
        client = config["client"]["smartphone_integrator"]["levels"]["channel"]
        console = config["server"]["default"]["backends"]["ConsoleBackend"]["levels"]["channel"]
    except (KeyError, TypeError) as exc:
        raise ValueError("Expected original smartphone tracing configuration") from exc
    for levels in (client, console):
        if (not isinstance(levels, dict) or not levels or
                not all(isinstance(key, str) and isinstance(value, str)
                        for key, value in levels.items())):
            raise ValueError("Expected original smartphone trace channel levels")
        for channel in SMARTPHONE_STARTUP_CHANNELS:
            levels[channel] = "debug"
    return config


def input_debug_tracing(source):
    config = original_json(source)
    try:
        console = config["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]
        clients = {name: config["client"][name]["levels"]["channel"]
                   for name in INPUT_ROUTING_CHANNELS}
    except (KeyError, TypeError) as exc:
        raise ValueError("Expected original input tracing configuration") from exc
    for levels in (console, *clients.values()):
        if (not isinstance(levels, dict) or not levels or
                not all(isinstance(key, str) and isinstance(value, str)
                        for key, value in levels.items())):
            raise ValueError("Expected original input trace channel levels")
    for name, channels in INPUT_ROUTING_CHANNELS.items():
        for channel in channels:
            clients[name][channel] = "trace"
            console[channel] = "trace"
    return config


def early_input_startup(source, diagnosis_only=False):
    config = json.loads(source)
    if not isinstance(config, dict) or not isinstance(
            config.get("components"), list):
        raise ValueError("Expected original HMI startup components")
    components = config["components"]
    if not all(isinstance(item, dict) and isinstance(item.get("name"), str)
               for item in components):
        raise ValueError("Invalid HMI startup component")
    by_name = {item["name"]: item for item in components}
    if len(by_name) != len(components):
        raise ValueError("Duplicate HMI startup component")
    for name in ("StaticInit", "Addon"):
        if name not in by_name or not isinstance(
                by_name[name].get("bundles"), list) or not all(
                    isinstance(item, dict) and isinstance(item.get("name"), str)
                    for item in by_name[name]["bundles"]):
            raise ValueError(f"Expected original {name} bundle list")
    static = by_name["StaticInit"]["bundles"]
    addon = by_name["Addon"]["bundles"]
    if (not static or static[-1].get("name") != "EAL" or
            any(item["name"] in EARLY_INPUT_BUNDLES for item in static)):
        raise ValueError("Expected unchanged StaticInit ending with EAL")
    moved = {}
    selected = EARLY_DIAGNOSIS_BUNDLES if diagnosis_only else EARLY_INPUT_BUNDLES
    for name in selected:
        matches = [item for item in addon if item["name"] == name]
        if len(matches) != (0 if name == "FwSwDiag" else 1):
            raise ValueError(f"Unexpected original addon bundle: {name}")
        moved[name] = copy.deepcopy(matches[0]) if matches else {"name": name}
    by_name["Addon"]["bundles"] = [
        item for item in addon if item["name"] not in selected]
    static.extend(moved[name] for name in selected)
    return config


def developer_launcher(source, early_input=False, diagnostic_release_zero=False):
    if source.count(PRODUCTION_PROPERTY) != 1:
        raise ValueError("Expected exactly one original production JVM property")
    result = source.replace(PRODUCTION_PROPERTY, "-DIS_PRODUCTION_MODE=false")
    if early_input:
        if source.count(STARTUP_PROPERTY) != 1:
            raise ValueError("Expected exactly one original startup path property")
        result = result.replace(STARTUP_PROPERTY,
                                "-Dipl.config.dir.hmi_startup=/tmp")
    if diagnostic_release_zero:
        original = 'BOOTCLASSPATH="$BOOTCLASSPATH:$LSD_JXE"'
        if source.count(original) != 1:
            raise ValueError("Expected one original firmware boot classpath")
        result = result.replace(
            original, 'BOOTCLASSPATH="/tmp/mh2p-diagnostic-release.jar:'
            '$BOOTCLASSPATH:$LSD_JXE"')
    return result


def prepare(config_dir, output, *, launcher=None, lsd_jar=None,
            developer_input=False, early_input=False, startup_apps=(),
            diagnostic_release_zero=False, host_graphics_apps=(),
            startup_tracing=False, telephone_tracing=False, early_diagnosis=False):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    if early_input and early_diagnosis:
        raise ValueError("Choose early input or diagnosis-only startup, not both")
    if (early_input or early_diagnosis) and not developer_input:
        raise ValueError("Early startup requires explicit developer input")
    if diagnostic_release_zero and not developer_input:
        raise ValueError("Diagnostic release correction requires developer input")
    if developer_input != (launcher is not None):
        raise ValueError("Developer input requires --launcher, and vice versa")
    if (early_input or early_diagnosis or diagnostic_release_zero) != (lsd_jar is not None):
        raise ValueError("Early startup or diagnostic release correction requires "
                         "--lsd-jar, and vice versa")
    original = {path.name: path.read_bytes()
                for path in sorted(config_dir.glob("*.json"))}
    if "persistence.json" not in original:
        raise ValueError("Production config directory has no persistence.json")
    payloads = dict(original)
    payloads["persistence.json"] = (
        json.dumps(local_persistence(original["persistence.json"].decode()),
                   indent=2) + "\n").encode()
    if startup_tracing:
        if "tracing.json" not in original:
            raise ValueError("Startup tracing requires original tracing.json")
        payloads["tracing.json"] = (
            json.dumps(startup_debug_tracing(original["tracing.json"].decode()),
                       indent=2) + "\n").encode()
    if telephone_tracing:
        if "tracing.json" not in original:
            raise ValueError("Telephone tracing requires original tracing.json")
        payloads["tracing.json"] = (
            json.dumps(telephone_debug_tracing(payloads["tracing.json"].decode()),
                       indent=2) + "\n").encode()
    if host_graphics_apps:
        if "framework.json" not in original:
            raise ValueError("Host graphics requires original framework.json")
        payloads["framework.json"] = (
            json.dumps(host_graphics(original["framework.json"].decode(),
                                     host_graphics_apps), indent=2) + "\n").encode()
    if startup_apps:
        if not {"servicemgr.json", "dsistartup.json"} <= original.keys():
            raise ValueError("Startup apps require original manager/DSI configs")
        payloads["servicemgr.json"] = (
            json.dumps(development_service_startup(
                original["servicemgr.json"].decode(),
                original["dsistartup.json"].decode(), startup_apps),
                indent=2) + "\n").encode()
    sources = {str((config_dir / name).resolve()): data
               for name, data in original.items()}
    if developer_input:
        data = launcher.read_bytes()
        sources[str(launcher.resolve())] = data
        payloads["lsd-development.sh"] = developer_launcher(
            data.decode(), early_input or early_diagnosis,
            diagnostic_release_zero).encode()
    if early_input or early_diagnosis or diagnostic_release_zero:
        archive = lsd_jar.read_bytes()
        sources[str(lsd_jar.resolve())] = archive
        with ZipFile(lsd_jar) as jar:
            if early_input or early_diagnosis:
                startup = jar.read("resources/hmi_startup.json")
                payloads["hmi_startup.json"] = (
                    json.dumps(early_input_startup(
                        startup, diagnosis_only=early_diagnosis),
                        indent=2)+"\n").encode()
            if diagnostic_release_zero:
                payloads["mh2p-diagnostic-release.jar"] = diagnostic_release_jar(
                    jar.read(CLASS_NAME))
    manifest = {
        "mode": "development-only",
        "local_persistence": True,
        "developer_input": developer_input,
        "diagnostic_release_zero_fingers": diagnostic_release_zero,
        "diagnostic_class_override": (
            CLASS_NAME if diagnostic_release_zero else None),
        "normal_native_input_modified": False,
        "early_input_bundles": list(EARLY_INPUT_BUNDLES) if early_input else [],
        "early_diagnosis_bundles": (
            list(EARLY_DIAGNOSIS_BUNDLES) if early_diagnosis else []),
        "startup_apps": list(startup_apps),
        "startup_tracing": startup_tracing,
        "telephone_tracing": telephone_tracing,
        "host_graphics_apps": list(host_graphics_apps),
        "host_graphics_transport": (
            "fixed-shared-objects" if host_graphics_apps else None),
        "guest_config_dir": "/tmp",
        "source_sha256": {
            path: hashlib.sha256(data).hexdigest()
            for path, data in sources.items()},
        "output_sha256": {
            name: hashlib.sha256(data).hexdigest()
            for name, data in payloads.items()},
        "original_files_modified": False,
        "factory_data_supplied": False,
        "native_rcc_persistence_verified": False,
        "native_touch_verified": False,
        "hmi_readiness_verified": False,
    }
    if "development-mode.json" in payloads:
        raise ValueError("Source configuration conflicts with development manifest")
    output.mkdir(parents=True)
    for name, data in payloads.items():
        (output / name).write_bytes(data)
    (output / "development-mode.json").write_text(
        json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-dir", type=Path, required=True,
                        help="Locally extracted original production JSON directory")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--developer-input", action="store_true",
                        help="Enable original developer services in a launcher copy")
    parser.add_argument("--launcher", type=Path,
                        help="Locally extracted original lsd.sh")
    parser.add_argument("--early-input", action="store_true",
                        help="Move diagnostic input bundles after StaticInit's EAL")
    parser.add_argument("--early-diagnosis", action="store_true",
                        help="Start only original TcpLog/FwSwDiag after EAL; "
                             "leave keypanel/filter startup unchanged")
    parser.add_argument("--lsd-jar", type=Path,
                        help="Original HMI class/resource archive (lsd.jar or ZIP JXE)")
    parser.add_argument("--diagnostic-release-zero", action="store_true",
                        help="Opt in to a single-class diagnostic override reporting "
                             "zero remaining fingers on release; requires developer input")
    parser.add_argument("--startup-app", action="append", default=[],
                        help="Opt in to an original normal-startup app via the "
                             "service manager; repeat for each app")
    parser.add_argument("--startup-tracing", action="store_true",
                        help="Expose original service-manager, connectivity, "
                             "mount and navigation startup diagnostics")
    parser.add_argument("--telephone-tracing", action="store_true",
                        help="Expose original Bluetooth/telephone availability and "
                             "coding diagnostics without enabling HCI payload capture")
    parser.add_argument("--host-graphics-app", action="append", default=[],
                        choices=tuple(HOST_GRAPHICS_PROCESSES),
                        help="Route a copied original app environment through "
                             "the shared host bridge; requires privileged fixed-"
                             "region setup before application startup")
    args = parser.parse_args()
    try:
        result = prepare(
            args.config_dir, args.output, launcher=args.launcher,
            lsd_jar=args.lsd_jar, developer_input=args.developer_input,
            early_input=args.early_input, startup_apps=args.startup_app,
            diagnostic_release_zero=args.diagnostic_release_zero,
            host_graphics_apps=args.host_graphics_app,
            startup_tracing=args.startup_tracing,
            telephone_tracing=args.telephone_tracing,
            early_diagnosis=args.early_diagnosis)
    except (OSError, ValueError, KeyError, BadZipFile) as exc:
        parser.exit(1, f"MH2P HMI development preparation failed: {exc}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
