// Soft u64 “seq → payload” sketch — converge-mode recipe (pit 170).
// Build: make soft-example   →  build/soft_u64_log
#include "fcs.h"

#include <cstdio>
#include <cstring>

using namespace fcs;
using namespace xdb;

static int fail(const char* where, RC rc) {
    std::fprintf(stderr, "%s: %s\n", where, rc_str(rc));
    return 1;
}

int main() {
    // Tiny durable log index: key = sequence, value = fixed blob.
    RamDev dev(1u << 14);
    Store s;
    static u8 arena[512 * PAGE];
    if (RC rc = s.open(&dev, arena, 512); rc != RC::OK) return fail("open", rc);

    s.configure_deploy(true);  // product Soft: Post + hole + P2 + read knives

    constexpr u64 N = 500;
    for (u64 seq = 1; seq <= N; ++seq) {
        char payload[24];
        std::snprintf(payload, sizeof payload, "evt-%llu", (unsigned long long)seq);
        if (RC rc = s.put(seq, payload, (u16)std::strlen(payload)); rc != RC::OK)
            return fail("put", rc);
        if (seq % 50 == 0) {
            if (RC rc = s.commit(); rc != RC::OK) return fail("commit", rc);
        }
    }
    if (RC rc = s.commit(); rc != RC::OK) return fail("commit.final", rc);

    // Read SLA boundary: repay Post debt (or polish()) before serving.
    if (RC rc = s.soft_cons_repay(); rc != RC::OK) return fail("repay", rc);

    u8 buf[64];
    u16 n = 0;
    if (RC rc = s.get(N / 2, buf, sizeof buf, &n); rc != RC::OK)
        return fail("get", rc);
    buf[n < sizeof buf ? n : sizeof buf - 1] = 0;

    u64 scanned = 0;
    if (RC rc = s.select(1, N + 1, [&](u64 /*k*/, const u8* /*v*/, u16 /*vn*/) {
            ++scanned;
            return true;
        });
        rc != RC::OK)
        return fail("select", rc);

    std::printf("soft_u64_log OK  mid=\"%s\"  scanned=%llu  runs=%u\n",
                (char*)buf, (unsigned long long)scanned, (unsigned)s.run_count());
    return 0;
}
