#ifndef __REQUEST_H
#define __REQUEST_H

#include <vector>
#include <functional>

using namespace std;

namespace ramulator
{

class Request
{
public:
    static constexpr size_t ADDR_VEC_SIZE = 8; // More than enough for all DRAM types

    bool is_first_command;
    long addr;
    // long addr_row;
    std::array<int, ADDR_VEC_SIZE> addr_vec = {};
    // specify which core this request sent from, for virtual address translation
    int coreid;
    bool is_found_queue = false;

    enum class Type
    {
        READ,
        WRITE,
        REFRESH,
        POWERDOWN,
        SELFREFRESH,
        EXTENSION,
        MAX
    } type;

    long arrive = -1;
    long depart = -1;
    function<void(Request&)> callback; // call back with more info

    Request(long addr, Type type, int coreid = 0)
        : is_first_command(true), addr(addr), coreid(coreid), type(type),
      callback([](Request& req){}) {}

    Request(long addr, Type type, function<void(Request&)> callback, int coreid = 0)
        : is_first_command(true), addr(addr), coreid(coreid), type(type), callback(callback) {}

    Request(std::array<int, ADDR_VEC_SIZE> addr_vec, Type type, function<void(Request&)> callback, int coreid = 0)
        : is_first_command(true), addr_vec(addr_vec), coreid(coreid), type(type), callback(callback) {}

    Request()
        : is_first_command(true), coreid(0), type(Type::MAX), callback([](Request& req){}) {}
};

} /*namespace ramulator*/

#endif /*__REQUEST_H*/

