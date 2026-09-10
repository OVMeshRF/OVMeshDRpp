#!/usr/bin/env python3
"""Desktop guards plus three-frame passive/synthetic startup regressions.

Standard-library-only runner; expects desktop and CLI executable paths.
Guard cases exit before window/device access. Smoke cases need an authorized
local graphics session; they open windows but never open a radio or GPS.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

desktop, cli = (str(Path(arg).resolve()) for arg in sys.argv[1:3])
cases = [
    (desktop, ["--ui-smoke", "0"], "positive frame count"),
    (desktop, ["--ui-smoke", "-1"], "positive frame count"),
    (desktop, ["--settings-directory", "relative"], "absolute local folder"),
    (desktop, ["--settings-directory", "/not-created", "--demo"], "ordinary desktop startup only"),
    (desktop, ["--settings-directory", "/not-created", "--ui-smoke", "3"], "ordinary desktop startup only"),
    (desktop, ["--settings-directory", "/not-created", "--view-survey", "/not-opened.sqlite"], "ordinary desktop startup only"),
    (desktop, ["--settings-directory", "/not-created", "--headless-demo"], "ordinary desktop startup only"),
    (desktop, ["--settings-directory", "/not-created", "--prepare-desktop-hackrf"], "ordinary desktop startup only"),
    (desktop, ["--settings-directory", "/not-created", "--analyze-saved", "/not-opened.sqlite"], "ordinary desktop startup only"),
    (desktop, ["--settings-directory", "/not-created", "--export", "/not-opened.sqlite"], "ordinary desktop startup only"),
    (cli, ["--settings-directory", "/not-created"], "requires the desktop executable"),
    (desktop, ["--view-survey", "relative.sqlite"], "absolute local path"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--demo"], "Choose one"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--analyze-saved", "/not-opened.sqlite"], "Choose one"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--export", "/not-opened.sqlite"], "Choose one"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--seconds", "3"], "is passive"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--until-stopped"], "is passive"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--gps-device", "/not-a-device", "--confirm-gps-access"], "is passive"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--survey-key-stdin", "1,unused"], "is passive"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--sample-rate", "8000000"], "is passive"),
    (desktop, ["--view-survey", "/not-opened.sqlite", "--confirm-radio-access"], "is passive"),
    (cli, ["--view-survey", "/not-opened.sqlite"], "requires the desktop executable"),
    (desktop, ["--until-stopped"], "requires a desktop"),
    (desktop, ["--headless-demo", "--until-stopped"], "requires a desktop"),
    (desktop, ["--receive-hackrf", "--until-stopped"], "requires a desktop"),
    (desktop, ["--demo", "--until-stopped", "--seconds", "1"], "do not combine"),
    (desktop, ["--demo", "--until-stopped", "--ui-smoke", "0"], "do not combine"),
    (desktop, ["--prepare-desktop-hackrf", "--until-stopped", "--ui-smoke", "1"], "do not combine"),
    (desktop, ["--desktop-receive-hackrf", "--until-stopped"], "requires explicit --confirm-radio-access"),
    (desktop, ["--desktop-receive-hackrf", "--seconds", "1"], "requires explicit --confirm-radio-access"),
    (desktop, ["--prepare-desktop-hackrf", "--until-stopped", "--confirm-radio-access"], "requires consent in the UI"),
    (desktop, ["--demo", "--until-stopped", "--confirm-radio-access"], "only to an explicit HackRF"),
    (cli, ["--demo", "--until-stopped"], "requires the desktop executable"),
    (cli, ["--prepare-desktop-hackrf", "--until-stopped"], "requires the desktop executable"),
]
for binary, args, expected in cases:
    result = subprocess.run([binary, *args], stdin=subprocess.DEVNULL, capture_output=True,
                            text=True, timeout=5)
    assert result.returncode == 1 and expected in result.stderr, (args, result.returncode, result.stderr)
    assert "Desktop control" not in result.stdout and "Started" not in result.stdout
# Supplying default/ordinary configuration must not create a managed launch.
for args in (["--ui-smoke", "3"], ["--demo", "--ui-smoke", "3"]):
    result = subprocess.run([desktop, *args], stdin=subprocess.DEVNULL,
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, (args, result.returncode, result.stderr)
    assert "Desktop control" not in result.stdout and "Managed desktop reception" not in result.stderr

# Ordinary demo launches must still apply their supplied recording/provenance.
with tempfile.TemporaryDirectory(prefix="desktop-mode-", dir=Path(cli).parent) as directory:
    session = str(Path(directory) / "demo.sqlite")
    output = str(Path(directory) / "demo.csv")
    result = subprocess.run([desktop, "--demo", "--ui-smoke", "3", "--session", session,
                             "--antenna-description", "Ordinary synthetic demo antenna",
                             "--receiver-description", "Synthetic startup regression",
                             "--survey-notes", "Three-frame local smoke"],
                            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert "Desktop control" not in result.stdout
    exported = subprocess.run([cli, "--export", session, "--output", output, "--provenance"],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
    assert exported.returncode == 0, exported.stderr
    assert "Ordinary synthetic demo antenna" in Path(output).read_text()
    assert "Three-frame local smoke" in Path(output).read_text()
    original = Path(session).read_bytes()
    viewed = subprocess.run([desktop, "--view-survey", session, "--ui-smoke", "3"],
                            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15)
    assert viewed.returncode == 0, (viewed.returncode, viewed.stderr)
    assert "Saved survey opened read-only: detailed_analysis=1" in viewed.stdout
    assert "Desktop control" not in viewed.stdout and "Started" not in viewed.stdout
    assert Path(session).read_bytes() == original, "Passive desktop viewing changed the saved database"

print(f"PASS: {len(cases)} desktop-mode guards, passive/demo three-frame startup, ordinary-demo provenance readback, and passive saved-view readback; no radio or GPS opened")
