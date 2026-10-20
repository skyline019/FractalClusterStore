// 第 14 轮：文件高水位探针——bench4 同款负载（随机键插入 N 条、每 B 条提交、值 VSZ；再覆盖 N/2），
// 逐次提交盯 tail_ 的增长：每次高水位抬升时打印 (提交号, 抬了多少页, 当时最大的洞, 洞总数/总页, pending_free 页数, 各层 run 数)，
// 结束时打印 run 布局（按地址）与洞的直方图。用法：build/layout [N=200000] [B=500] [VSZ=32] [mode=1]
#define private public
#include "fcs.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <map>
using namespace fcs;
static u64 mix(u64 x){ x^=x>>33; x*=0xff51afd7ed558ccdULL; x^=x>>33; x*=0xc4ceb9fe1a85ec53ULL; x^=x>>33; return x; }
int main(int argc,char**argv){
    u32 N=argc>1?(u32)atoi(argv[1]):200000, B=argc>2?(u32)atoi(argv[2]):500, VSZ=argc>3?(u32)atoi(argv[3]):32; int mode=argc>4?atoi(argv[4]):1;
    int quiet=argc>5?atoi(argv[5]):0; int compact=argc>6?atoi(argv[6]):0; int rpct=argc>7?atoi(argv[7]):0;   // compact=1 ⇒ 结束后再调一次 compact_file()（量它的代价与收益）
    const char*P="build/layout.db"; std::remove(P);
    FileDev dev(P,(PageId)((N*(VSZ+24)+8*1024*1024)/PAGE+4096));
    Store s; if(s.open(&dev,nullptr,2048)!=RC::OK){printf("open fail\n");return 1;}
    s.set_maint_mode(mode); if(rpct) s.set_hw_reclaim_pct((u32)rpct);
    std::vector<u64> keys(N); for(u32 i=0;i<N;i++) keys[i]=mix(i+1)%(N*8)+1;
    std::vector<u8> val(VSZ,7), val2(VSZ,9);
    u32 tail_prev=s.tail_; u32 commits=0, grows=0; u64 grown=0; u32 peak=0;
    auto holes=[&](u32& cnt,u64& tot,u32& mx){ cnt=0;tot=0;mx=0; for(auto&kv:s.free_){ if(kv.first>=s.tail_) continue; u32 n=kv.second; if(kv.first+n>s.tail_) n=s.tail_-kv.first; cnt++; tot+=n; if(n>mx) mx=n; } };
    auto report=[&](const char* tag){
        u32 hc; u64 ht; u32 hm; holes(hc,ht,hm);
        u64 pf=0; for(auto&p:s.pending_free_) pf+=p.second;
        auto h=s.lvl_hist();
        if(s.tail_>tail_prev){ grows++; grown+=s.tail_-tail_prev; }
        if(s.tail_>peak) peak=s.tail_;
        if(!quiet && s.tail_!=tail_prev) printf("%s#%u tail %u -> %u (+%u)  holes %u/%llu pg max %u  pending_free %llu  runs L0=%u L1=%u L2=%u L3=%u live_top=%u\n", tag, commits, tail_prev, s.tail_, s.tail_-tail_prev, hc,(unsigned long long)ht,hm,(unsigned long long)pf,h[0],h[1],h[2],h[3],s.live_top());
        tail_prev=s.tail_;
    };
    for(u32 i=0;i<N;i++){ s.put(keys[i],val.data(),(u16)VSZ); if((i+1)%B==0){ s.commit(); commits++; report("ins "); } }
    s.commit(); commits++; report("ins ");
    printf("== after insert: tail=%u live_pages=%llu used=%u runs=%u grows=%u grown=%llu\n", s.tail_, (unsigned long long)s.live_pages(), s.used_pages(), s.run_count(), grows,(unsigned long long)grown);
    for(u32 i=0;i<N/2;i++){ u64 k=keys[mix(i+999)%N]; s.put(k,val2.data(),(u16)VSZ); if((i+1)%B==0){ s.commit(); commits++; report("upd "); } }
    s.commit(); commits++; report("upd ");
    { double w0=prog_us(); while(s.maint_pending() && prog_us()-w0<60e6){ if(mode==3) usleep(1000); else s.commit(); } s.commit(); s.commit(); }   // 末尾两个空提交：让最后一批释放页过提交点（pending → free_，顶部回落）
    report("fin ");
    { auto ms=s.maint_stats(); printf("== after update: tail=%u peak_tail=%u live_pages=%llu used=%u runs=%u grows=%u grown=%llu  hw %.1f MB live %.1f MB (%.2fx) peak %.2fx  stream runs=%llu moves=%llu extends=%llu reclaims=%llu  dev writes %.1f MB\n", s.tail_, peak, (unsigned long long)s.live_pages(), s.used_pages(), s.run_count(), grows,(unsigned long long)grown, s.tail_*4096/1048576.0, s.live_pages()*4096/1048576.0, (double)s.tail_/std::max<u64>(1,s.live_pages()), (double)peak/std::max<u64>(1,s.live_pages()), (unsigned long long)ms.stream_runs, (unsigned long long)ms.stream_moves, 
#ifdef LAYOUT_OLD
        0ull, 0ull,
#else
        (unsigned long long)ms.stream_extends, (unsigned long long)ms.hw_reclaims,
#endif
        dev.st.write_bytes.load()/1048576.0); }
    // run 布局
    std::vector<std::pair<u32,Run*>> rs; for(auto&kv:s.runs_) rs.push_back({kv.first,kv.second}); std::sort(rs.begin(),rs.end());
    printf("== runs by address (page0 npages lvl seq):\n");
    u32 prev_end=2; for(auto&pr:rs){ Run* r=pr.second; u32 blk=s.run_block(r); if(pr.first>prev_end) printf("   [hole %u pg]\n", pr.first-prev_end); printf("   %7u %6u L%u seq=%u\n", pr.first, blk, r->lvl, r->seq); prev_end=pr.first+blk; }
    if(s.tail_>prev_end) printf("   [hole %u pg to tail]\n", s.tail_-prev_end);
    std::map<u32,u32> hh; for(auto&kv:s.free_){ if(kv.first>=s.tail_) continue; u32 n=kv.second; if(kv.first+n>s.tail_) n=s.tail_-kv.first; u32 b=1; while(b*2<=n) b*=2; hh[b]++; }
    printf("== free holes by size class:"); for(auto&kv:hh) printf(" [%u,%u):%u", kv.first, kv.first*2, kv.second); printf("\n");
    if(compact){
        double w0=dev.st.write_bytes.load()/1048576.0; double t0=prog_us();
        RC rc = compact==2 ? (s.relocate()==RC::OK ? s.shrink() : RC::BUSY) : s.compact_file();
        printf("== %s: rc=%d tail=%u live_pages=%llu (%.2fx)  extra writes %.1f MB  %.0f ms\n", compact==2?"relocate+shrink":"compact_file", (int)rc, s.tail_, (unsigned long long)s.live_pages(), (double)s.tail_/std::max<u64>(1,s.live_pages()), dev.st.write_bytes.load()/1048576.0-w0, (prog_us()-t0)/1000.0);
    }
    std::remove(P); return 0;
}
