#include "fcs.h"
#include "blockdev.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <thread>
#include <atomic>
#include <algorithm>
using namespace xdb; using namespace fcs;
static double now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main(int argc,char**argv){
    int nkeys=argc>1?atoi(argv[1]):100000, vb=argc>2?atoi(argv[2]):100, secs=argc>3?atoi(argv[3]):4, maint_every=argc>4?atoi(argv[4]):20;
    int units_max=argc>5?atoi(argv[5]):0;     // 第 5 参（第 12 轮）：每次 commit() 之后最多跑多少个维护单元（0 = 引擎默认 256）
    int mmode=argc>6?atoi(argv[6]):1; long budget=argc>7?atol(argv[7]):-1;   // 第 6/7 参（第 13 轮）：维护模式（1 / 3）、mode 1 预算 µs（-1 = 引擎默认，0 = 不限）
    int pace_ms=argc>8?atoi(argv[8]):0;        // 第 8 参（第 14 轮）：写者节拍——每次提交之间至少隔这么多 ms（0 = 尽力写）。A/B 比读者尾延迟时必须定节拍，否则写者变快 = 读者撞到更多提交
    static u8 arena[1024*PAGE];
    const char*P="build/stall.db"; std::remove(P); FileDev d(P,400000);
    Store s; if(s.open(&d,arena,1024)!=RC::OK){printf("open fail\n");return 1;}
    s.set_shared_reads(true);
#ifndef STALL_OLDAPI
    if(units_max>0) s.set_maint_units_max((u32)units_max);
    s.set_maint_mode(mmode); if(budget>=0) s.set_maint_budget_us((u64)budget);
#endif
    std::vector<u8> v(vb,9); std::mt19937 rng(1);
    for(int i=0;i<2*nkeys;i++){ v[0]=(u8)i; s.put(rng()%nkeys+1, v.data(), vb); if(i%2000==1999) s.commit(); }
    s.commit();
    std::atomic<bool> stop{false};
    std::vector<double> lat; lat.reserve(4000000);
    double get_max=0; size_t get_over1=0, get_over10=0;            // 单次 get 的最长等待 / >1 ms / >10 ms 次数（L1/L2 分片看这个）
    std::thread rd([&]{ std::mt19937 r(5); u8 buf[4096]; u16 len; double tprev=now();
        while(!stop){ for(int i=0;i<64;i++){ double g0=now(); s.get(r()%nkeys+1, buf, sizeof buf, &len); double g=now()-g0; if(g>get_max) get_max=g; if(g>0.001) get_over1++; if(g>0.01) get_over10++; } double t=now(); lat.push_back((t-tprev)/64); tprev=t; } });
    double t0=now(); int commits=0, maints=0; double maint_max=0, maint_sum=0, commit_max=0;
    if(maint_every<0){ std::this_thread::sleep_for(std::chrono::seconds(secs)); }
    while(now()-t0<secs){
        double cyc0=now();
        for(int i=0;i<2000;i++){ v[0]=(u8)i; s.put(rng()%nkeys+1, v.data(), vb); }
        double a=now(); s.commit(); double b=now(); commit_max=std::max(commit_max,b-a); commits++;
        if(pace_ms>0){ double left=pace_ms/1e3-(now()-cyc0); if(left>0) std::this_thread::sleep_for(std::chrono::microseconds((long)(left*1e6))); }
        if(maint_every>0 && commits%maint_every==0){ double c=now(); RC rc=s.consolidate(); double e=now(); maints++; maint_max=std::max(maint_max,e-c); maint_sum+=e-c; if(rc!=RC::OK) printf("maint rc=%s\n", rc_str(rc)); }
    }
    stop=true; rd.join();
    double cm=0; { double w0=now(); while(s.maint_pending() && now()-w0<60){ if(mmode==3) std::this_thread::sleep_for(std::chrono::milliseconds(1)); else s.commit(); } cm=now()-w0; }   // 把欠的维护补完（mode 3 后台线程；mode 1 空提交）
    std::sort(lat.begin(), lat.end());
    auto pct=[&](double p){ return lat[(size_t)std::min<double>(lat.size()-1, p*lat.size())]*1e6; };
    size_t stalls=0; for(double x:lat) if(x*64>0.005) stalls++;
    auto ms=s.maint_stats();
    printf("keys=%d vb=%d mode=%d budget=%lld  %d commits (max %.1f ms)  %d consolidate (max %.1f ms, avg %.1f ms)  used=%u 页  runs=%u deferred=%llu 补欠账 %.1f s\n", nkeys, vb, mmode, (long long)s.maint_budget_us(), commits, commit_max*1e3, maints, maint_max*1e3, maints? maint_sum/maints*1e3:0, s.used_pages(), s.run_count(), (unsigned long long)ms.deferred, cm);
    printf("reader: %zu batches×64 get  per-get p50 %.2f µs  p99 %.2f µs  p99.9 %.1f µs  max batch %.1f ms  batches>5ms=%zu\n", lat.size(), pct(0.5), pct(0.99), pct(0.999), lat.back()*64*1e3, stalls);
    printf("reader single get: max %.1f ms  >1ms %zu  >10ms %zu\n", get_max*1e3, get_over1, get_over10);
    printf("maint: units=%llu yields=%llu unit_max %.1f ms units>4ms=%llu sweep %.0f rebal %.0f level %.0f ms job_fast=%llu job_slow=%llu\n", (unsigned long long)ms.units, (unsigned long long)ms.yields, ms.unit_max_us/1e3, (unsigned long long)ms.unit_long, ms.sweep_us/1e3, ms.rebal_us/1e3, ms.level_us/1e3, (unsigned long long)ms.job_fast, (unsigned long long)ms.job_slow);
    std::remove(P);
}
