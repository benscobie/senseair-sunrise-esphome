"""Smoke tests: validate that example YAML configs pass ESPHome config validation.

These tests run `esphome config <yaml>` which loads the schema, validates
the configuration, and resolves all references — without invoking the
PlatformIO toolchain.  This catches schema mismatches, missing imports,
and validation errors.

Requires: esphome installed and secrets.yaml present (or a test copy).
Marked slow — skip with: pytest -m "not slow"
"""

from pathlib import Path
import subprocess
import sys

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent
EXAMPLE_YAMLS = sorted(PROJECT_ROOT.glob("example*.yaml"))


@pytest.fixture(scope="session", autouse=True)
def ensure_secrets(tmp_path_factory):
    """Create a temporary secrets.yaml if one doesn't exist, so CI can run."""
    secrets = PROJECT_ROOT / "secrets.yaml"
    if secrets.exists():
        yield
        return
    # Write dummy secrets for validation (never used to connect anywhere)
    secrets.write_text(
        'wifi_ssid: "test"\n'
        'wifi_password: "testtest"\n'
        'esphome_api_key: "dGVzdHRlc3R0ZXN0dGVzdHRlc3R0ZXN0dGVzdHQ="\n'
        'mqtt_broker: "127.0.0.1"\n'
        'mqtt_username: "test"\n'
        'mqtt_password: "test"\n'
    )
    yield
    secrets.unlink(missing_ok=True)


@pytest.mark.slow
@pytest.mark.parametrize(
    "yaml_file",
    EXAMPLE_YAMLS,
    ids=[p.name for p in EXAMPLE_YAMLS],
)
def test_esphome_config_validates(yaml_file):
    """Each example YAML must pass `esphome config` without errors."""
    result = subprocess.run(
        [sys.executable, "-m", "esphome", "config", str(yaml_file)],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
        cwd=PROJECT_ROOT,
    )
    assert result.returncode == 0, (
        f"esphome config {yaml_file.name} failed:\n{result.stderr}"
    )
