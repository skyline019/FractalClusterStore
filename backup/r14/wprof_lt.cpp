// wprof 变体（第 12 次讨论）：第 5 参 lvl_t（1000 = 关掉层合并，只剩单元压实 ⇒ "cell-tiered" 策略的上界估算），第 6 参 level_merges
#include "fcs.h"
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace fcs;
static double now(){ return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(int argc,char**argv){
    int nkeys=argc>1?atoi(argv[1]):100000, vb=argc>2?atoi(argv[2]):100, commits=argc>3?atoi(argv[3]):150, per=argc>4?atoi(argv[4]):2000;
    static u8 arena[1024*PAGE];
    const char*P="build/wprof_lt.db"; std::remove(P); FileDev d(P,400000);
    Store s; if(s.open(&d,arena,1024)!=RC::OK){printf("open fail\n");return 1;}
    if(argc>5) s.set_lvl_t((u32)atoi(argv[5]));
    if(argc>6) s.set_level_merges((u32)atoi(argv[6]));
    int mode=argc>7?atoi(argv[7]):1; long budget=argc>8?atol(argv[8]):-1;     // 第 13 轮：第 7 参维护模式（1 / 3），第 8 参 mode 1 预算 µs（-1 = 引擎默认，0 = 不限）
    s.set_maint_mode(mode); if(budget>=0) s.set_maint_budget_us((u64)budget);
    std::vector<u8> v(vb,9); std::mt19937 rng(1);
    std::vector<double> ct; u64 logical=0;
    double t0=now();
    for(int c=0;c<commits;c++){
        for(int i=0;i<per;i++){ v[0]=(u8)i; s.put(rng()%nkeys+1, v.data(), vb); logical+=8+vb; }
        double a=now(); s.commit(); double b=now(); ct.push_back((b-a)*1e3);
    }
    double wall=now()-t0;
    { double w0=now(); while(s.maint_pending() && now()-w0<120){ if(mode==3) usleep(1000); else s.commit(); } }   // 欠账补完再算总墙钟（mode 3 由后台线程补；mode 1 用空提交触发）
    double wall_all=now()-t0;
    std::vector<double> so=ct; std::sort(so.begin(), so.end());
    double sum=0; for(double x:ct) sum+=x;
    // 回读抽查 20000 键
    std::vector<u8> out(70000); u16 len=0; u32 bad=0; std::mt19937 r2(7);
    for(int i=0;i<20000;i++){ u64 k=r2()%nkeys+1; RC rc=s.get(k,out.data(),65535,&len); if(rc==RC::OK && len!=vb) ++bad; if(rc!=RC::OK&&rc!=RC::NOTFOUND) ++bad; }
    Report rep; RC vr=s.verify(&rep);
    auto ms=s.maint_stats();
    printf("lvl_t=%s merges=%s mode=%d budget=%lld: avg %.1f ms p50 %.1f p90 %.1f max %.1f (>50ms: %zu) | wall %.1f s (+欠账 %.1f s) deferred=%llu | used=%u runs=%u cells=%u segmax=%u | 设备写 %.1f MB = 逻辑 %.1f MB × %.2f | verify=%s bad=%u\n",
        argc>5?argv[5]:"def", argc>6?argv[6]:"0", mode, (long long)s.maint_budget_us(), sum/commits, so[commits/2], so[commits*9/10], so.back(), (size_t)std::count_if(ct.begin(),ct.end(),[](double x){return x>50;}),
        wall, wall_all, (unsigned long long)ms.deferred, s.used_pages(), s.run_count(), s.cell_count(), s.max_segs_per_cell(), d.st.write_bytes.load()/1048576.0, logical/1048576.0, (double)d.st.write_bytes.load()/logical, rc_str(vr), rep.errors+bad);
    printf("  maint: units=%llu slices=%llu sweep %.0f ms rebal %.0f ms level %.0f ms recounts=%llu job_fast=%llu job_slow=%llu yields=%llu unk_left=%u\n",
        (unsigned long long)ms.units, (unsigned long long)ms.slices, ms.sweep_us/1e3, ms.rebal_us/1e3, ms.level_us/1e3, (unsigned long long)ms.recounts, (unsigned long long)ms.job_fast, (unsigned long long)ms.job_slow, (unsigned long long)ms.yields, s.unknown_cells());
    { auto h=s.lvl_hist(); printf("  lvl_hist:"); for(size_t i=0;i<h.size();i++) printf(" L%zu=%u", i, h[i]); printf("\n"); }
    std::remove(P);
}
