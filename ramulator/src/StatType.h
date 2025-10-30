#ifndef __STATTYPE_H
#define __STATTYPE_H

#include <cstdint>
#include <string>

/*
  SIMPLIFIED STATTYPE HEADER

  This provides compatibility definitions for the simplified statistics
  implementation. The complex RamulatorStats namespace has been removed
  and replaced with simple flag constants for backward compatibility.
*/

namespace ramulator {
// Forward declarations for compatibility
class ScalarStat;
class AverageStat;
class VectorStat;
class AverageVectorStat;
} // namespace ramulator

// Backward compatibility namespace
namespace RamulatorStats {

// Flag constants for compatibility
const uint16_t init      = 0x00000001;
const uint16_t display   = 0x00000002;
const uint16_t total     = 0x00000010;
const uint16_t pdf       = 0x00000020;
const uint16_t cdf       = 0x00000040;
const uint16_t dist      = 0x00000080;
const uint16_t nozero    = 0x00000100;
const uint16_t nonan     = 0x00000200;

// Global tick counter for compatibility
extern uint64_t curTick;

// Simple flags class for compatibility
class Flags {
public:
    uint16_t flags;
    
    Flags() : flags(display) {}
    Flags(uint16_t f) : flags(f) {}
    void operator=(uint16_t f) { flags = f; }
    bool is_total() const { return flags & total; }
    bool is_pdf() const { return flags & pdf; }
    bool is_nozero() const { return flags & nozero; }
    bool is_nonan() const { return flags & nonan; }
    bool is_cdf() const { return flags & cdf; }
    bool is_display() const { return flags & display; }
};

// Simple StatList class for compatibility
class StatList {
private:
    std::string output_filename;
public:
    void output(const std::string& filename);
    void printall();
    void add(void* /*stat*/) {} // Simplified - not used in new implementation
};

// Global statlist instance for compatibility
extern StatList statlist;

// Reset stats function for compatibility
void reset_stats();

} // namespace RamulatorStats

#endif