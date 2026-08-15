#include "guidstream.hpp"

#include <cstring>
#include <stdexcept>

namespace guidstream {

namespace {
constexpr size_t RRSIZE = 312;               // sizeof(dmu_replay_record_t)
constexpr size_t CKSUM_OFF = RRSIZE - 32;    // 280: trailing zio_cksum_t
constexpr size_t TOGUID_OFF = 40;            // drr_begin.drr_toguid
constexpr size_t MAGIC_OFF = 8;              // drr_begin.drr_magic
constexpr size_t ENDSUM_OFF = 8;             // drr_end.drr_checksum
constexpr uint64_t DMU_BACKUP_MAGIC = 0x2F5bacbacULL;
enum { DRR_BEGIN = 0, DRR_END = 5 };

struct Cksum {
    uint64_t w[4];
};

// fletcher4, incremental, native byte order; `size` must be a multiple of 4.
//
// Strict aliasing: `buf` points into the std::string we rewrite, i.e. a char
// array, and we read it through a `uint32_t*`.  Reading a char array through a
// uint32_t lvalue is technically UB under the strict-aliasing rule (it is not the
// object's dynamic type), even though the *alignment* is fine here -- std::string
// data is suitably aligned and every offset we fold from (RRSIZE=312, CKSUM_OFF=280
// and 4-aligned payloads) preserves 4-byte alignment.  We keep the direct read
// because (a) OpenZFS itself computes fletcher4 exactly this way, (b) it compiles
// and runs correctly on the x86_64 targets this code is limited to, and (c) the
// interleaved `std::memcpy` stores in rewrite_toguid already act as optimization
// barriers against reordering.  The strictly-conforming alternative is to `memcpy`
// each word into a local `uint32_t`; switch to that if this is ever built for a
// platform/compiler where the aliasing assumption doesn't hold.
void fletcher4(const void* buf, size_t size, Cksum& zc) {
    const uint32_t* ip = static_cast<const uint32_t*>(buf);
    const uint32_t* end = ip + size / 4;
    uint64_t a = zc.w[0], b = zc.w[1], c = zc.w[2], d = zc.w[3];
    for (; ip < end; ++ip) {
        a += *ip; b += a; c += b; d += c;
    }
    zc.w[0] = a; zc.w[1] = b; zc.w[2] = c; zc.w[3] = d;
}
}  // namespace

std::string rewrite_toguid(const std::string& stream, uint64_t guid) {
    if (stream.size() < RRSIZE)
        throw std::runtime_error("send stream too short to be valid");

    std::string out = stream;  // rewrite in place on a copy
    char* p = out.data();
    Cksum zc{{0, 0, 0, 0}};
    size_t off = 0;
    bool first = true;

    while (off + RRSIZE <= out.size()) {
        char* rec = p + off;
        uint32_t type, payloadlen;
        std::memcpy(&type, rec + 0, 4);
        std::memcpy(&payloadlen, rec + 4, 4);

        if (first) {
            uint64_t magic;
            std::memcpy(&magic, rec + MAGIC_OFF, 8);
            if (type != DRR_BEGIN || magic != DMU_BACKUP_MAGIC)
                throw std::runtime_error("not a zfs send stream");
            std::memcpy(rec + TOGUID_OFF, &guid, 8);
        }
        // drr_toguid appears in two records: DRR_BEGIN (at TOGUID_OFF) and DRR_END
        // (`drr_end.drr_toguid`, at the same in-record offset).  A real `zfs send`
        // sets *both* to the same value.  We rewrite only BEGIN's, so after this the
        // BEGIN and END toguid fields disagree.  That is intentional and safe: the
        // receiver takes the *received snapshot's* GUID from DRR_BEGIN.drr_toguid
        // (module/zfs/dmu_recv.c), and validates DRR_END only via its checksum
        // (`drr_end.drr_checksum`) -- it never cross-checks drr_end.drr_toguid.  So
        // the recv accepts the stream and assigns our chosen GUID.  We deliberately
        // leave END's toguid untouched to keep this rewriter minimal (one field +
        // the checksum chain); if strict byte-for-byte fidelity with `zfs send` were
        // ever required (e.g. for a third-party receiver that inspected END's
        // toguid), also do `if (type == DRR_END) memcpy(rec + TOGUID_OFF, &guid, 8);`
        // -- it would fold into the same checksum recompute below at no extra cost.

        // Mirror dmu_send.c:dump_record exactly -- fold [0:CKSUM_OFF), store the
        // running sum in the trailing field (except BEGIN), then fold the trailing
        // 32 bytes so the whole record is chained.  DRR_END additionally carries
        // the running-so-far value in its drr_checksum field.
        if (type == DRR_END) std::memcpy(rec + ENDSUM_OFF, &zc, sizeof(zc));
        fletcher4(rec, CKSUM_OFF, zc);
        if (type != DRR_BEGIN) std::memcpy(rec + CKSUM_OFF, &zc, sizeof(zc));
        fletcher4(rec + CKSUM_OFF, sizeof(zc), zc);

        off += RRSIZE;
        if (payloadlen) {
            if (payloadlen % 4 != 0)
                throw std::runtime_error("send stream payload not 4-byte aligned");
            if (off + payloadlen > out.size())
                throw std::runtime_error("truncated send stream payload");
            fletcher4(p + off, payloadlen, zc);
            off += payloadlen;
        }
        first = false;
    }
    if (first) throw std::runtime_error("empty send stream");
    if (off != out.size())
        throw std::runtime_error("trailing bytes after final send record");
    return out;
}

}  // namespace guidstream
