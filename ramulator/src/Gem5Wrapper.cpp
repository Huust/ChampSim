#include <map>

#include "Config.h"
#include "DDR3.h"
#include "DDR4.h"
#include "GDDR5.h"
#include "Gem5Wrapper.h"
#include "HBM.h"
#include "LPDDR3.h"
#include "LPDDR4.h"
#include "Memory.h"
#include "MemoryFactory.h"
#include "Request.h"
#include "SALP.h"
#include "Statistics.h"
#include "WideIO.h"
#include "WideIO2.h"

using namespace ramulator;

static map<string, function<MemoryBase *(const Config &, int, StatContext*)>> name_to_func = {
    {"DDR3", &MemoryFactory<DDR3>::create},       {"DDR4", &MemoryFactory<DDR4>::create},
    {"LPDDR3", &MemoryFactory<LPDDR3>::create},   {"LPDDR4", &MemoryFactory<LPDDR4>::create},
    {"GDDR5", &MemoryFactory<GDDR5>::create},     {"WideIO", &MemoryFactory<WideIO>::create},
    {"WideIO2", &MemoryFactory<WideIO2>::create}, {"HBM", &MemoryFactory<HBM>::create},
    {"SALP-1", &MemoryFactory<SALP>::create},     {"SALP-2", &MemoryFactory<SALP>::create},
    {"SALP-MASA", &MemoryFactory<SALP>::create},
};

Gem5Wrapper::Gem5Wrapper(const Config &configs, int cacheline) {
    const string &std_name = configs["standard"];
    assert(name_to_func.find(std_name) != name_to_func.end() && "unrecognized standard name");
    mem = name_to_func[std_name](configs, cacheline, nullptr);
    tCK = mem->clk_ns();

    assert(configs.contains("gem5_channel_id") && "channel_id is not set!");

    channel_id = stoi(configs["gem5_channel_id"]);

    if (configs.contains("stats_dir")) {
        if (channel_id == 0) {
            printf("stats_dir: %s\n",
                   (configs["stats_dir"] + "/ramulator_stats_channel" + ".txt").c_str());
            RamulatorStats::statlist.output(configs["stats_dir"] + "/ramulator_stats_channel" +
                                   ".txt");
        } else {
            printf("Common statsfile for channel = %s\n", configs["gem5_channel_id"].c_str());
        }
    } else {
        if (channel_id == 0) {
            RamulatorStats::statlist.output(configs["standard"] + ".stats");
        } else {
            printf("Common statsfile for channel = %s\n", configs["gem5_channel_id"].c_str());
        }
    }
}

Gem5Wrapper::~Gem5Wrapper() { delete mem; }

void Gem5Wrapper::tick() { mem->tick(); }

bool Gem5Wrapper::send(Request req) { 
    return mem->send(req);
}

void Gem5Wrapper::finish(void) {
    mem->finish();
    if(channel_id == 0) {
        RamulatorStats::statlist.printall();
    }
}

void Gem5Wrapper::resetStats() {
    RamulatorStats::reset_stats();
    mem->resetStats();
}
