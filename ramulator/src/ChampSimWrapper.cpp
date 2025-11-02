#include <map>

#include "ChampSimWrapper.h"
#include "Config.h"
#include "Request.h"
#include "MemoryFactory.h"
#include "Memory.h"
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

static map<string, function<MemoryBase *(const Config &, int)>> name_to_func = {
    {"DDR3", &MemoryFactory<DDR3>::create},       {"DDR4", &MemoryFactory<DDR4>::create},
    {"LPDDR3", &MemoryFactory<LPDDR3>::create},   {"LPDDR4", &MemoryFactory<LPDDR4>::create},
    {"GDDR5", &MemoryFactory<GDDR5>::create},     {"WideIO", &MemoryFactory<WideIO>::create},
    {"WideIO2", &MemoryFactory<WideIO2>::create}, {"HBM", &MemoryFactory<HBM>::create},
    {"SALP-1", &MemoryFactory<SALP>::create},     {"SALP-2", &MemoryFactory<SALP>::create},
    {"SALP-MASA", &MemoryFactory<SALP>::create},
};

ChampSimWrapper::ChampSimWrapper(const Config& configs, int cacheline)
{
    const string &std_name = configs["standard"];
    assert(name_to_func.find(std_name) != name_to_func.end() && "unrecognized standard name");
    mem = name_to_func[std_name](configs, cacheline);
    tCK = mem->clk_ns();

    if (configs.contains("stats_dir")) {
        printf("stats_dir: %s\n",
                (configs["stats_dir"]).c_str());
        RamulatorStats::statlist.output(configs["stats_dir"]);
    } else {
        RamulatorStats::statlist.output(configs["standard"] + ".stats");
    }
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
  std::cout << "[RAMULATOR] Finished Ramulator" << std::endl;
  mem->finish();
  RamulatorStats::statlist.printall();
}

void ChampSimWrapper::resetStats() {
    RamulatorStats::reset_stats();
    mem->resetStats();
}

double ChampSimWrapper::get_tCK() {
    return tCK;
}
