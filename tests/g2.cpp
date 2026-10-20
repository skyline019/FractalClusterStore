// g2：大量范围墓碑 + 存活账。两件事：
//  1) 墓碑是"逻辑删除"，一个字节都没重写 ⇒ 单元的 live/dead 必须重算，
//     否则 c->live 与 verify 的口径分家（`live mismatch`），这些单元也永远不算有垃圾。
//  2) range_hidden 不能是 O(墓碑数)：每个条目带"前缀最大 hi"，往回走一旦 maxhi <= k 就停。
//  5000 个墓碑下点查仍是 0.3 µs 量级。
#include "fcs.h"
#include <cstdio>
#include <chrono>
#include <vector>
using namespace fcs;
static u8 arena[512*PAGE];
static double now(){ return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(){
    const char* P="build/rt2.db"; std::remove(P);
    FileDev fd(P, 20000);
    Store s; s.open(&fd, arena, 512);
    u8 v[16]={0}; 
    for (u64 k=1;k<=20000;++k) s.put(k, v, 8);
    s.commit();
    double t0=now();
    for (u64 k=1;k<=5000;++k) s.del_range(k*4, k*4+1);   // 5000 个墓碑
    s.commit();
    double t1=now();
    int hit=0;
    u8 b[8]; u16 l;
    double t2=now();
    for (int r=0;r<20000;++r){ u64 k=(u64)(r*7919%20000)+1; if (s.get(k,b,sizeof b,&l)==RC::OK) ++hit; }
    double t3=now();
    printf("墓碑 %u 个  插入 %.1f ms  get %.2f µs/次 (hit=%d)\n",
        s.range_tombstones(), (t1-t0)*1e3, (t3-t2)*1e6/20000, hit);
    Report rep; s.verify(&rep);
    printf("verify=%u(%s)\n", rep.errors, rep.first_err);
    if (rep.errors) return 1;
    // 墓碑盖住的记录必须算"死"：归一之后它们应该真的消失，并且空间回收不动摇
    u32 used0 = s.used_pages();
    s.consolidate(0);
    Report rep2; s.verify(&rep2);
    int vis = 0;
    s.select(0, ~0ull, [&](u64, const void*, u16) { ++vis; return true; });
    printf("归一后：used %u→%u 可见=%d 墓碑=%u verify=%u(%s)\n", used0, s.used_pages(), vis,
           s.range_tombstones(), rep2.errors, rep2.first_err);
    if (rep2.errors || vis != 15000) return 1;
    printf("PASS\n");
    return 0;
}
