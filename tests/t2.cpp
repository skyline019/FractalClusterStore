#include "store.h"
#include <cstdio>
#include <map>
#include <random>
using namespace xdb;
static u8 arena[NAV_CAP * PAGE];

static int failures = 0;
static void chk(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL: %s\n", what); ++failures; }
}

static void dump(Engine& e, u32 limit) {
    Engine::VerifyReport r;
    RC rc = e.verify(&r);
    std::printf("  verify rc=%s pages=%u leaves=%u inners=%u pubs=%u orphans=%u errors=%u first=%u(%s)\n",
                rc_str(rc), r.pages, r.leaves, r.inners, r.pubs, r.orphans, r.errors, r.first_err_page, r.first_err);
    if (limit) {
        u32 cnt = 0;
        e.scan([&](u64 k, const u8* v, u16 n) {
            if (cnt < limit) std::printf("    %llu -> %u (%u B)\n", (unsigned long long)k, n >= 4 ? *(const u32*)v : 0u, n);
            ++cnt; return true;
        });
        std::printf("    scan total=%u\n", cnt);
    }
}

int main(int argc, char** argv) {
    u32 seed = argc > 1 ? (u32)atoi(argv[1]) : 1;
    int rounds = argc > 2 ? atoi(argv[2]) : 6;
    std::mt19937_64 rng(seed);
    RamDev* dev = new RamDev(4096);
    Engine* e = new Engine();
    RC rc = e->open(dev, arena, NAV_CAP);
    if (rc != RC::OK) { std::printf("open %s\n", rc_str(rc)); return 1; }
    std::map<u64, u32> oracle;
    for (int round = 0; round < rounds; ++round) {
        int nops = 400;
        for (int i = 0; i < nops; ++i) {
            u64 k = rng() % 500 + 1;
            u32 op = (u32)(rng() % 100);
            rc = RC::OK;
            if (op < 25) {
                rc = e->del(k);
                if (rc == RC::OK) oracle.erase(k);
            } else {
                u32 v = (u32)rng();
                u16 len = (u16)(4 + (rng() % 40));
                u8 buf[64];
                std::memcpy(buf, &v, 4);
                for (u16 j = 4; j < len; ++j) buf[j] = (u8)(j * 31 + v);
                rc = e->put(k, buf, len);
                if (rc == RC::OK) oracle[k] = v;
            }
            if (rc != RC::OK && rc != RC::FULL) {
                std::printf("round %d op %d rc=%s\n", round, i, rc_str(rc));
                dump(*e, 0);
                return 1;
            }
            if (i % 37 == 36) { rc = e->commit(); if (rc != RC::OK) { std::printf("commit rc=%s\n", rc_str(rc)); dump(*e, 0); return 1; } }
        }
        rc = e->commit();
        if (rc != RC::OK) { std::printf("commit rc=%s\n", rc_str(rc)); dump(*e, 0); return 1; }
        // 校验读
        u32 bad = 0;
        for (auto& kv : oracle) {
            u8 buf[512]; u16 len = 0;
            RC g = e->get(kv.first, buf, sizeof buf, &len);
            if (g != RC::OK) { if (bad++ < 3) std::printf("  miss k=%llu rc=%s\n", (unsigned long long)kv.first, rc_str(g)); continue; }
            u32 v; std::memcpy(&v, buf, 4);
            if (v != kv.second && bad++ < 3) std::printf("  wrong k=%llu got=%u want=%u\n", (unsigned long long)kv.first, v, kv.second);
        }
        chk(bad == 0, "oracle");
        // 扫描键集
        u32 sc = 0; u64 prev = 0; bool ord = true;
        e->scan([&](u64 k, const u8*, u16) { if (sc && k <= prev) ord = false; prev = k; ++sc; return true; });
        if (sc != oracle.size()) std::printf("  scan count=%u oracle=%zu\n", sc, oracle.size());
        chk(sc == oracle.size(), "scan count");
        chk(ord, "scan order");
        Engine::VerifyReport rep;
        RC vr = e->verify(&rep);
        if (vr != RC::OK) {
            std::printf("round %d verify failed err=%u page=%u %s\n", round, rep.errors, rep.first_err_page, rep.first_err);
            dump(*e, 20);
            return 1;
        }
        std::printf("round %d ok: pages=%u leaves=%u inners=%u pubs=%u orphans=%u keys=%zu free=%u\n",
                    round, rep.pages, rep.leaves, rep.inners, rep.pubs, rep.orphans, oracle.size(), e->free_count());
        // 重开（挂载）
        if (round % 2 == 1) {
            e->~Engine();
            new (e) Engine();
            rc = e->open(dev, arena, NAV_CAP);
            if (rc != RC::OK) { std::printf("reopen rc=%s\n", rc_str(rc)); return 1; }
            u32 bad2 = 0;
            for (auto& kv : oracle) {
                u8 buf[512]; u16 len = 0;
                RC g = e->get(kv.first, buf, sizeof buf, &len);
                u32 v = 0; if (g == RC::OK) std::memcpy(&v, buf, 4);
                if (g != RC::OK || v != kv.second) { if (bad2++ < 3) std::printf("  reopen miss/wrong k=%llu rc=%s\n", (unsigned long long)kv.first, rc_str(g)); }
            }
            chk(bad2 == 0, "reopen oracle");
            std::printf("  reopened ok (vers=%u)\n", e->last_commit());
        }
    }
    std::printf("FAILURES: %d\n", failures);
    delete e; delete dev;   // 第 12 轮 LSan 常开（detect_leaks=1）：测具自己的 new 也得收（v2 Engine/RamDev 析构干净）
    return failures ? 1 : 0;
}
