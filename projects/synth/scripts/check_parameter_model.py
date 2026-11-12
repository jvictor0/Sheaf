#!/usr/bin/env python3
"""Check reviewed source correspondence and fresh Lean axiom-audit output."""
import hashlib
import json
from pathlib import Path
import re
import struct
from fractions import Fraction
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
# Every theorem/lemma, however decorated, is enumerated; escape hatches are
# refused anywhere in the model, not only inside audited proofs.
declaration = re.compile(r"^[ \t]*(?:@\[[^\]]*\][ \t]*)?(private[ \t]+|protected[ \t]+)?"
                         r"(?:theorem|lemma)[ \t]+([A-Za-z_][A-Za-z_0-9.']*)", re.M)
escape = re.compile(r"\b(?:sorry|admit|native|native_decide|axiom|macro_rules|macro|notation|syntax|elab|"
                    r"infix|infixl|infixr|prefix|postfix)\b|warningAsError")
lean_comment = re.compile(r"/-.*?-/|--[^\n]*", re.S)
declared, statements = [], {}
for model in sorted((root / "formal/Sheaf").glob("*.lean")):
    text = model.read_text()
    if model.name != "Audit.lean" and (hit := escape.search(lean_comment.sub(" ", text))):
        raise SystemExit(f"Proof escape hatch in {model.name}: {hit[0]!r}")
    matches = list(declaration.finditer(text))
    if not matches:
        continue
    namespaces = re.findall(r"^namespace ([A-Za-z_][A-Za-z_0-9.]*)", text, re.M)
    if len(namespaces) != 1 or any(match[1] for match in matches):
        raise SystemExit(f"Review theorem enumeration for {model.name}")
    for match in matches:
        name = namespaces[0] + "." + match[2]
        declared.append(name)
        statement = text[match.start():text.index(":=", match.end())]
        statements[name] = " ".join(statement.split())
if sorted(declared) != sorted(manifest["theorems"]):
    raise SystemExit("Model theorem declarations differ from the manifest")
# Pinned statements: weakening or narrowing a claim needs a reviewed manifest edit.
for name, statement in statements.items():
    if manifest["statements"].get(name) != statement:
        raise SystemExit(f"Theorem statement changed: {name}\n  now: {statement}\n"
                         "review the claim before updating formal/requirements.json statements")
if set(manifest["statements"]) != set(statements):
    raise SystemExit("Pinned statements differ from the theorem manifest")
for contract in manifest["contracts"]:
    missing = [proof for proof in contract["proofs"] if proof not in manifest["theorems"]]
    if missing:
        raise SystemExit(f"Contract cites unaudited proofs: {missing}")
# The C++ settlement constants must be the ones the Lean publication model assumes.
bridge = re.sub(r"//[^\n]*|/\*.*?\*/", " ", (root / "include/synth/RateSeparatedParameters.hpp").read_text(), flags=re.S)
samples = re.search(r"kRootSettleSamples = (\d+);", bridge)
tolerance = re.search(r"kRootSettleTolerance = ([0-9.e+-]+)f;", bridge)
behavior = (root / "formal/Sheaf/Behavior.lean").read_text()
law = re.search(r"if (\d+) ≤ age ∧ magnitude \(raw-target\) ≤ 1/(\d+) then target else raw", behavior)
uses = (bridge.count("root.unchangedTargetAge >= kRootSettleSamples"),
        bridge.count("<= double(kRootSettleTolerance)"))
if not (samples and tolerance and law) or uses != (1, 1) or len(re.findall(r"kRootSettle(?:Samples|Tolerance) =", bridge)) != 2:
    raise SystemExit("Settlement constants not found in the bridge or the Lean model")
binary32 = struct.unpack("f", struct.pack("f", float(tolerance[1])))[0]
if (int(samples[1]) != int(law[1]) or
        binary32 != struct.unpack("f", struct.pack("f", 1 / int(law[2])))[0] or
        Fraction(binary32) > Fraction(1, int(law[2]))):
    raise SystemExit("Settlement constants differ between RateSeparatedParameters.hpp and Behavior.lean")
subprocess.run([sys.executable, str(root / "scripts/check_source_filter_coefficients.py")], check=True)
subprocess.run([sys.executable, str(root / "scripts/generate_source_filter_certificate.py")], check=True)
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
