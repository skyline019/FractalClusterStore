#!/usr/bin/env python3
from pathlib import Path
path = Path("/mnt/e/FCS/embdb/x/include/fcs.h")
text = path.read_text(encoding="utf-8")

repls = [
    ("        u32 exp = 0, out_ver = 0, maxseq = 0;",
     "        u32 exp = 0; u64 out_ver = 0, maxseq = 0;"),
    ("                const u32 maxseq = j.maxseq;",
     "                const u64 maxseq = j.maxseq;"),
    ("        u32 exp_keys = 0, out_ver = 0;",
     "        u32 exp_keys = 0; u64 out_ver = 0;"),
    ("        u32 newlvl = 0, maxseq = 0;\n        u32 out_ver = 0;",
     "        u32 newlvl = 0; u64 maxseq = 0;\n        u64 out_ver = 0;"),
    ("                                  std::vector<std::pair<u32, u32>>* job_src) {",
     "                                  std::vector<std::pair<u32, u64>>* job_src) {"),
    ('std::fprintf(stderr, "  RUN seq=%u rid=%u npg=%u nrec=%u live_recs=%u live_bytes=%llu\\n",\n                    r->seq, r->page0, r->npages, r->nrec, r->live_recs, (unsigned long long)r->live_bytes);',
     'std::fprintf(stderr, "  RUN seq=%llu rid=%u npg=%u nrec=%u live_recs=%u live_bytes=%llu\\n",\n                    (unsigned long long)r->seq, r->page0, r->npages, r->nrec, r->live_recs, (unsigned long long)r->live_bytes);'),
]

for a, b in repls:
    if a not in text:
        print("MISSING", a[:60])
    else:
        text = text.replace(a, b, 1)
        print("ok", a[:40])

# MergeJob fields
old = None
# find out_ver maxseq in MergeJob
import re
m = re.search(r"struct MergeJob \{.*?u32 out_ver.*?u32 maxseq.*?;", text, re.S)
if m:
    print("found MergeJob fields snippet:\n", m.group(0)[:300])

# Fix MergeJob member types
text2 = text
text2 = text2.replace("u32 out_ver = 0;", "u64 out_ver = 0;", 1)  # might hit wrong one
# Better: in MergeJob only
idx = text.find("struct MergeJob {")
if idx < 0:
    raise SystemExit("no MergeJob")
# replace within next 80 lines
chunk = text[idx:idx+2500]
chunk2 = chunk.replace("u32 out_ver = 0;", "u64 out_ver = 0;", 1).replace("u32 maxseq = 0;", "u64 maxseq = 0;", 1)
text = text[:idx] + chunk2 + text[idx+2500:]
print("ok MergeJob ver/seq types")

# RunWriter ctor takes ver as u32? check
path.write_text(text, encoding="utf-8")
print("written")
