#!/usr/bin/env python3
import re, json, pathlib
from collections import Counter
here = pathlib.Path("/mnt/e/FCS/embdb/x/web/pits/data")
text = json.loads((here / "pits_text.json").read_text(encoding="utf-8"))
print("text type", type(text).__name__, "len", len(text))
print("sample id", text[0].get("id") if text else None, "title", (text[0].get("title") or "")[:40])
all_ids = []
thin = []
for p in sorted(here.glob("batch*.js")):
    t = p.read_text(encoding="utf-8")
    blocks = re.split(r"PIT\(\{", t)[1:]
    for block in blocks:
        m = re.match(r"\s*id:\s*'?([\w-]+)'?", block)
        if not m:
            continue
        pid = m.group(1)
        all_ids.append(pid)
        old_m = re.search(r"old:\s*\[(.*?)\],\s*new:\s*\[", block, re.S)
        new_m = re.search(r"new:\s*\[(.*?)\]\s*\}\s*\);", block, re.S)
        os_ = old_m.group(1).count("{ t:") if old_m else 0
        ns_ = new_m.group(1).count("{ t:") if new_m else 0
        concrete = 0
        if old_m:
            concrete = sum(old_m.group(1).count(k) for k in ("kv:", "rows:", "pages:", "lanes:", "log:"))
        if os_ < 5 or ns_ < 5 or concrete < 3:
            thin.append((str(pid), os_, ns_, concrete, p.name))
print("storyboards", len(all_ids), "thin", len(thin))
print("thin by file", dict(Counter(x[4] for x in thin)))
print("shortest:")
for x in sorted(thin, key=lambda z: (z[1] + z[2], z[3]))[:30]:
    print(x)
