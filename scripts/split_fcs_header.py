#!/usr/bin/env python3
"""Mechanical fcs.h split per docs/30-discuss/ENG_DEBT_SPLIT.md (A–D + Store).
Behavior-identical: cut-paste only. Persistent backup under backup/eng-debt-split/.
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "include"
SRC = INC / "fcs.h"
BAK = ROOT / "backup" / "eng-debt-split"


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    h.update(p.read_bytes())
    return h.hexdigest()


def find_banner(lines: list[str], title: str) -> int:
    """1-based line of '//  Mxx' banner content line, return 0-based index of ===== before it."""
    needle = f"//  {title}"
    for i, ln in enumerate(lines):
        if ln.startswith(needle):
            # prefer the ===== line just above
            j = i - 1
            while j >= 0 and lines[j].strip() == "":
                j -= 1
            if j >= 0 and lines[j].startswith("// ==="):
                return j
            return i
    raise SystemExit(f"banner not found: {title}")


def main() -> int:
    BAK.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    pre = BAK / "fcs.h.pre-split"
    stamped = BAK / f"fcs.h.pre-split.{stamp}"
    shutil.copy2(SRC, pre)
    shutil.copy2(SRC, stamped)
    (BAK / "SHA256.pre-split").write_text(sha256(SRC) + "\n", encoding="utf-8")
    (BAK / "WC.pre-split").write_text(f"{sum(1 for _ in SRC.open('r', encoding='utf-8', errors='replace'))}\n", encoding="utf-8")

    text = SRC.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)

    i01 = find_banner(lines, "M01  参数")
    i02 = find_banner(lines, "M02  编解码")
    i03 = find_banner(lines, "M03  内存表")
    i04 = find_banner(lines, "M04  布隆过滤器")
    i05 = find_banner(lines, "M05  段 / 单元")
    i06 = find_banner(lines, "M06  线程槽")
    i07 = find_banner(lines, "M07  Store")

    # preamble: lines[0:i01) — includes #pragma, includes, namespace open, using
    preamble = lines[:i01]
    # find namespace fcs { inside preamble
    ns_open = None
    for i, ln in enumerate(preamble):
        if ln.strip() == "namespace fcs {":
            ns_open = i
            break
    if ns_open is None:
        raise SystemExit("namespace fcs { not found in preamble")

    head_includes = "".join(preamble[:ns_open])  # through using? keep up to before namespace
    # Actually keep pragma+includes only in each file; M00 TOC stays in fcs.h

    m01_m02 = lines[i01:i03]
    m03_m05 = lines[i03:i06]
    m06 = lines[i06:i07]
    m07 = lines[i07:]
    # strip trailing namespace close from m07 if present
    while m07 and m07[-1].strip() == "":
        m07.pop()
    if m07 and m07[-1].strip() == "} // namespace fcs":
        m07 = m07[:-1]
    while m07 and m07[-1].strip() == "":
        m07.pop()

    std_inc = """#include <vector>
#include <memory>
#include <array>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cstdlib>
"""

    codec = f"""#pragma once
#include "common.h"
#include "layout.h"
{std_inc}
namespace fcs {{

using namespace xdb;

{''.join(m01_m02)}
}} // namespace fcs
"""

    dirh = f"""#pragma once
#include "fcs_codec.h"

namespace fcs {{

{''.join(m03_m05)}
}} // namespace fcs
"""

    synch = f"""#pragma once
#include "fcs_codec.h"

namespace fcs {{

{''.join(m06)}
}} // namespace fcs
"""

    storeh = f"""#pragma once
#include "fcs_dir.h"
#include "fcs_sync.h"
#include "blockdev.h"

namespace fcs {{

{''.join(m07)}

}} // namespace fcs
"""

    # fcs.h facade: M00 TOC + aggregate includes (public API unchanged)
    # Keep original preamble comments through TOC; then include modules instead of bodies.
    facade_pre = "".join(preamble[:ns_open])
    # Drop duplicate std includes from facade if present — keep as original for minimal diff feel
    facade = f"""{facade_pre}#include "fcs_codec.h"
#include "fcs_dir.h"
#include "fcs_sync.h"
#include "fcs_store.h"

// M01–M07 实现已机械拆至 fcs_codec.h / fcs_dir.h / fcs_sync.h / fcs_store.h（坑 169）
// 对外仍只 #include \"fcs.h\"。还原：backup/eng-debt-split/fcs.h.pre-split
"""

    (INC / "fcs_codec.h").write_text(codec, encoding="utf-8")
    (INC / "fcs_dir.h").write_text(dirh, encoding="utf-8")
    (INC / "fcs_sync.h").write_text(synch, encoding="utf-8")
    (INC / "fcs_store.h").write_text(storeh, encoding="utf-8")
    (INC / "fcs.h").write_text(facade, encoding="utf-8")

    # also keep stamped post-split copies of new files
    for name in ("fcs.h", "fcs_codec.h", "fcs_dir.h", "fcs_sync.h", "fcs_store.h"):
        shutil.copy2(INC / name, BAK / f"{name}.post-split.{stamp}")

    manifest = BAK / "MANIFEST.md"
    manifest.write_text(
        f"""# eng-debt-split backup

- stamp: `{stamp}`
- pre-split sha256: `{sha256(pre)}`
- files: `fcs.h.pre-split` (persistent) + `fcs.h.pre-split.{stamp}`
- post-split copies: `*.post-split.{stamp}`

Restore:
```sh
cp backup/eng-debt-split/fcs.h.pre-split include/fcs.h
rm -f include/fcs_codec.h include/fcs_dir.h include/fcs_sync.h include/fcs_store.h
```
""",
        encoding="utf-8",
    )
    print(f"SPLIT_OK stamp={stamp}")
    print(f"  codec={len(codec.splitlines())} dir={len(dirh.splitlines())} "
          f"sync={len(synch.splitlines())} store={len(storeh.splitlines())} "
          f"facade={len(facade.splitlines())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
