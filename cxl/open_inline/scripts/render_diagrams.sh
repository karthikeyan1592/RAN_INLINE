#!/usr/bin/env bash
# Regenerate SVG+PNG from the ```plantuml blocks in DIAGRAMS.md
set -e
cd "$(dirname "$0")/.."
mkdir -p docs/diagrams docs/puml
python3 - << 'PY'
import re, pathlib
md = pathlib.Path("DIAGRAMS.md").read_text()
for i,b in enumerate(re.findall(r"```plantuml\n(.*?)```", md, re.S)):
    name = (re.search(r"@startuml\s+(\S+)", b) or [None,f"diagram_{i}"])[1]
    pathlib.Path(f"docs/puml/{name}.puml").write_text(b)
PY
plantuml -tsvg -o "$(pwd)/docs/diagrams" docs/puml/*.puml
plantuml -tpng -o "$(pwd)/docs/diagrams" docs/puml/*.puml
echo "rendered $(ls docs/diagrams/*.svg | wc -l) diagrams to docs/diagrams/"
