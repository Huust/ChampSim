#ifndef __CHAMPSIM_WRAPPER_H
#define __CHAMPSIM_WRAPPER_H

#include <string>
#include <memory>

#include "Config.h"

using namespace std;

namespace ramulator
{

class Request;
class MemoryBase;
class StatContext;

class ChampSimWrapper
{
private:
    MemoryBase *mem;
    double tCK;
    std::unique_ptr<StatContext> stat_context_;

public:
    ChampSimWrapper(const Config& configs, int cacheline);
    ~ChampSimWrapper();
    void tick();
    bool send(Request req);
    void finish(void);
    void resetStats();
    double get_tCK();
    long get_capacity();

    // Get the statistics context for this instance
    StatContext* get_stat_context() { return stat_context_.get(); }
};

} /*namespace ramulator*/

#endif /*__CHAMPSIM_WRAPPER_H*/
