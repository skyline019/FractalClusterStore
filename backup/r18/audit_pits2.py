#!/usr/bin/env python3
"""Accurate PIT step counts via bracket matching."""
import pathlib, re

def extract_array_after(src: str, key: str, start: int):
    i = src.find(key, start)
    if i < 0:
        return None, -1
    i = src.find("[", i)
    if i < 0:
        return None, -1
    depth = 0
    for j in range(i, len(src)):
        c = src[j]
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                return src[i + 1 : j], j
    return None, -1


def count_steps(arr: str) -> int:
    if not arr:
        return 0
    return len(re.findall(r"\{\s*t\s*:", arr))


here = pathlib.Path("/mnt/e/FCS/embdb/x/web/pits/data")
rows = []
for p in sorted(here.glob("batch*.js")):
    t = p.read_text(encoding="utf-8")
    for m in re.finditer(r"PIT\(\{\s*id:\s*'?([\w-]+)'?", t):
        pid = m.group(1)
        pos = m.start()
        # end at next PIT or EOF
        nxt = t.find("PIT({", pos + 4)
        block = t[pos : nxt if nxt > 0 else len(t)]
        old_a, _ = extract_array_after(block, "old:", 0)
        new_a, _ = extract_array_after(block, "new:", 0)
        # prefer the storyboard old/new near end — take last occurrence of 'old: [' before code? 
        # Actually structure is code.old then old: storyboard. Find last '  old: [' 
        old_idxs = [m2.start() for m2 in re.finditer(r"(?m)^  old:\s*\[", block)]
        new_idxs = [m2.start() for m2 in re.finditer(r"(?m)^  new:\s*\[", block)]
        if old_idxs:
            old_a, _ = extract_array_after(block, "old:", old_idxs[-1])
        if new_idxs:
            new_a, _ = extract_array_after(block, "new:", new_idxs[-1])
        os_, ns_ = count_steps(old_a or ""), count_steps(new_a or "")
        has_tbl = "tbl:" in (old_a or "") or "tbl:" in (new_a or "")
        rows.append((pid, os_, ns_, has_tbl, p.name))

thin = [r for r in rows if r[1] < 5 or r[2] < 5]
no_tbl = [r for r in rows if not r[3]]
print(f"total {len(rows)} thin(<5 steps) {len(thin)} no_tbl {len(no_tbl)}")
print("thin sample:")
for r in sorted(thin, key=lambda x: x[1] + x[2])[:40]:
    print(r)
