#ifndef __CONTROLLER_H
#define __CONTROLLER_H

#include <cassert>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <list>
#include <sstream>
#include <string>
#include <vector>

#include "Config.h"
#include "DRAM.h"
#include "Refresh.h"
#include "Request.h"
#include "Scheduler.h"
#include "Statistics.h"

#include "ALDRAM.h"
#include "SALP.h"
#include "TLDRAM.h"

using namespace std;

namespace ramulator {

extern bool warmup_complete;

template <typename T> class Controller {
  protected:
    // For counting bandwidth
    ScalarStat read_transaction_bytes;
    ScalarStat write_transaction_bytes;

    ScalarStat row_hits;
    ScalarStat row_misses;
    ScalarStat row_conflicts;
    VectorStat read_row_hits;
    VectorStat read_row_misses;
    VectorStat read_row_conflicts;
    VectorStat write_row_hits;
    VectorStat write_row_misses;
    VectorStat write_row_conflicts;
    ScalarStat useless_activates;

    ScalarStat read_latency_avg;
    ScalarStat read_latency_sum;

    ScalarStat req_queue_length_avg;
    ScalarStat req_queue_length_sum;
    ScalarStat read_req_queue_length_avg;
    ScalarStat read_req_queue_length_sum;
    ScalarStat write_req_queue_length_avg;
    ScalarStat write_req_queue_length_sum;

  public:
    /* Member Variables */
    long clk = 0;
    DRAM<T> *channel;

    Scheduler<T> *scheduler; // determines the highest priority request whose commands will be issued
    RowPolicy<T> *rowpolicy; // determines the row-policy (e.g., closed-row vs. open-row)
    RowTable<T> *rowtable;   // tracks metadata about rows (e.g., which are open and for how long)
    Refresh<T> *refresh;

    struct Queue {
        list<Request> q;
        unsigned int max = 32;
        unsigned int size() { return q.size(); }
    };

    Queue readq;  // queue for read requests
    Queue writeq; // queue for write requests
    Queue actq;   // read and write requests for which activate was issued are moved to
                  // actq, which has higher priority than readq and writeq.
                  // This is an optimization
                  // for avoiding useless activations (i.e., PRECHARGE
                  // after ACTIVATE w/o READ of WRITE command)
    Queue otherq; // queue for all "other" requests (e.g., refresh)

    deque<Request> pending;         // read & write requests that are about to receive data from DRAM
    bool write_mode = false;        // whether write requests should be prioritized over reads
    float wr_high_watermark = 0.8f; // threshold for switching to write mode
    float wr_low_watermark = 0.2f;  // threshold for switching back to read mode
    // long refreshed = 0;  // last time refresh requests were generated

    /* Command trace for DRAMPower 3.1 */
    string cmd_trace_prefix = "cmd-trace-";
    vector<ofstream> cmd_trace_files;
    bool record_cmd_trace = false;
    /* Commands to stdout */
    bool print_cmd_trace = false;

    bool detailed_analysis = false;
    ofstream read_queue_length_trace_file;
    ofstream write_queue_length_trace_file;
    int queue_length_trace_heapmap_interval = 100000; // number of cycles
    std::vector<int> queue_length_trace_heapmap_read;
    std::vector<int> queue_length_trace_heapmap_write;
    long periodic_read_transaction_bytes;
    long periodic_write_transaction_bytes;
    ofstream periodic_read_bw_file;
    ofstream periodic_write_bw_file;

    stringstream periodic_read_bw_string_buffer;
    stringstream periodic_write_bw_string_buffer;
    stringstream read_queue_length_string_buffer;
    stringstream write_queue_length_string_buffer;


    /* Constructor */
    Controller(const Config &configs, DRAM<T> *channel)
        : channel(channel), scheduler(new Scheduler<T>(this)), rowpolicy(new RowPolicy<T>(this)),
          rowtable(new RowTable<T>(this)), refresh(new Refresh<T>(this)), cmd_trace_files(channel->children.size()) {
        record_cmd_trace = configs.record_cmd_trace();
        print_cmd_trace = configs.print_cmd_trace();
        detailed_analysis = configs["detailed_analysis"] == "on";
        if (record_cmd_trace) {
            if (configs["cmd_trace_prefix"] != "") {
                cmd_trace_prefix = configs["cmd_trace_prefix"];
            }
            if (configs.contains("stats_dir")) {
                cmd_trace_prefix = configs["stats_dir"] + "/" + cmd_trace_prefix;
            }
            string prefix = cmd_trace_prefix + "chan-" + to_string(channel->id) + "-rank-";
            string suffix = ".cmdtrace";
            for (unsigned int i = 0; i < channel->children.size(); i++) {
                cmd_trace_files[i].open(prefix + to_string(i) + suffix);
            }
        }

        if (configs["readq_size"] != "") {
            readq.max = configs.get_int_value("readq_size");
        }

        if (configs["writeq_size"] != "") {
            writeq.max = configs.get_int_value("writeq_size");
        }

        if (configs["scheduling_policy"] != "") {
            scheduler->set_type(configs["scheduling_policy"]);
        }

        // printf("Ramulator: print queue length trace = %s\n", detailed_analysis ? "onnn" : "offff");
        if (detailed_analysis) {
            string queue_length_trace_prefix = "queue-length-trace-";
            string periodic_bw_prefix = "periodic-bw-";
            string llc_heat_map_prefix = "llc-heat-map-";
            if (configs.contains("stats_dir")) {
                queue_length_trace_prefix = configs["stats_dir"] + "/" + queue_length_trace_prefix;
                periodic_bw_prefix = configs["stats_dir"] + "/" + periodic_bw_prefix;
                llc_heat_map_prefix = configs["stats_dir"] + "/" + llc_heat_map_prefix;
            }
            // Queue length files
            queue_length_trace_prefix = queue_length_trace_prefix + "chan-" + to_string(channel->id);
            read_queue_length_trace_file.open(queue_length_trace_prefix + "_read.txt");
            write_queue_length_trace_file.open(queue_length_trace_prefix + "_write.txt");

            // Periodic BW files
            periodic_bw_prefix = periodic_bw_prefix + "chan-" + to_string(channel->id);
            periodic_read_bw_file.open(periodic_bw_prefix + "_read.txt");
            periodic_write_bw_file.open(periodic_bw_prefix + "_write.txt");

            if (configs["queue_length_trace_heapmap_interval"] != "") {
                queue_length_trace_heapmap_interval = configs.get_int_value("queue_length_trace_heapmap_interval");
            }
            queue_length_trace_heapmap_read.resize(readq.max + 1);
            queue_length_trace_heapmap_write.resize(writeq.max + 1);
            periodic_read_transaction_bytes = 0;
            periodic_write_transaction_bytes = 0;
        }

        // regStats
        row_hits.name("row_hits_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row hits per channel per core")
            .precision(0);
        row_misses.name("row_misses_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row misses per channel per core")
            .precision(0);
        row_conflicts.name("row_conflicts_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row conflicts per channel per core")
            .precision(0);

        read_row_hits.init(configs.get_core_num())
            .name("read_row_hits_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row hits for read requests per channel per core")
            .precision(0);
        read_row_misses.init(configs.get_core_num())
            .name("read_row_misses_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row misses for read requests per channel per core")
            .precision(0);
        read_row_conflicts.init(configs.get_core_num())
            .name("read_row_conflicts_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row conflicts for read requests per channel per core")
            .precision(0);

        write_row_hits.init(configs.get_core_num())
            .name("write_row_hits_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row hits for write requests per channel per core")
            .precision(0);
        write_row_misses.init(configs.get_core_num())
            .name("write_row_misses_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row misses for write requests per channel per core")
            .precision(0);
        write_row_conflicts.init(configs.get_core_num())
            .name("write_row_conflicts_channel_" + to_string(channel->id) + "_core")
            .desc("Number of row conflicts for write requests per channel per core")
            .precision(0);

        useless_activates.name("useless_activates_" + to_string(channel->id) + "_core")
            .desc("Number of useless activations. E.g, ACT -> PRE w/o RD or WR")
            .precision(0);

        read_transaction_bytes.name("read_transaction_bytes_" + to_string(channel->id))
            .desc("The total byte of read transaction per channel")
            .precision(0);
        write_transaction_bytes.name("write_transaction_bytes_" + to_string(channel->id))
            .desc("The total byte of write transaction per channel")
            .precision(0);

        read_latency_sum.name("read_latency_sum_" + to_string(channel->id))
            .desc("The memory latency cycles (in memory time domain) sum for all read requests in this channel")
            .precision(0);
        read_latency_avg.name("read_latency_avg_" + to_string(channel->id))
            .desc("The average memory latency cycles (in memory time domain) per request for all read requests in this "
                  "channel")
            .precision(6);

        req_queue_length_sum.name("req_queue_length_sum_" + to_string(channel->id))
            .desc("Sum of read and write queue length per memory cycle per channel.")
            .precision(0);
        req_queue_length_avg.name("req_queue_length_avg_" + to_string(channel->id))
            .desc("Average of read and write queue length per memory cycle per channel.")
            .precision(6);

        read_req_queue_length_sum.name("read_req_queue_length_sum_" + to_string(channel->id))
            .desc("Read queue length sum per memory cycle per channel.")
            .precision(0);
        read_req_queue_length_avg.name("read_req_queue_length_avg_" + to_string(channel->id))
            .desc("Read queue length average per memory cycle per channel.")
            .precision(6);

        write_req_queue_length_sum.name("write_req_queue_length_sum_" + to_string(channel->id))
            .desc("Write queue length sum per memory cycle per channel.")
            .precision(0);
        write_req_queue_length_avg.name("write_req_queue_length_avg_" + to_string(channel->id))
            .desc("Write queue length average per memory cycle per channel.")
            .precision(6);
    }

    ~Controller() {
        delete scheduler;
        delete rowpolicy;
        delete rowtable;
        delete channel;
        delete refresh;
        for (auto &file : cmd_trace_files)
            file.close();
        cmd_trace_files.clear();
        if (detailed_analysis) {
            read_queue_length_trace_file.close();
            write_queue_length_trace_file.close();

            periodic_read_bw_file.close();
            periodic_write_bw_file.close();
        }
    }

    void resetStats() {
        if (detailed_analysis) {
            queue_length_trace_heapmap_read.assign(readq.max + 1, 0);
            queue_length_trace_heapmap_write.assign(writeq.max + 1, 0);
            periodic_read_transaction_bytes = 0;
            periodic_write_transaction_bytes = 0;
            read_queue_length_string_buffer.str(std::string());
            read_queue_length_string_buffer.clear();
            write_queue_length_string_buffer.str(std::string());
            write_queue_length_string_buffer.clear();
            periodic_read_bw_string_buffer.str(std::string());
            periodic_read_bw_string_buffer.clear();
            periodic_write_bw_string_buffer.str(std::string());
            periodic_write_bw_string_buffer.clear();
        }
    }

    void finish(long read_req, long dram_cycles) {
        read_latency_avg = read_latency_sum.value() / read_req;
        req_queue_length_avg = req_queue_length_sum.value() / dram_cycles;
        read_req_queue_length_avg = read_req_queue_length_sum.value() / dram_cycles;
        write_req_queue_length_avg = write_req_queue_length_sum.value() / dram_cycles;

        if (detailed_analysis) {
            read_queue_length_trace_file << read_queue_length_string_buffer.str();
            write_queue_length_trace_file << write_queue_length_string_buffer.str();
            periodic_read_bw_file << periodic_read_bw_string_buffer.str();
            periodic_write_bw_file << periodic_write_bw_string_buffer.str();

            // Explicit Flush - This is crucial!
            read_queue_length_trace_file.flush();
            write_queue_length_trace_file.flush();
            periodic_read_bw_file.flush();
            periodic_write_bw_file.flush();

            // Reset the string buffers
            read_queue_length_string_buffer.str(std::string());
            read_queue_length_string_buffer.clear();
            write_queue_length_string_buffer.str(std::string());
            write_queue_length_string_buffer.clear();
            periodic_read_bw_string_buffer.str(std::string());
            periodic_read_bw_string_buffer.clear();
            periodic_write_bw_string_buffer.str(std::string());
            periodic_write_bw_string_buffer.clear();
        }

        // call finish function of each channel
        channel->finish(dram_cycles);
    }

    /* Member Functions */
    Queue &get_queue(Request::Type type) {
        switch (int(type)) {
        case int(Request::Type::READ):
            return readq;
        case int(Request::Type::WRITE):
            return writeq;
        default:
            return otherq;
        }
    }

    bool enqueue(Request &req) {
        Queue &queue = get_queue(req.type);
        if (queue.max == queue.size())
            return false;

        req.arrive = clk;

        // shortcut for read requests, if a write to same addr exists
        // necessary for coherence
        if (req.type == Request::Type::READ && find_if(writeq.q.begin(), writeq.q.end(), [req](Request &wreq) {
                                                   return req.addr == wreq.addr;
                                               }) != writeq.q.end()) {
            req.depart = clk + 1;
            req.is_found_queue = true;
            pending.push_back(req);
        } else if (req.type == Request::Type::WRITE &&
                   find_if(writeq.q.begin(), writeq.q.end(),
                           [req](Request &wreq) { return req.addr == wreq.addr; }) != writeq.q.end()) {
            // write to write queue, we can simply overwrite the data, no need to put another one...
            req.depart = clk + 1;
            req.is_found_queue = true;
            pending.push_back(req);
        } else {
            req.depart = clk + 1;
            queue.q.push_back(req);
        }

        return true;
    }

    void tick() {
        clk++;
        req_queue_length_sum += readq.size() + writeq.size() + pending.size();
        read_req_queue_length_sum += readq.size() + pending.size();
        unsigned int writeq_size = writeq.size();
        write_req_queue_length_sum += writeq_size;

        if (detailed_analysis) {
            queue_length_trace_heapmap_read[readq.size()]++;
            queue_length_trace_heapmap_write[writeq_size]++;

            if (clk % queue_length_trace_heapmap_interval == 0) {
                // print the queue length heatmap trace
                // Queue length files
                // Reads
                read_queue_length_string_buffer << clk << ": ";

                for (unsigned i = 0; i < queue_length_trace_heapmap_read.size(); i++) {
                    read_queue_length_string_buffer << queue_length_trace_heapmap_read[i] << " ";
                    queue_length_trace_heapmap_read[i] = 0;
                }
                read_queue_length_string_buffer << endl;

                // Writes
                write_queue_length_string_buffer << clk << ": ";

                for (unsigned i = 0; i < queue_length_trace_heapmap_write.size(); i++) {
                    write_queue_length_string_buffer << queue_length_trace_heapmap_write[i] << " ";
                    queue_length_trace_heapmap_write[i] = 0;
                }
                write_queue_length_string_buffer << endl;

                // Pring the periodic BW
                // Reads
                periodic_read_bw_string_buffer
                    << clk << ": "
                    << (static_cast<float>(periodic_read_transaction_bytes) * pow(10, 9)) /
                           (static_cast<float>(queue_length_trace_heapmap_interval) * channel->spec->speed_entry.tCK)
                    << endl;
                periodic_write_bw_string_buffer
                    << clk << ": "
                    << (static_cast<float>(periodic_write_transaction_bytes) * pow(10, 9)) /
                           (static_cast<float>(queue_length_trace_heapmap_interval) * channel->spec->speed_entry.tCK)
                    << endl;
                periodic_read_transaction_bytes = 0;
                periodic_write_transaction_bytes = 0;
            }
        }

        /*** 1. Serve completed reads and writes ***/
        if (pending.size()) {
            Request &req = pending[0];
            if (req.depart <= clk) {
                if (!req.is_found_queue) { // this request really accessed a row
                    if (req.type == Request::Type::READ) {
                        read_latency_sum += req.depart - req.arrive;
                    }
                    channel->update_serving_requests(req.addr_vec.data(), -1, clk);
                }
                req.callback(req);
                pending.pop_front();
            }
        }

        /*** 2. Refresh scheduler ***/
        refresh->tick_ref();

        /*** 3. Should we schedule writes? ***/
        if (!write_mode) {
            // yes -- write queue is almost full or read queue is empty
            if (writeq.size() > static_cast<unsigned int>(wr_high_watermark * writeq.max) || readq.size() == 0)
                write_mode = true;
        } else {
            // no -- write queue is almost empty and read queue is not empty
            if (writeq.size() < static_cast<unsigned int>(wr_low_watermark * writeq.max) && readq.size() != 0)
                write_mode = false;
        }

        /*** 4. Find the best command to schedule, if any ***/

        // First check the actq (which has higher priority) to see if there
        // are requests available to service in this cycle
        Queue *queue = &actq;
        typename T::Command cmd;
        auto req = scheduler->get_head(queue->q);

        bool is_valid_req = (req != queue->q.end());

        if (is_valid_req) {
            cmd = get_first_cmd(req);
            is_valid_req = is_ready(cmd, req->addr_vec);
        }

        if (!is_valid_req) {
            queue = !write_mode ? &readq : &writeq;

            if (otherq.size())
                queue = &otherq; // "other" requests are rare, so we give them precedence over reads/writes


            req = scheduler->get_head(queue->q);

            is_valid_req = (req != queue->q.end());

            if (is_valid_req) {
                cmd = get_first_cmd(req);
                is_valid_req = is_ready(cmd, req->addr_vec);
            }
        }

        if (!is_valid_req) {
            // we couldn't find a command to schedule -- let's try to be speculative
            auto cmd = T::Command::PRE;
            vector<int> victim = rowpolicy->get_victim(cmd);
            if (!victim.empty()) {
                // Convert victim to a std::array<int, Request::ADDR_VEC_SIZE>
                std::array<int, Request::ADDR_VEC_SIZE> victim_addr_vec = {};
                std::copy(victim.begin(), victim.end(), victim_addr_vec.begin());
                issue_cmd(cmd, victim_addr_vec);
            }
            return; // nothing more to be done this cycle
        }

        // If the request requires an ACT command first and the actq is full, then we should not issue the command
        if (cmd == T::Command::ACT && actq.size() >= actq.max) {
            return;
        }

        if (req->is_first_command) {
            req->is_first_command = false;
            int coreid = req->coreid;
            int tx = (channel->spec->prefetch_size * channel->spec->channel_width / 8);

            if (req->type == Request::Type::READ) {
                if (is_row_hit(req)) {
                    ++read_row_hits[coreid];
                    ++row_hits;
                } else if (is_row_open(req)) {
                    ++read_row_conflicts[coreid];
                    ++row_conflicts;
                } else {
                    ++read_row_misses[coreid];
                    ++row_misses;
                }
                read_transaction_bytes += tx;
                periodic_read_transaction_bytes += tx;
            } else if (req->type == Request::Type::WRITE) {
                if (is_row_hit(req)) {
                    ++write_row_hits[coreid];
                    ++row_hits;
                } else if (is_row_open(req)) {
                    ++write_row_conflicts[coreid];
                    ++row_conflicts;                    
                } else {
                    ++write_row_misses[coreid];
                    ++row_misses;
                }
                write_transaction_bytes += tx;
                periodic_write_transaction_bytes += tx;
            }

            if (req->type == Request::Type::READ || req->type == Request::Type::WRITE) {
                channel->update_serving_requests(req->addr_vec.data(), 1, clk);
            }
        }

        // issue command on behalf of request
        issue_cmd(cmd, get_addr_vec(cmd, req));

        // check whether this is NOT the last command (which finishes the request), if cmd is equal to
        // channel->spec->translate[int(req->type)]), it means it is the last command
        if (cmd != channel->spec->translate[int(req->type)]) {
            if (channel->spec->is_opening(cmd)) {
                // Means that cmd is ACT
                // promote the request that caused issuing activation to actq
                actq.q.push_back(*req);
                assert(actq.size() < actq.max);
                queue->q.erase(req);
            }
            return;
        }

        // set a future completion time for read requests
        if (req->type == Request::Type::READ) {
            req->depart = clk + channel->spec->read_latency;
            pending.push_back(*req);
        }

        if (req->type == Request::Type::WRITE) {
            req->depart = clk + 1;
            pending.push_back(*req);
        }

        // remove request from queue
        queue->q.erase(req);
    }

    bool is_ready(list<Request>::iterator req) {
        typename T::Command cmd = get_first_cmd(req);
        return channel->check(cmd, req->addr_vec.data(), clk);
    }

    bool is_ready_for_schedule(list<Request>::iterator req) {
        typename T::Command cmd = get_first_cmd(req);
        return (cmd != T::Command::PRE) && channel->check(cmd, req->addr_vec.data(), clk);
    }

    bool is_ready(typename T::Command cmd, const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec) {
        return channel->check(cmd, addr_vec.data(), clk);
    }

    bool is_ready(typename T::Command cmd, const vector<int> &addr_vec) {
        return channel->check(cmd, addr_vec.data(), clk);
    }

    bool is_row_hit(Request *req) {
        typename T::Command cmd = channel->spec->translate[int(req->type)];
        return channel->check_row_hit(cmd, req->addr_vec.data());
    }

    bool is_row_hit(list<Request>::iterator req) {
        // cmd must be decided by the request type, not the first cmd
        typename T::Command cmd = channel->spec->translate[int(req->type)];
        return channel->check_row_hit(cmd, req->addr_vec.data());
    }

    bool is_row_hit(typename T::Command cmd, const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec) {
        return channel->check_row_hit(cmd, addr_vec.data());
    }

    bool is_row_open(list<Request>::iterator req) {
        // cmd must be decided by the request type, not the first cmd
        typename T::Command cmd = channel->spec->translate[int(req->type)];
        return channel->check_row_open(cmd, req->addr_vec.data());
    }

    bool is_row_open(typename T::Command cmd, const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec) {
        return channel->check_row_open(cmd, addr_vec.data());
    }

    void update_temp(ALDRAM::Temp current_temperature) {}

    // For telling whether this channel is busying in processing read or write
    bool is_active() { return (channel->cur_serving_requests > 0); }

    // For telling whether this channel is under refresh
    bool is_refresh() { return clk <= channel->end_of_refreshing; }

    void set_high_writeq_watermark(const float watermark) { wr_high_watermark = watermark; }

    void set_low_writeq_watermark(const float watermark) { wr_low_watermark = watermark; }


  private:
    typename T::Command get_first_cmd(list<Request>::iterator req) {
        typename T::Command cmd = channel->spec->translate[int(req->type)];
        return channel->decode(cmd, req->addr_vec.data());
    }

    // upgrade to an autoprecharge command
    void cmd_issue_autoprecharge(typename T::Command &cmd, const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec) {

        // currently, autoprecharge is only used with closed row policy
        if (channel->spec->is_accessing(cmd) && rowpolicy->type == RowPolicy<T>::Type::ClosedAP) {
            // check if it is the last request to the opened row
            Queue *queue = write_mode ? &writeq : &readq;

            auto begin = addr_vec.begin();
            vector<int> rowgroup(begin, begin + int(T::Level::Row) + 1);

            int num_row_hits = 0;

            for (auto itr = queue->q.begin(); itr != queue->q.end(); ++itr) {
                if (is_row_hit(itr)) {
                    auto begin2 = itr->addr_vec.begin();
                    vector<int> rowgroup2(begin2, begin2 + int(T::Level::Row) + 1);
                    if (rowgroup == rowgroup2)
                        num_row_hits++;
                }
            }

            if (num_row_hits == 0) {
                Queue *queue = &actq;
                for (auto itr = queue->q.begin(); itr != queue->q.end(); ++itr) {
                    if (is_row_hit(itr)) {
                        auto begin2 = itr->addr_vec.begin();
                        vector<int> rowgroup2(begin2, begin2 + int(T::Level::Row) + 1);
                        if (rowgroup == rowgroup2)
                            num_row_hits++;
                    }
                }
            }

            assert(num_row_hits > 0); // The current request should be a hit,
                                      // so there should be at least one request
                                      // that hits in the current open row
            if (num_row_hits == 1) {
                if (cmd == T::Command::RD)
                    cmd = T::Command::RDA;
                else if (cmd == T::Command::WR)
                    cmd = T::Command::WRA;
                else
                    assert(false && "Unimplemented command type.");
            }
        }
    }

    void issue_cmd(typename T::Command cmd, const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec) {
        cmd_issue_autoprecharge(cmd, addr_vec);
        assert(is_ready(cmd, addr_vec));
        channel->update(cmd, addr_vec.data(), clk);

        if (cmd == T::Command::PRE) {
            if (rowtable->get_hits(addr_vec, true) == 0) {
                // Depends on the policy, as it may schedule a random victim to close
                useless_activates++;
            }
        }

        rowtable->update(cmd, addr_vec, clk);
        if (record_cmd_trace) {
            // select rank
            auto &file = cmd_trace_files[addr_vec[1]];
            string &cmd_name = channel->spec->command_name[int(cmd)];
            file << clk << ',' << cmd_name;
            // TODO bad coding here
            if (cmd_name == "PREA" || cmd_name == "REF")
                file << endl;
            else {
                int bank_id = addr_vec[int(T::Level::Bank)];
                if (channel->spec->standard_name == "DDR4" || channel->spec->standard_name == "GDDR5")
                    bank_id += addr_vec[int(T::Level::Bank) - 1] * channel->spec->org_entry.count[int(T::Level::Bank)];
                file << ',' << bank_id << endl;
            }
        }
        if (print_cmd_trace) {
            printf("%5s %10ld:", channel->spec->command_name[int(cmd)].c_str(), clk);
            for (int lev = 0; lev < int(T::Level::MAX); lev++)
                printf(" %5d", addr_vec[lev]);
            printf("\n");
        }
    }
    std::array<int, Request::ADDR_VEC_SIZE> get_addr_vec(typename T::Command cmd, list<Request>::iterator req) {
        return req->addr_vec;
    }

    int get_num_expected_row_hits(list<Request> &q, const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec) {
        auto begin = addr_vec.begin();
        vector<int> rowgroup(begin, begin + int(T::Level::Row) + 1);
        int num_row_hits = count_if(q.begin(), q.end(), [rowgroup](Request &r) {
            auto begin2 = r.addr_vec.begin();
            vector<int> rowgroup2(begin2, begin2 + int(T::Level::Row) + 1);
            return rowgroup == rowgroup2;
        });

        return num_row_hits;
    }
};

template <>
std::array<int, Request::ADDR_VEC_SIZE> Controller<SALP>::get_addr_vec(SALP::Command cmd, list<Request>::iterator req);

template <> bool Controller<SALP>::is_ready(list<Request>::iterator req);

template <> void Controller<ALDRAM>::update_temp(ALDRAM::Temp current_temperature);

template <> void Controller<TLDRAM>::tick();

template <>
void Controller<TLDRAM>::cmd_issue_autoprecharge(typename TLDRAM::Command &cmd,
                                                 const std::array<int, Request::ADDR_VEC_SIZE> &addr_vec);

} /*namespace ramulator*/

#endif /*__CONTROLLER_H*/
