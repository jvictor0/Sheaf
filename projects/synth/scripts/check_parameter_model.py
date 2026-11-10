#!/usr/bin/env python3
"""Check reviewed source correspondence and fresh Lean axiom-audit output."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "formal/requirements.json").read_text())
for path, expected in manifest["sources"].items():
    actual = hashlib.sha256((root / path).read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"Model source changed: {path}; review contracts before refreshing its fingerprint")
for contract in manifest["contracts"]:
    if contract["anchor"] not in (root / contract["source"]).read_text():
        raise SystemExit(f"Missing model source anchor: {contract['anchor']}")
audit = (root / "formal/Sheaf/Audit.lean").read_text()
expected = ["#print axioms " + name for name in manifest["theorems"]]
if [line for line in audit.splitlines() if line.startswith("#print axioms ")] != expected:
    raise SystemExit("Audit declarations differ from the theorem manifest")
declared = []
for model in (root / "formal/Sheaf").glob("*.lean"):
    text = model.read_text()
    names = re.findall(r"^theorem ([A-Za-z_][A-Za-z_0-9.]*)", text, re.M)
    if not names:
        continue
    namespaces = re.findall(r"^namespace ([A-Za-z_][A-Za-z_0-9.]*)", text, re.M)
    if len(namespaces) != 1:
        raise SystemExit(f"Review theorem enumeration for {model.name}")
    declared.extend(namespaces[0] + "." + name for name in names)
if set(declared) != set(manifest["theorems"]):
    raise SystemExit("Model theorem declarations differ from the manifest")
subprocess.run([sys.executable, str(root / "scripts/check_source_filter_coefficients.py")], check=True)
subprocess.run(["lake", "build"], cwd=root, check=True)
run = subprocess.run(["lake", "env", "lean", "formal/Sheaf/Audit.lean"],
                     cwd=root, text=True, capture_output=True, check=True)
print(run.stdout, end="")
allowed = {"propext", "Classical.choice", "Quot.sound"}
for match in re.finditer(r"depends on axioms: \[([^]]*)\]", run.stdout):
    axioms = {word.strip() for word in match[1].split(",") if word.strip()}
    if axioms - allowed:
        raise SystemExit(f"Unapproved axioms: {axioms - allowed}")
for name in manifest["theorems"]:
    if "'" + name + "'" not in run.stdout:
        raise SystemExit(f"Missing fresh axiom audit: {name}")
print(f"Verified {len(expected)} theorem declarations and {len(manifest['sources'])} reviewed sources")
