#ifndef __CHAMPSIM_WRAPPER_H
#define __CHAMPSIM_WRAPPER_H

#include <string>

#include "Config.h"

using namespace std;

namespace ramulator
{

class Request;
class MemoryBase;

class ChampSimWrapper
{
private:
    MemoryBase *mem;
    double tCK;

public:
    ChampSimWrapper(const Config& configs, int cacheline);
    ~ChampSimWrapper();
    void tick();
    bool send(Request req);
    void finish(void);
    void resetStats();
    double get_tCK();
};

} /*namespace ramulator*/

#endif /*__CHAMPSIM_WRAPPER_H*/
