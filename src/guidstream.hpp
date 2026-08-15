// Rewrite the DRR_BEGIN toguid of a (small, in-memory) ZFS send stream and
// recompute its fletcher4 checksums exactly the way module/zfs/dmu_send.c does,
// so `zfs recv` accepts the result and assigns the chosen GUID to the received
// snapshot.  Used only for the tiny data-less incremental behind --align-guids.
//
// Same-endianness only (x86_64 -> x86_64); rewrites the first DRR_BEGIN.
#pragma once

#include <cstdint>
#include <string>

namespace guidstream {

// Returns a copy of `stream` with the first DRR_BEGIN.drr_toguid set to `guid`
// and all checksums recomputed.  Throws std::runtime_error if the input is not a
// well-formed send stream.
std::string rewrite_toguid(const std::string& stream, uint64_t guid);

}  // namespace guidstream
