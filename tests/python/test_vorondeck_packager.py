"""Exercise the actual release packager without running a device binary."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tarfile

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("packager", ROOT / "scripts/vorondeck-package.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


def test_package_runtime_manifest_and_feature_provenance(tmp_path, monkeypatch):
    source = tmp_path / "source"
    source.mkdir()
    # A tiny source fixture exercises the real selection, hashing and tar writer.
    names = {
        "ui_xml/main.xml": b"<component/>",
        "assets/images/printer.png": b"image",
        "assets/test_gcodes/demo.gcode": b"G28",
        "assets/presentation/banner.png": b"banner",
        "config/settings.json": b"private settings",
        "config/settings.json.template": b'{"wizard_completed":false}',
        "tests/example.py": b"test",
        "LICENSE": b"GPL-3.0-or-later",
    }
    managers = (
        "scripts/moonraker-vorondeck.py", "scripts/moonraker-vorondeck-runtime.py",
        "scripts/install-vorondeck.py", "scripts/vorondeck-bootstrap.py",
        "scripts/vorondeck_distribution.py", "scripts/vorondeck_installer_i18n.py",
        "scripts/setup-vorondeck-updates.py", "assets/updates/check-vorondeck-update.py",
        "assets/updates/vorondeck_public_releases.py",
    )
    for name in managers:
        names[name] = b"# updater\n"
    for name, data in names.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    monkeypatch.setattr(packager, "__file__", str(source / "scripts/vorondeck-package.py"))

    def git_output(command, **kwargs):
        if "ls-files" in command:
            return "\0".join(names).encode()
        assert command[-2:] == ["rev-parse", "HEAD"]
        return "a" * 40 + "\n"

    monkeypatch.setattr(packager.subprocess, "check_output", git_output)
    binary = tmp_path / "helix-screen"
    elf = bytearray(64)
    elf[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", elf, 18, 183)  # AArch64
    binary.write_bytes(elf)
    output = tmp_path / "package.tar.gz"
    monkeypatch.setattr(sys, "argv", ["packager", "--binary", str(binary),
                                      "--output", str(output), "--version", "1.0.2"])
    features = tmp_path / ".build-features"
    for content in (None, "remote_control=yes\ndiag_uploads=no\n", "remote_control=no\n"):
        if content is not None:
            features.write_text(content)
        with pytest.raises(SystemExit):
            packager.main()
        assert not output.exists()
    features.write_text("remote_control=no\ndiag_uploads=no\n")
    packager.main()
    with tarfile.open(output) as archive:
        payload = {m.name: archive.extractfile(m).read() for m in archive.getmembers()}
        assert archive.getmember("bin/helix-screen").mode == 0o755
    manifest = json.loads(payload.pop("vorondeck-release.json"))
    assert manifest["architecture"] == "aarch64"
    assert manifest["version"] == "1.0.2"
    assert manifest["source_commit"] == "a" * 40
    assert manifest["files"] == {n: hashlib.sha256(d).hexdigest() for n, d in payload.items()}
    assert json.loads(payload["config/settings.json.template"])["wizard_completed"] is False
    for name in ("config/settings.json", "tests/example.py", "assets/test_gcodes/demo.gcode",
                 "assets/presentation/banner.png"):
        assert name not in payload
    for name in managers:
        assert payload["assets/updates/manager/" + name] == names[name]
    assert payload["ui_xml/main.xml"] == names["ui_xml/main.xml"]
    assert payload["assets/images/printer.png"] == names["assets/images/printer.png"]
    assert payload["LICENSE"] == names["LICENSE"]
    # A second attempt may not overwrite an existing artifact.
    before = output.read_bytes()
    with pytest.raises(SystemExit):
        packager.main()
    assert output.read_bytes() == before
