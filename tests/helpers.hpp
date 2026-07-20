// Test helpers for driving zvols and comparing their contents.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace th {

// Run `zfs <args...>`, throwing on failure.
void zfs(const std::vector<std::string>& args);

std::string get_prop(const std::string& dataset, const std::string& prop);
uint64_t volsize(const std::string& dataset);
bool dataset_exists(const std::string& dataset);

// Path to a zvol/snapshot device, waited for until it exists.
std::string dev_path(const std::string& dataset);

void snapshot(const std::string& dataset, const std::string& name);
void set_volsize(const std::string& dataset, uint64_t size);
std::vector<std::string> snapshot_names(const std::string& dataset);

// Create a source zvol under the current test pool (snapdev=visible); returns its
// full dataset name.
std::string make_zvol(const std::string& volblocksize, const std::string& size,
                      const std::string& name = "src");

// Write `size` deterministic bytes derived from `seed` at `offset`.
void write_pattern(const std::string& dataset, uint64_t offset, uint64_t size,
                   uint64_t seed);
void punch_hole(const std::string& dataset, uint64_t offset, uint64_t length);
std::vector<uint8_t> read_range(const std::string& dataset, uint64_t offset,
                                uint64_t size);
// Byte-for-byte comparison of the first `size` bytes of two datasets' devices.
bool devices_equal(const std::string& a, const std::string& b, uint64_t size);

// Run the tool under test; returns its exit code.
int migrate(const std::string& source, const std::string& volblocksize,
            const std::vector<std::string>& extra = {});

}  // namespace th
