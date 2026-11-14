#!/usr/bin/env python3
"""Verify research-file integrity; never execute CAD, CAM, or NC content."""

import hashlib
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
import zipfile


def main():
    root = Path(__file__).resolve().parents[1]
    manifest = json.loads((root / "samples/manifest.json").read_text())
    expected = set()
    count = total = 0
    for dataset in manifest["datasets"]:
        for entry in dataset["files"]:
            path = (root / entry["local_path"]).resolve()
            if not path.is_relative_to(root / "samples/upstream"):
                raise ValueError(f"File outside sample directory: {path}")
            if path in expected:
                raise ValueError(f"Duplicate manifest entry: {path}")
            expected.add(path)
            data = path.read_bytes()
            if len(data) != entry["bytes"]:
                raise ValueError(f"Byte count differs: {path}")
            if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError(f"SHA-256 differs: {path}")
            suffix = path.suffix.lower()
            if suffix == ".step":
                if not data.startswith(b"ISO-10303-21;") or b"END-ISO-10303-21;" not in data:
                    raise ValueError(f"Missing STEP envelope: {path}")
            elif suffix == ".fcstd":
                with zipfile.ZipFile(path) as archive:
                    if archive.testzip() is not None:
                        raise ValueError(f"Corrupt FreeCAD ZIP: {path}")
                    ET.fromstring(archive.read("Document.xml"))
            elif suffix in (".json", ".camotics"):
                document = json.loads(data)
                if suffix == ".camotics":
                    for name in document.get("files", []):
                        target = (path.parent / name).resolve()
                        if not target.is_relative_to(path.parent) or not target.is_file():
                            raise ValueError(f"Missing or external project reference: {path}: {name}")
            elif suffix == ".pdf" and not data.startswith(b"%PDF-"):
                raise ValueError(f"Missing PDF header: {path}")
            count += 1
            total += len(data)
    actual = {p.resolve() for p in (root / "samples/upstream").rglob("*") if p.is_file()}
    if actual != expected:
        raise ValueError(f"Unlisted upstream files: {sorted(actual - expected)}")
    print(f"Verified {count} upstream files ({total:,} bytes); no machine execution performed.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, ET.ParseError) as error:
        print(f"Verification failed: {error}", file=sys.stderr)
        sys.exit(1)
