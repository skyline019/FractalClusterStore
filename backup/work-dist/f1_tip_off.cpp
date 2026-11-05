#include "include/fcs.h"
#include <cstdio>
#include <map>
#include <random>
using namespace fcs;
static u8 arena[512 * PAGE];
int main(int argc, char** argv) {
    const bool tip = !(argc > 1 && argv[1][0] == '0');
    static RamDev dev(2048);
    static Store s;
    s.open(&dev, arena, 512);
    s.set_flush_tip(tip);
    std::printf("flush_tip=%d\n", tip ? 1 : 0);
    u8 buf[256];
    u16 len = 0;
    s.put(1, "hello", 5);
    s.commit();
    s.put(1, "HELLO", 5);
    s.put(2, "two", 3);
    s.commit();
    s.del(2);
    s.commit();
    std::mt19937_64 rng(7);
    std::map<u64, std::vector<u8>> oracle;
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 500; ++i) {
            u64 k = rng() % 3000 + 1;
            if (rng() % 8 == 0) {
                s.del(k);
                oracle.erase(k);
                continue;
            }
            u8 v[40];
            u32 x = (u32)rng();
            std::memcpy(v, &x, 4);
            u16 l = (u16)(4 + rng() % 30);
            for (u16 j = 4; j < l; ++j) v[j] = (u8)(x + j);
            s.put(k, v, l);
            oracle[k].assign(v, v + l);
        }
        s.commit();
        Report rep;
        RC vr = s.verify(&rep);
        std::printf("round %d verify=%s err=%u(%s)\n", round, rc_str(vr), rep.errors,
                    rep.errors ? rep.first_err : "-");
        if (vr != RC::OK) return 1;
    }
    std::printf("PASS\n");
    return 0;
}
