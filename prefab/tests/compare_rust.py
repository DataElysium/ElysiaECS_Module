"""Compile the real Rust prefab library and compare the unchanged car fixture.

Usage: python3 tests/compare_rust.py /path/to/bevy_dll_demo [cpp-example-binary]
The temporary Cargo project never modifies the Bevy workspace.
"""
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
bevy = Path(sys.argv[1]).resolve()
binary = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else root / "build/linux/x86_64/release/prefab_car"
fixture = root / "fixtures/game.json"

def compare(a, b, path="root"):
    if isinstance(a, dict) and isinstance(b, dict):
        assert a.keys() == b.keys(), (path, a.keys(), b.keys())
        for key in a:
            compare(a[key], b[key], path + "." + key)
    elif isinstance(a, list) and isinstance(b, list):
        assert len(a) == len(b), path
        for i, (x, y) in enumerate(zip(a, b)):
            compare(x, y, f"{path}[{i}]")
    elif isinstance(a, (float, int)) and isinstance(b, (float, int)):
        assert math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-7), (path, a, b)
    else:
        assert a == b, (path, a, b)

with tempfile.TemporaryDirectory(prefix="elysia-prefab-rust-") as directory:
    temp = Path(directory)
    (temp / "src").mkdir()
    (temp / "src/main.rs").write_text((root / "tests/rust_reference.rs").read_text())
    lines = ['[package]', 'name="elysia_prefab_reference"', 'version="0.1.0"', 'edition="2024"',
             '[workspace]', '[dependencies]', 'serde_json="1"']
    for name, path in {"bevy_ecs": "libs/bevy_base/bevy_ecs", "ecs_prefab": "libs/ecs_prefab", "shared_api": "shared_api"}.items():
        lines.append(f'{name} = {{ path = {json.dumps(str(bevy / path))} }}')
    lines.append('[patch.crates-io]')
    for name in ['bevy_ecs', 'bevy_ecs_macros', 'bevy_app', 'bevy_macro_utils']:
        lines.append(f'{name} = {{ path = {json.dumps(str(bevy / "libs/bevy_base" / name))} }}')
    (temp / "Cargo.toml").write_text("\n".join(lines))
    rust = subprocess.check_output(['cargo', 'run', '--quiet', '--manifest-path', str(temp / 'Cargo.toml'),
                                    '--', str(fixture)], text=True)
    cpp = subprocess.check_output([str(binary), str(fixture), '--dump'], text=True)
    compare(json.loads(rust), json.loads(cpp))
    print(f"Rust/C++ parity: {len(json.loads(cpp))} entity records, all registered components and hierarchy paths match.")
