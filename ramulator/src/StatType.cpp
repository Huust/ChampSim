#include "Statistics.h"

namespace ramulator {

// Global variables for statistics (for backward compatibility)
std::vector<StatBase*> all_stats_global;
std::ofstream stats_output;
uint64_t current_tick = 0;

// Function to get the global stats vector
std::vector<StatBase*>& get_all_stats() {
    return all_stats_global;
}

// StatContext member functions
void StatContext::reset_stats() {
    for (auto* stat : all_stats_) {
        if (stat) {
            stat->reset();
        }
    }
}

void StatContext::print_stats() {
    // NOTE: By default, statistics are printed to stdout (command line).
    // To enable file output instead, uncomment the file output code below
    // and comment out the stdout section. When file output is enabled,
    // the "stats_output_dir" setting in JSON config will be used.

    // ========== STDOUT OUTPUT (Active by default) ==========
    std::ostream& output = std::cout;

    output << std::endl;
    output << "---------- Begin Ramulator Statistics ----------" << std::endl;

    int displayed_count = 0;
    for (auto* stat : all_stats_) {
        if (stat) {
            stat->prepare();
            stat->print(output);
            displayed_count++;
        }
    }

    output << "# Displayed " << displayed_count << " Ramulator statistics" << std::endl;
    output << std::endl;
    output << "---------- End Ramulator Statistics ----------" << std::endl;

    // ========== FILE OUTPUT (Commented out - uncomment to enable) ==========
    // std::string filename = output_filename_.empty() ? "ramulator.stats" : output_filename_;
    // std::ofstream output_file(filename.c_str(), std::ios_base::out);
    // if (!output_file.good()) {
    //     std::cerr << "Error: Could not open statistics output file: " << filename << std::endl;
    //     return;
    // }
    //
    // output_file << std::endl;
    // output_file << "---------- Begin Ramulator Statistics ----------" << std::endl;
    //
    // int displayed_count = 0;
    // for (auto* stat : all_stats_) {
    //     if (stat) {
    //         stat->prepare();
    //         stat->print(output_file);
    //         displayed_count++;
    //     }
    // }
    //
    // output_file << "# Displayed " << displayed_count << " Ramulator statistics" << std::endl;
    // output_file << std::endl;
    // output_file << "---------- End Ramulator Statistics ----------" << std::endl;
    // output_file.close();
}

void reset_stats() {
    for (auto* stat : get_all_stats()) {
        if (stat) {
            stat->reset();
        }
    }
}

void print_stats(const std::string& filename) {
    stats_output.open(filename.c_str(), std::ios_base::out);
    if (!stats_output.good()) {
        std::cerr << "Error: Could not open statistics output file: " << filename << std::endl;
        return;
    }

    auto& all_stats = get_all_stats();
    // std::cerr << "DEBUG: About to print " << all_stats.size() << " stats" << std::endl;

    stats_output << std::endl;
    stats_output << "---------- Begin Simulation Statistics ----------" << std::endl;
    
    int displayed_count = 0;
    for (auto* stat : all_stats) {
        if (stat) {
            stat->prepare();
            stat->print(stats_output);
            displayed_count++;
        }
    }
    
    stats_output << "# Displayed " << displayed_count << " statistics" << std::endl;
    stats_output << std::endl;
    stats_output << "---------- End Simulation Statistics ----------" << std::endl;
    stats_output.close();
}} // namespace ramulator

// For backward compatibility with the global RamulatorStats namespace
namespace RamulatorStats {
    uint64_t curTick = 0;
    
    // StatList implementation for compatibility
    StatList statlist;
    
    void StatList::output(const std::string& filename) {
        output_filename = filename;  // Just store the filename for later use
    }
    
    void StatList::printall() {
        if (output_filename.empty()) {
            ramulator::print_stats("ramulator.stats");  // Default filename
        } else {
            ramulator::print_stats(output_filename);  // Use stored filename
        }
    }
    
    void reset_stats() {
        ramulator::reset_stats();
    }
}
