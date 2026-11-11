# SPDX-License-Identifier: GPL-2.0-or-later
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

import prepare_hmi_development as development
from hmi_diagnostic_input import CLASS_NAME
from test_hmi_diagnostic_input import synthetic_gateway


def persistence_config():
    return {"PersistenceA": {
        "backends": {"database": ["SQLite", "/original/database"],
                     "rccpersistencebackend": ["asi.persistence", 12]},
        "default": "database",
        "mapping": [
            {"partitions": [24354, 678364556],
             "backend": "rccpersistencebackend"},
            {"partitions": [1], "backend": "rcc"}]}}


class InputTracingTests(unittest.TestCase):
    def config(self):
        return {
            "client": {
                name: {"backends": {"original": {"enabled": True}},
                       "levels": {"channel": {"*": "warning", "unrelated": "info"}}}
                for name in (*development.INPUT_ROUTING_CHANNELS, "unrelated")},
            "server": {"default": {"backends": {"ConsoleBackend": {
                "levels": {"channel": {"*": "warning", "Clients": "info"}}}}}}}

    def test_only_selected_channels_change(self):
        source = self.config()
        expected = copy.deepcopy(source)
        console = expected["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]
        for name, channels in development.INPUT_ROUTING_CHANNELS.items():
            for channel in channels:
                expected["client"][name]["levels"]["channel"][channel] = "trace"
                console[channel] = "trace"
        result = development.input_debug_tracing(json.dumps(source))
        self.assertEqual(result, expected)
        self.assertEqual(development.input_debug_tracing(json.dumps(result)), result)
        self.assertNotEqual(source, result)

    def test_terminalmode_raw_and_dsi_channels_are_enabled_without_policy_changes(self):
        source = self.config()
        source["client"]["hmi"]["levels"]["channel"].update({
            "hmi_App_TerminalMode_DSI": "debug",
            "hmi_App_TerminalMode_HMI": "debug",
            "hmi_App_TerminalMode_Main": "debug",
            "hmi_App_TerminalMode_Audio": "debug",
        })
        result = development.input_debug_tracing(json.dumps(source))
        for channel in ("hmi_App_TerminalMode_DSI", "hmi_App_TerminalMode_HMI",
                        "hmi_App_TerminalMode_Main"):
            self.assertEqual(result["client"]["hmi"]["levels"]["channel"][channel],
                             "trace")
            self.assertEqual(
                result["server"]["default"]["backends"]["ConsoleBackend"][
                    "levels"]["channel"][channel], "trace")
        self.assertEqual(
            result["client"]["hmi"]["levels"]["channel"]["hmi_App_TerminalMode_Audio"],
            "debug")
        self.assertEqual(result["client"]["hmi"]["backends"],
                         source["client"]["hmi"]["backends"])

    def test_missing_and_malformed_channels_rejected(self):
        for name in development.INPUT_ROUTING_CHANNELS:
            for value in (None, [], {}, {"*": None}):
                source = self.config()
                source["client"][name]["levels"]["channel"] = value
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    development.input_debug_tracing(json.dumps(source))
        with self.assertRaises(ValueError):
            development.input_debug_tracing("{}")

    def test_console_must_be_valid_and_no_duplicate_keys(self):
        for value in (None, [], {}, {"*": 1}):
            source = self.config()
            source["server"]["default"]["backends"]["ConsoleBackend"]["levels"][
                "channel"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                development.input_debug_tracing(json.dumps(source))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            development.input_debug_tracing('{"client": {}, "client": {}}')


class SmartphoneTracingTests(unittest.TestCase):
    def test_only_selected_client_and_console_channels_change(self):
        source = {
            "client": {
               "smartphone_integrator": {
                   "backends": {"original": {"enabled": True}},
                   "levels": {"channel": {"*": "warning", "SmartphoneIntegrator": "info"}}},
               "unrelated": {"levels": {"channel": {"Media": "info"}}}},
            "server": {"default": {"backends": {"ConsoleBackend": {
               "levels": {"channel": {"*": "warning", "Clients": "info"}}}}}}}
        expected = copy.deepcopy(source)
        for levels in (
               expected["client"]["smartphone_integrator"]["levels"]["channel"],
               expected["server"]["default"]["backends"]["ConsoleBackend"]["levels"]["channel"]):
            levels.update({channel: "debug" for channel in development.SMARTPHONE_STARTUP_CHANNELS})
        observed = development.smartphone_debug_tracing(json.dumps(source))
        self.assertEqual(observed, expected)
        self.assertEqual(development.smartphone_debug_tracing(json.dumps(observed)), observed)
        self.assertIn("Media_SmartphoneIntegrator_Configuration",
                      development.SMARTPHONE_STARTUP_CHANNELS)
        self.assertFalse(any("HCI" in channel for channel in development.SMARTPHONE_STARTUP_CHANNELS))

    def test_unknown_or_malformed_original_configuration_rejected(self):
        for source in ({}, {"client": None}, {
               "client": {"smartphone_integrator": {"levels": {"channel": []}}},
               "server": {"default": {"backends": {"ConsoleBackend": {
                   "levels": {"channel": {"*": "warning"}}}}}}}):
            with self.subTest(source=source), self.assertRaises(ValueError):
               development.smartphone_debug_tracing(json.dumps(source))


def startup_config():
    return {"build_configuration": "test", "components": [
        {"name": "StaticInit", "bundles": [{"name": "InitSystem"},
                                         {"name": "EAL"}]},
        {"name": "Addon", "bundles": [
            {"name": "PostStartup"}, {"name": "TcpLog", "setting": 1},
            {"name": "TcpKeyPanel"}, {"name": "RemoveKeyFilter"}]},
        {"name": "Other", "dependencies": [{"id": 9}]}]}


def framework_config():
    return {"nodes": ["unchanged"], "procs": [
        {"name": "browserProc", "node": "mmx", "exec": "/original/browser",
         "environment": {"LD_LIBRARY_PATH": "/original/webkit:${LD_LIBRARY_PATH}",
                         "CUSTOMER_UPDATE_FLAG_CHECK_SWDL_STATE": "/original/flag"}},
        {"name": "navigationProc", "node": "mmx", "exec": "/original/navStartup",
         "environment": {"LD_PRELOAD": "/navigation/libtcmalloc_minimal.so",
                         "NAV_MAP_PATHS": "/original/maps"}},
        {"name": "other", "node": "rcc", "environment": {"unchanged": "1"}}]}


def tracing_config():
    return {
        "client": {
            "servicemgr": {
                "backends": {"RemoteConnectBackend": {"enabled": True}},
                "levels": {"channel": {
                    "*": "warning", "MIB_SM_DomainStarter": "info",
                    "FW_SM_Service": "error", "OTHER": "error"}}},
            "connectivity_launcher": {
                "levels": {"channel": {"*": "warning", "CON_LAUNCHER": "debug"}}},
            "honcho": {"levels": {"channel": {"SYS": "debug"}}},
            "navigationProc": {
                "levels": {"channel": {"*": "warning", "HERE_RENDER": "info"}}},
            "bluetoothProc": {"levels": {"channel": {
                "*": "warning", "CON_BT": "info"}}},
            "connectivityProc": {"levels": {"channel": {
                "CON_BTSTACK": "info", "CON_BTSTACK_IA_SAP": "trace",
                "CON_BTSTACK_HCICAPTURE": "off"}}},
            "telephoneProc": {"levels": {"channel": {
                "CON_TEL": "info", "CON_TEL_CONFIG": "warning"}}},
            "connectionmanager": {"levels": {"channel": {"CON_CONMAN": "info"}}},
            "other": {"enabled": False}},
        "server": {"default": {"backends": {
            "ConsoleBackend": {"levels": {
                "channel": {"*": "off", "MIB_SM": "error", "FW": "error"},
                "probe": {"*": "error"}}},
            "FileBackend": {"enabled": False}}}}}


class HmiDevelopmentTests(unittest.TestCase):
    def test_original_json_rejects_duplicate_entries_at_any_depth(self):
        for source in ('{"channel": 1, "channel": 2}',
                       '{"levels": {"CON_TEL": "debug", "CON_TEL": "info"}}'):
            with self.subTest(source=source), self.assertRaisesRegex(
                    ValueError, "Duplicate original JSON entry"):
                development.original_json(source)

    def test_telephone_tracing_is_targeted_and_preserves_capture_level(self):
        source = tracing_config()
        expected = copy.deepcopy(source)
        for name, prefix in (
                ("bluetoothProc", "CON_BT"), ("connectivityProc", "CON_BTSTACK"),
                ("telephoneProc", "CON_TEL"), ("connectionmanager", "CON_CONMAN")):
            expected["client"][name]["levels"]["channel"][prefix] = "debug"
            expected["server"]["default"]["backends"]["ConsoleBackend"][
                "levels"]["channel"][prefix] = "debug"
        expected["client"]["telephoneProc"]["levels"]["channel"][
            "CON_TEL_CONFIG"] = "debug"
        expected["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]["CON_BTSTACK_HCICAPTURE"] = "off"
        result = development.telephone_debug_tracing(json.dumps(source))
        self.assertEqual(result, expected)
        self.assertEqual(source, tracing_config())
        self.assertEqual(development.telephone_debug_tracing(json.dumps(result)), result)

    def test_telephone_tracing_disables_console_hci_capture(self):
        source = tracing_config()
        source["client"]["connectivityProc"]["levels"]["channel"][
            "CON_BTSTACK_HCICAPTURE"] = "trace"
        source["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]["CON_BTSTACK_HCICAPTURE"] = "trace"
        result = development.telephone_debug_tracing(json.dumps(source))
        self.assertEqual(result["client"]["connectivityProc"]["levels"]["channel"][
            "CON_BTSTACK_HCICAPTURE"], "trace")
        self.assertEqual(result["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"]["CON_BTSTACK_HCICAPTURE"], "off")

    def test_telephone_tracing_rejects_malformed_sources(self):
        for source in (None, [], {}, {"client": None}):
            with self.subTest(source=source), self.assertRaises(ValueError):
                development.telephone_debug_tracing(json.dumps(source))
        for name in ("bluetoothProc", "connectivityProc", "telephoneProc",
                     "connectionmanager"):
            for levels in (None, {}, [], {"*": 3}):
                source = tracing_config()
                source["client"][name]["levels"]["channel"] = levels
                with self.subTest(name=name, levels=levels), self.assertRaises(ValueError):
                    development.telephone_debug_tracing(json.dumps(source))
            del source["client"][name]
            with self.assertRaises(ValueError):
                development.telephone_debug_tracing(json.dumps(source))
        for levels in (None, {}, [], {"*": 3}):
            source = tracing_config()
            source["server"]["default"]["backends"]["ConsoleBackend"][
                "levels"]["channel"] = levels
            with self.subTest(console=levels), self.assertRaises(ValueError):
                development.telephone_debug_tracing(json.dumps(source))

    def test_telephone_tracing_preparation_and_composition(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "persistence.json").write_text(json.dumps(persistence_config()))
            with self.assertRaises(ValueError):
                development.prepare(root, root / "missing", telephone_tracing=True)
            self.assertFalse((root / "missing").exists())
            original = json.dumps(tracing_config())
            (root / "tracing.json").write_text(original)
            manifest = development.prepare(
                root, root / "debug", startup_tracing=True, telephone_tracing=True)
            self.assertTrue(manifest["telephone_tracing"])
            self.assertTrue(manifest["startup_tracing"])
            self.assertEqual((root / "tracing.json").read_text(), original)
            self.assertEqual(json.loads((root / "debug/tracing.json").read_text()),
                             development.telephone_debug_tracing(json.dumps(
                                 development.startup_debug_tracing(original))))

    def test_startup_tracing_changes_only_targeted_channel_levels(self):
        source = tracing_config()
        expected = copy.deepcopy(source)
        expected["client"]["servicemgr"]["levels"]["channel"].update(
            MIB_SM_DomainStarter="debug", FW_SM_Service="debug",
            MIB_SM="debug", FW_SM="debug")
        expected["server"]["default"]["backends"]["ConsoleBackend"][
            "levels"]["channel"].update(
                MIB_SM="debug", FW_SM="debug", CON_LAUNCHER="debug",
                SYS="debug", HERE="info")
        result = development.startup_debug_tracing(
            "# Original\n" + json.dumps(source))
        self.assertEqual(result, expected)
        self.assertEqual(source, tracing_config())
        self.assertEqual(
            development.startup_debug_tracing(json.dumps(result)), result)

    def test_startup_tracing_rejects_missing_or_malformed_original_channels(self):
        invalid = [None, [], {}, {"client": None}]
        for name in ("servicemgr", "connectivity_launcher", "honcho", "navigationProc"):
            config = tracing_config()
            del config["client"][name]
            invalid.append(config)
            for levels in ([], {}, {"*": 1}):
                config = tracing_config()
                config["client"][name]["levels"]["channel"] = levels
                invalid.append(config)
        for levels in (None, [], {}, {"*": 1}):
            config = tracing_config()
            config["server"]["default"]["backends"]["ConsoleBackend"][
                "levels"]["channel"] = levels
            invalid.append(config)
        for config in invalid:
            with self.subTest(config=config), self.assertRaises(ValueError):
                development.startup_debug_tracing(json.dumps(config))

    def test_startup_tracing_preparation_is_explicit_and_source_preserving(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "persistence.json").write_text(json.dumps(persistence_config()))
            with self.assertRaises(ValueError):
                development.prepare(root, root / "missing", startup_tracing=True)
            self.assertFalse((root / "missing").exists())
            original = json.dumps(tracing_config()).encode()
            (root / "tracing.json").write_bytes(original)
            manifest = development.prepare(root, root / "default")
            self.assertFalse(manifest["startup_tracing"])
            self.assertEqual((root / "default/tracing.json").read_bytes(), original)
            manifest = development.prepare(root, root / "debug", startup_tracing=True)
            self.assertTrue(manifest["startup_tracing"])
            self.assertFalse(manifest["hmi_readiness_verified"])
            self.assertEqual((root / "tracing.json").read_bytes(), original)
            self.assertEqual(
                json.loads((root / "debug/tracing.json").read_text()),
                development.startup_debug_tracing(original.decode()))
            self.assertEqual(
                (root / "default/persistence.json").read_bytes(),
                (root / "debug/persistence.json").read_bytes())

    def test_host_graphics_changes_only_selected_process_environments(self):
        source = framework_config()
        expected = copy.deepcopy(source)
        for proc in expected["procs"][:2]:
            env = proc["environment"]
            env["LD_PRELOAD"] = (
                "/lib/libmh2p-screen.so" +
                (":" + env["LD_PRELOAD"] if "LD_PRELOAD" in env else ""))
            env.update(GLBRIDGE_W="1920", GLBRIDGE_H="720",
                       GLBRIDGE_SHARED_TRANSPORT="1")
        self.assertEqual(development.host_graphics(
            "# Original\n" + json.dumps(source), ["navigation", "browser"]),
            expected)
        browser_only = development.host_graphics(json.dumps(source), ["browser"])
        self.assertEqual(browser_only["procs"][1:], source["procs"][1:])
        self.assertNotIn("LD_PRELOAD", source["procs"][0]["environment"])

    def test_host_graphics_rejects_ambiguous_unknown_or_changed_configuration(self):
        original = framework_config()
        bad_configs = [{"procs": {}}, {"procs": [{}]}]
        for change in ("duplicate", "node", "preload", "environment", "bridge"):
            config = copy.deepcopy(original)
            proc = config["procs"][1]
            if change == "duplicate":
                config["procs"].append(copy.deepcopy(proc))
            elif change == "environment":
                proc["environment"] = []
            elif change == "node":
                proc["node"] = "rcc"
            elif change == "preload":
                proc["environment"]["LD_PRELOAD"] = "/unexpected"
            else:
                proc["environment"]["GLBRIDGE_W"] = "100"
            bad_configs.append(config)
        for config in bad_configs:
            with self.subTest(config=config), self.assertRaises(ValueError):
                development.host_graphics(json.dumps(config), ["navigation"])
        for apps in ([], ["unknown"], ["browser", "browser"]):
            with self.subTest(apps=apps), self.assertRaises(ValueError):
                development.host_graphics(json.dumps(original), apps)
        with self.assertRaises(ValueError):
            development.host_graphics(
                json.dumps(development.host_graphics(
                    json.dumps(original), ["browser"])), ["browser"])

    def test_host_graphics_preparation_is_explicit_and_source_preserving(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "persistence.json").write_text(json.dumps(persistence_config()))
            framework = json.dumps(framework_config()).encode()
            (root / "framework.json").write_bytes(framework)
            manifest = development.prepare(root, root / "default")
            self.assertEqual((root / "default/framework.json").read_bytes(),
                             framework)
            self.assertEqual(manifest["host_graphics_apps"], [])
            output = root / "graphics"
            manifest = development.prepare(
                root, output, host_graphics_apps=["browser", "navigation"])
            self.assertEqual(manifest["host_graphics_apps"],
                             ["browser", "navigation"])
            self.assertEqual(manifest["host_graphics_transport"],
                             "fixed-shared-objects")
            self.assertEqual((root / "framework.json").read_bytes(), framework)
            self.assertEqual(json.loads((output / "framework.json").read_text()),
                             development.host_graphics(
                                 framework.decode(), ["browser", "navigation"]))
            with self.assertRaises(ValueError):
                development.prepare(
                    root, root / "invalid", host_graphics_apps=["unknown"])
            self.assertFalse((root / "invalid").exists())

    def test_diagnostic_release_override_is_explicit_and_preserves_originals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "persistence.json").write_text(json.dumps(persistence_config()))
            launcher = root / "lsd.sh"
            launcher.write_text(
                development.PRODUCTION_PROPERTY + "\n" +
                'BOOTCLASSPATH="$BOOTCLASSPATH:$LSD_JXE"\n')
            archive = root / "lsd.jar"
            original = synthetic_gateway()
            with ZipFile(archive, "w") as jar:
                jar.writestr(CLASS_NAME, original)
                jar.writestr("resources/hmi_startup.json",
                             json.dumps(startup_config()))
            before = archive.read_bytes(), launcher.read_bytes()
            output = root / "output"
            manifest = development.prepare(
                root, output, launcher=launcher, lsd_jar=archive,
                developer_input=True, diagnostic_release_zero=True)
            self.assertTrue(manifest["diagnostic_release_zero_fingers"])
            self.assertFalse(manifest["normal_native_input_modified"])
            self.assertFalse(manifest["hmi_readiness_verified"])
            self.assertEqual(manifest["diagnostic_class_override"], CLASS_NAME)
            self.assertEqual(before, (archive.read_bytes(), launcher.read_bytes()))
            self.assertIn(
                'BOOTCLASSPATH="/tmp/mh2p-diagnostic-release.jar:'
                '$BOOTCLASSPATH:$LSD_JXE"',
                (output / "lsd-development.sh").read_text())
            with ZipFile(output / "mh2p-diagnostic-release.jar") as jar:
                self.assertEqual(jar.namelist(), [CLASS_NAME])
                self.assertNotEqual(jar.read(CLASS_NAME), original)
            default = root / "default"
            manifest = development.prepare(
                root, default, launcher=launcher, developer_input=True)
            self.assertFalse(manifest["diagnostic_release_zero_fingers"])
            self.assertFalse((default / "mh2p-diagnostic-release.jar").exists())
            self.assertNotIn("mh2p-diagnostic-release.jar",
                             (default / "lsd-development.sh").read_text())
            launcher.write_text(launcher.read_text() + "\n" +
                                development.STARTUP_PROPERTY)
            combined = root / "combined"
            manifest = development.prepare(
                root, combined, launcher=launcher, lsd_jar=archive,
                developer_input=True, early_input=True,
                diagnostic_release_zero=True)
            self.assertTrue(manifest["diagnostic_release_zero_fingers"])
            self.assertEqual(manifest["early_input_bundles"],
                             list(development.EARLY_INPUT_BUNDLES))
            self.assertIn("-Dipl.config.dir.hmi_startup=/tmp",
                          (combined / "lsd-development.sh").read_text())
            self.assertEqual((combined / "mh2p-diagnostic-release.jar").read_bytes(),
                             (output / "mh2p-diagnostic-release.jar").read_bytes())

    def test_diagnostic_override_requires_explicit_input_archive_and_classpath(self):
        for kwargs in (
                {"diagnostic_release_zero": True},
                {"diagnostic_release_zero": True, "developer_input": True,
                 "launcher": Path("/unused")}):
            with self.assertRaises(ValueError):
                development.prepare(Path("/unused"), Path("/unused-output"),
                                    **kwargs)
        with self.assertRaises(ValueError):
            development.developer_launcher(
                development.PRODUCTION_PROPERTY, diagnostic_release_zero=True)

    def test_service_jobs_preserve_forks_delays_and_original_settings(self):
        manager = {"startup": [], "restart_enabled": True}
        startup = {"on_startup_normal": [
            {"cmd": "init", "app": "media", "apprunmode": 17664},
            {"cmd": "inject_userevent", "event": 1000},
            {"cmd": "fork", "commands": [
                {"cmd": "sleep", "timeout": 2500, "blocking": True},
                {"cmd": "run", "app": "media", "blocking": False},
                {"cmd": "init", "app": "other"},
                {"cmd": "run", "app": "other"}]},
            {"cmd": "fork", "commands": [
                {"cmd": "sleep", "timeout": 26000},
                {"cmd": "init", "app": "late"},
                {"cmd": "run", "app": "late"}]}]}
        expected = copy.deepcopy(manager)
        expected["startup"] = [
            startup["on_startup_normal"][0],
            {"cmd": "fork", "commands":
             startup["on_startup_normal"][2]["commands"][:2]}]
        before = copy.deepcopy(startup)
        self.assertEqual(development.development_service_startup(
            json.dumps(manager), "# Original\n" + json.dumps(startup), ["media"]),
            expected)
        self.assertEqual(startup, before)
        self.assertEqual(manager["startup"], [])

    def test_service_jobs_reject_unknown_ambiguous_or_nonempty_startup(self):
        manager = json.dumps({"startup": []})
        pair = [{"cmd": "init", "app": "media"},
                {"cmd": "run", "app": "media"}]
        for jobs, apps in ((pair, ["missing"]), (pair, []),
                           (pair, ["media", "media"]),
                           (pair[:1], ["media"]),
                           (pair[::-1], ["media"]),
                           (pair + pair, ["media"]),
                           ([{"cmd": "fork"}], ["media"]),
                           ([{"cmd": "init", "app": []}], ["media"])):
            with self.subTest(jobs=jobs, apps=apps), self.assertRaises(ValueError):
                development.development_service_startup(
                    manager, json.dumps({"on_startup_normal": jobs}), apps)
        with self.assertRaises(ValueError):
            development.development_service_startup(
                '{"startup":[{}]}',
                json.dumps({"on_startup_normal": pair}), ["media"])

    def test_service_preparation_is_explicit_and_does_not_mutate_dsi_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "persistence.json").write_text(json.dumps(persistence_config()))
            (root / "servicemgr.json").write_text('{"startup":[],"unchanged":1}')
            jobs = [{"cmd": "init", "app": "media", "timeout": 20000},
                    {"cmd": "run", "app": "media", "blocking": False}]
            dsi = json.dumps({"on_startup_normal": jobs}).encode()
            (root / "dsistartup.json").write_bytes(dsi)
            unchanged = root / "default"
            development.prepare(root, unchanged)
            self.assertEqual((unchanged / "servicemgr.json").read_bytes(),
                             (root / "servicemgr.json").read_bytes())
            output = root / "selected"
            manifest = development.prepare(root, output, startup_apps=["media"])
            self.assertEqual(manifest["startup_apps"], ["media"])
            self.assertEqual((output / "dsistartup.json").read_bytes(), dsi)
            self.assertEqual(json.loads((output / "servicemgr.json").read_text()),
                             {"startup": jobs, "unchanged": 1})
            self.assertFalse(manifest["hmi_readiness_verified"])

    def test_local_mapping_only_and_hash_comments(self):
        source = persistence_config()
        text = "# Original comment\n" + json.dumps(source)
        result = development.local_persistence(text)
        expected = copy.deepcopy(source)
        expected["PersistenceA"]["mapping"][0]["backend"] = "database"
        self.assertEqual(result, expected)
        self.assertEqual(source, persistence_config())

    def test_rejects_missing_ambiguous_and_wrong_backend_mappings(self):
        for change in ("missing", "duplicate", "namespace", "sqlite",
                       "database-path", "shape"):
            with self.subTest(change=change):
                config = persistence_config()
                core = config["PersistenceA"]
                if change == "missing":
                    core["mapping"].pop(0)
                elif change == "duplicate":
                    core["mapping"].append(copy.deepcopy(core["mapping"][0]))
                elif change == "namespace":
                    core["mapping"][0]["partitions"] = [1]
                elif change == "sqlite":
                    core["backends"]["database"] = ["other"]
                elif change == "database-path":
                    core["backends"]["database"] = ["SQLite"]
                else:
                    core["mapping"] = [None]
                with self.assertRaises(ValueError):
                    development.local_persistence(json.dumps(config))

    def test_startup_preserves_all_other_graph_and_bundle_settings(self):
        source = startup_config()
        result = development.early_input_startup(json.dumps(source))
        expected = copy.deepcopy(source)
        expected["components"][0]["bundles"].extend([
            {"name": "TcpLog", "setting": 1}, {"name": "FwSwDiag"},
            {"name": "TcpKeyPanel"}, {"name": "RemoveKeyFilter"}])
        expected["components"][1]["bundles"] = [{"name": "PostStartup"}]
        self.assertEqual(result, expected)
        self.assertEqual(source, startup_config())

    def test_rejects_unexpected_startup_graph(self):
        for change in ("duplicate", "missing", "already-early", "eal", "bundle"):
            with self.subTest(change=change):
                config = startup_config()
                if change == "duplicate":
                    config["components"].append(config["components"][0])
                elif change == "missing":
                    config["components"].pop(1)
                elif change == "already-early":
                    config["components"][0]["bundles"].insert(
                        0, {"name": "TcpKeyPanel"})
                elif change == "eal":
                    config["components"][0]["bundles"].pop()
                else:
                    config["components"][1]["bundles"].pop()
                with self.assertRaises(ValueError):
                    development.early_input_startup(json.dumps(config))

    def test_diagnosis_only_keeps_native_input_bundle_order_unchanged(self):
        source = startup_config()
        result = development.early_input_startup(
            json.dumps(source), diagnosis_only=True)
        expected = copy.deepcopy(source)
        expected["components"][0]["bundles"].extend([
            {"name": "TcpLog", "setting": 1}, {"name": "FwSwDiag"}])
        expected["components"][1]["bundles"] = [
            item for item in source["components"][1]["bundles"]
            if item["name"] != "TcpLog"]
        self.assertEqual(result, expected)
        self.assertEqual(source, startup_config())

    def test_launcher_changes_only_explicit_properties(self):
        source = ("before\n" + development.PRODUCTION_PROPERTY + "\n" +
                  development.STARTUP_PROPERTY + "\nafter\n")
        expected = source.replace("-DIS_PRODUCTION_MODE=true",
                                  "-DIS_PRODUCTION_MODE=false")
        self.assertEqual(development.developer_launcher(source), expected)
        self.assertEqual(
            development.developer_launcher(source, True),
            expected.replace(development.STARTUP_PROPERTY,
                             "-Dipl.config.dir.hmi_startup=/tmp"))
        for bad in ("", source + development.PRODUCTION_PROPERTY):
            with self.assertRaises(ValueError):
                development.developer_launcher(bad)
        with self.assertRaises(ValueError):
            development.developer_launcher(development.PRODUCTION_PROPERTY, True)

    def test_preparation_hashes_preserves_sources_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "production"
            config.mkdir()
            (config / "persistence.json").write_text(json.dumps(persistence_config()))
            (config / "framework.json").write_bytes(b'{"unchanged":true}\n')
            launcher = root / "lsd.sh"
            launcher.write_text(development.PRODUCTION_PROPERTY + "\n" +
                                development.STARTUP_PROPERTY + "\n")
            jar_path = root / "lsd.jar"
            with ZipFile(jar_path, "w") as jar:
                jar.writestr("resources/hmi_startup.json",
                             json.dumps(startup_config()))
            inputs = [*config.glob("*.json"), launcher, jar_path]
            before = {path: path.read_bytes() for path in inputs}
            output = root / "development"
            manifest = development.prepare(
                config, output, launcher=launcher, lsd_jar=jar_path,
                developer_input=True, early_input=True)
            self.assertEqual(before, {path: path.read_bytes() for path in inputs})
            self.assertEqual((output / "framework.json").read_bytes(),
                             before[config / "framework.json"])
            for name, digest in manifest["output_sha256"].items():
                self.assertEqual(hashlib.sha256((output / name).read_bytes())
                                 .hexdigest(), digest)
            for name, digest in manifest["source_sha256"].items():
                self.assertEqual(hashlib.sha256(Path(name).read_bytes())
                                 .hexdigest(), digest)
            for flag in ("original_files_modified", "factory_data_supplied",
                         "native_touch_verified", "hmi_readiness_verified",
                         "native_rcc_persistence_verified"):
                self.assertFalse(manifest[flag])
            with self.assertRaises(FileExistsError):
                development.prepare(config, output)

    def test_local_only_does_not_enable_developer_services(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "persistence.json").write_text(json.dumps(persistence_config()))
            output = root / "output"
            manifest = development.prepare(root, output)
            self.assertFalse(manifest["developer_input"])
            self.assertEqual(manifest["early_input_bundles"], [])
            self.assertEqual({p.name for p in output.iterdir()},
                             {"persistence.json", "development-mode.json"})

    def test_diagnosis_only_preparation_preserves_sources_and_input_graph(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "config"
            config.mkdir()
            persistence = config / "persistence.json"
            persistence.write_text(json.dumps(persistence_config()))
            launcher = root / "lsd.sh"
            launcher.write_text(development.PRODUCTION_PROPERTY + "\n" +
                                development.STARTUP_PROPERTY + "\n")
            archive = root / "lsd.jar"
            with ZipFile(archive, "w") as jar:
                jar.writestr("resources/hmi_startup.json",
                             json.dumps(startup_config()))
            before = {path: path.read_bytes()
                      for path in (persistence, launcher, archive)}
            output = root / "output"
            manifest = development.prepare(
                config, output, launcher=launcher, lsd_jar=archive,
                developer_input=True, early_diagnosis=True)
            self.assertEqual(before, {path: path.read_bytes() for path in before})
            self.assertEqual(manifest["early_input_bundles"], [])
            self.assertEqual(manifest["early_diagnosis_bundles"],
                             ["TcpLog", "FwSwDiag"])
            self.assertFalse(manifest["normal_native_input_modified"])
            self.assertFalse(manifest["native_touch_verified"])
            self.assertFalse(manifest["hmi_readiness_verified"])
            self.assertEqual(
                json.loads((output / "hmi_startup.json").read_text()),
                development.early_input_startup(
                    json.dumps(startup_config()), diagnosis_only=True))
            self.assertIn("-Dipl.config.dir.hmi_startup=/tmp",
                          (output / "lsd-development.sh").read_text())
            self.assertFalse((output / "mh2p-diagnostic-release.jar").exists())

    def test_invalid_inputs_do_not_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "output"
            for kwargs in ({}, {"early_input": True}, {"early_diagnosis": True},
                           {"early_input": True, "early_diagnosis": True},
                           {"developer_input": True},
                           {"launcher": root / "missing"},
                           {"lsd_jar": root / "missing"}):
                with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                    development.prepare(root, output, **kwargs)
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
