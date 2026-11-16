#!/usr/bin/env python3
"""Create a local Arduino sketch ZIP with the local Caddy PUBLIC CA certificate.

Nothing is uploaded and no secret tokens are embedded. Do not publish generated
sketch archives if you later add credentials or device-specific settings.
Usage:
    python3 scripts/prepare_firmware.py cloud-root.crt
Output:
    vega-firmware-v6.6-with-ca.zip
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import ssl
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware" / "Vega_S_400_Local_v6_6_Cloud"
OUT = ROOT / "vega-firmware-v6.6-with-ca.zip"


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare Vega-S ESP8266 firmware with a trusted public Caddy CA")
    parser.add_argument("ca_file", type=Path, help="Caddy root.crt (public certificate, not private key)")
    args = parser.parse_args()

    if not SOURCE.is_dir():
        sys.exit(f"Missing firmware directory: {SOURCE}")
    try:
        ca = args.ca_file.read_text(encoding="ascii").strip()
        der = ssl.PEM_cert_to_DER_cert(ca)
    except (ValueError, OSError, UnicodeError) as exc:
        sys.exit(f"Invalid PEM certificate: {exc}")
    if ca.count("-----BEGIN CERTIFICATE-----") != 1 or ca.count("-----END CERTIFICATE-----") != 1:
        sys.exit("Expected exactly one public PEM certificate (root.crt)")
    if "PRIVATE KEY" in ca or not der:
        sys.exit("Refusing private key or empty certificate")

    cert_header = (
        '#pragma once\n'
        '// Public trust anchor from your own Caddy server. NEVER use setInsecure().\n'
        'static const char VEGA_CLOUD_ROOT_CA[] PROGMEM = R"VEGACA(\n'
        + ca + '\n)VEGACA";\n'
    )

    with tempfile.TemporaryDirectory(prefix="vega-build-") as tempdir:
        dest = Path(tempdir) / SOURCE.name
        shutil.copytree(SOURCE, dest)
        (dest / "CloudCert.h").write_text(cert_header, encoding="utf-8")
        # Keep Arduino folder name identical to .ino basename.
        ino = dest / (SOURCE.name + ".ino")
        if not ino.is_file():
            sys.exit(f"Arduino sketch missing: {ino.name}")
        connector = (dest / "CloudConnector.h").read_text(encoding="utf-8")
        if "https://213.91.182.27:38443" not in connector:
            sys.exit("Cloud connector public URL does not match expected IP:port")
        with zipfile.ZipFile(OUT, "w", compression=zipfile.ZIP_DEFLATED) as z:
            for file in sorted(dest.rglob("*")):
                if file.is_file():
                    z.write(file, arcname=str(file.relative_to(Path(tempdir))))
    print("Created:", OUT)
    print("Note: sketch is NOT compiled or hardware tested. Do not flash a live heater.")
    print("Extract on Mac, open Vega_S_400_Local_v6_6_Cloud.ino in Arduino IDE.")


if __name__ == "__main__":
    main()
