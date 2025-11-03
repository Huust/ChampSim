#include <map>

#include "ChampSimWrapper.h"
#include "Config.h"
#include "Request.h"
#include "MemoryFactory.h"
#include "Memory.h"
#include "Statistics.h"
#include "DDR3.h"
#include "DDR4.h"
#include "LPDDR3.h"
#include "LPDDR4.h"
#include "GDDR5.h"
#include "WideIO.h"
#include "WideIO2.h"
#include "HBM.h"
#include "SALP.h"

using namespace ramulator;

static map<string, function<MemoryBase *(const Config &, int, StatContext*)>> name_to_func = {
    {"DDR3", &MemoryFactory<DDR3>::create},       {"DDR4", &MemoryFactory<DDR4>::create},
    {"LPDDR3", &MemoryFactory<LPDDR3>::create},   {"LPDDR4", &MemoryFactory<LPDDR4>::create},
    {"GDDR5", &MemoryFactory<GDDR5>::create},     {"WideIO", &MemoryFactory<WideIO>::create},
    {"WideIO2", &MemoryFactory<WideIO2>::create}, {"HBM", &MemoryFactory<HBM>::create},
    {"SALP-1", &MemoryFactory<SALP>::create},     {"SALP-2", &MemoryFactory<SALP>::create},
    {"SALP-MASA", &MemoryFactory<SALP>::create},
};

ChampSimWrapper::ChampSimWrapper(const Config& configs, int cacheline)
{
    // Create stat context for this instance
    stat_context_ = std::make_unique<StatContext>();

    const string &std_name = configs["standard"];
    assert(name_to_func.find(std_name) != name_to_func.end() && "unrecognized standard name");
    mem = name_to_func[std_name](configs, cacheline, stat_context_.get());
    tCK = mem->clk_ns();

    // Note: Statistics output configuration
    // By default, Ramulator statistics are printed to stdout (command line) after simulation.
    // To enable file output, modify StatContext::print_stats() in StatType.cpp.
    // The stats_dir config below will be used when file output is enabled.
    if (configs.contains("stats_dir")) {
        std::string output_file = configs["stats_dir"];
        stat_context_->set_output_filename(output_file);
        // File output is currently disabled - statistics will print to stdout
    } else {
        std::string output_file = configs["standard"] + ".stats";
        stat_context_->set_output_filename(output_file);
    }
    printf("[RAMULATOR] %s initialized. Statistics will be printed to stdout after simulation.\n", std_name.c_str());
}


ChampSimWrapper::~ChampSimWrapper() {
    delete mem;
}

void ChampSimWrapper::tick() {
    mem->tick();
}

bool ChampSimWrapper::send(Request req) {
    return mem->send(req);
}

void ChampSimWrapper::finish() {
  mem->finish();
  // Use instance-specific stat context instead of global
  stat_context_->print_stats();
}

void ChampSimWrapper::resetStats() {
    // Use instance-specific stat context instead of global
    stat_context_->reset_stats();
    mem->resetStats();
}

double ChampSimWrapper::get_tCK() {
    return tCK;
}

long ChampSimWrapper::get_capacity() {
    return mem->get_max_address();
}
