#include "store.h"
#include <cstdio>
#include <map>
#include <random>
using namespace xdb;

static u8 arena[NAV_CAP * PAGE];

static void dump_err(Engine& e) {
    Engine::VerifyReport r;
    e.verify(&r);
    std::printf("  verify: pages=%u leaves=%u inners=%u pubs=%u orphans=%u errors=%u first=%u(%s)\n",
                r.pages, r.leaves, r.inners, r.pubs, r.orphans, r.errors, r.first_err_page, r.first_err);
}

int main() {
    RamDev dev(1024);
    Engine e;
    RC rc = e.open(&dev, arena, NAV_CAP);
    std::printf("open rc=%s root=%u\n", rc_str(rc), e.root());
    if (rc != RC::OK) return 1;
    // basic
    for (int i = 0; i < 100; ++i) {
        uint32_t v = (uint32_t)(i * 7 + 1);
        rc = e.put((uint64_t)(i + 1), &v, 4);
        if (rc != RC::OK) { std::printf("put %d rc=%s\n", i, rc_str(rc)); dump_err(e); return 1; }
    }
    rc = e.commit();
    std::printf("commit rc=%s vers=%u\n", rc_str(rc), e.last_commit());
    for (int i = 0; i < 100; ++i) {
        uint8_t buf[64]; uint16_t len = 0;
        rc = e.get((uint64_t)(i + 1), buf, sizeof buf, &len);
        uint32_t v = 0; std::memcpy(&v, buf, 4);
        if (rc != RC::OK || v != (uint32_t)(i * 7 + 1)) {
            std::printf("get %d rc=%s v=%u want=%u\n", i, rc_str(rc), v, (unsigned)(i * 7 + 1));
            dump_err(e); return 1;
        }
    }
    dump_err(e);
    std::printf("basic ok\n");
    return 0;
}
