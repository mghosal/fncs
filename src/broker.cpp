/* autoconf header */
#include "config.h"

/* C++ standard headers */
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <sstream>
#include <vector>

/* 3rd party headers */
#include "czmq.h"

/* fncs headers */
#include "log.hpp"
#include "fncs.hpp"
#include "fncs_internal.hpp"

using namespace ::std;

class SimulatorState {
    public:
        SimulatorState()
            : name("")
            , time_requested(0)
            , time_delta(0)
            , time_last_processed(0)
            , processing(true)
            , messages_pending(false)
        {}

        string name;
        fncs::time time_requested;
        fncs::time time_delta;
        fncs::time time_last_processed;
        bool processing;
        bool messages_pending;
};

typedef map<string,size_t> SimIndex;
typedef vector<SimulatorState> SimVec;
typedef vector<size_t> IndexVec;
typedef vector<fncs::time> TimeVec;
typedef map<string,IndexVec> TopicMap;
typedef map<string,set<string> > SimKeyMap;
typedef map<string,TimeVec> SimTimeMap;

static ofstream trace; /* the trace stream, if requested */
static ofstream ofs_status; /* the trace stream, if requested */
static unsigned int n_sims = 0; /* how many sims will connect */
static fncs::time time_granted = 0; /* global clock */
static int64_t time_zclock_last = 0; /* global real-time mono clock */
static unsigned long long count_publish = 0;
static unsigned long long count_time_request = 0;
static bool is_running = false;

static inline void broker_die(const SimVec &simulators, zsock_t *server) {
    is_running = false;
    /* repeat the fatal die to all connected sims */
    for (size_t i=0; i<simulators.size(); ++i) {
        zstr_sendm(server, simulators[i].name.c_str());
        zstr_send(server, fncs::DIE);
    }
    zsock_destroy(&server);
    zsys_shutdown(); /* without this, Windows will assert */
    if (trace.is_open()) {
        trace.close();
    }
    if (ofs_status.is_open()) {
        ofs_status.close();
    }
    exit(EXIT_FAILURE);
}


static int server_handler(zloop_t *loop, zsock_t *server, void *arg)
{
    /* declare all variables */
    static set<string> byes;           /* which sims have disconnected */
    static int n_processing = 0;       /* how many sims are processing a time step */
    static SimVec simulators;          /* vector of connected simulator state */
    static SimIndex name_to_index;     /* quickly lookup sim state index */
    static TopicMap topic_to_indexes;  /* quickly lookup subscribed sims */
    static SimKeyMap name_to_keys;     /* summary of topics per sim name */
    static SimKeyMap name_to_peers;    /* summary of peers per sim name */
    static SimTimeMap name_to_peertimes; /* summary of peer deltas */

    {
        int rc = 0;
        zmsg_t *msg = NULL;
        zframe_t *frame = NULL;
        string sender;
        string message_type;

        LDEBUG4 << "incoming message";
        msg = zmsg_recv(server);
        if (!msg) {
            LERROR << "null message received";
            broker_die(simulators, server);
        }

        /* first frame is sender */
        frame = zmsg_first(msg);
        if (!frame) {
            LERROR << "message missing sender";
            broker_die(simulators, server);
        }
        sender = fncs::to_string(frame);

        /* next frame is message type identifier */
        frame = zmsg_next(msg);
        if (!frame) {
            LERROR << "message missing type identifier";
            broker_die(simulators, server);
        }
        message_type = fncs::to_string(frame);

        /* dispatcher */
        if (fncs::HELLO == message_type) {
            SimulatorState state;
            string config_string;
            fncs::Config config;
            string time_delta;
            size_t index = 0;

            LDEBUG4 << "HELLO received";

            /* check for duplicate sims */
            if (name_to_index.count(sender) != 0) {
                LERROR << "simulator '" << sender << "' already connected";
                broker_die(simulators, server);
            }
            index = simulators.size();
            LDEBUG4 << "registering client '" << sender << "'";

            /* next frame is config chunk */
            frame = zmsg_next(msg);
            if (!frame) {
                LERROR << "HELLO message missing config frame";
                broker_die(simulators, server);
            }

            /* copy config frame into chunk */
            config_string = fncs::to_string(frame);
            LDEBUG2 << "-- recv configuration as follows --" << endl << config_string;

            /* parse config chunk */
            config = fncs::parse_config(config_string);

            /* get time delta from config */
            time_delta = config.time_delta;
            if (time_delta.empty()) {
                LWARNING << sender << " config does not contain 'time_delta'";
                LWARNING << sender << " time_delta defaulting to 1s";
                time_delta = "1s";
            }
            state.time_delta = fncs::parse_time(time_delta);

            /* parse subscription values */
            if (!config.values.empty()) {
                vector<fncs::Subscription> &subs = config.values;
                set<string> peers;
                for (size_t i=0; i<subs.size(); ++i) {
                    string topic = subs[i].topic;
                    LDEBUG4 << "adding value '" << topic << "'";
                    TopicMap::iterator it = topic_to_indexes.find(topic);
                    if (it != topic_to_indexes.end()) {
                        it->second.push_back(index);
                    }
                    else {
                        topic_to_indexes[topic] = IndexVec(1,index);
                    }
                    size_t loc = topic.find('/');
                    if (loc == string::npos) {
                        LWARNING << "invalid topic: " << topic;
                    }
                    else {
                        string name = topic.substr(0,loc);
                        string key = topic.substr(loc+1);
                        name_to_keys[name].insert(key);
                        LDEBUG4 << "name_to_keys[" << name << "]=" << key;
                        peers.insert(name);
                    }
                }
                for (set<string>::iterator it=peers.begin();
                        it!=peers.end(); ++it) {
                    SimTimeMap::iterator stm = name_to_peertimes.find(*it);
                    if (stm == name_to_peertimes.end()) {
                        name_to_peertimes[*it] = TimeVec(1, state.time_delta);
                    }
                    else {
                        name_to_peertimes[*it].push_back(state.time_delta);
                    }
                }
                name_to_peers[sender] = peers;
            }
            else {
                LDEBUG4 << "no subscription values";
            }

            /* populate sim state object */
            state.name = sender;
            /*state.time_delta = fncs::parse_time(time_delta);*/ /*above*/
            state.time_requested = 0;
            state.time_last_processed = 0;
            state.processing = false;
            state.messages_pending = false;
            name_to_index[sender] = index;
            simulators.push_back(state);

            LDEBUG4 << "simulators.size() = " << simulators.size();

            /* if all sims have connected, send the go-ahead */
            if (simulators.size() == n_sims) {
                /* easier to keep a counter than iterating over states */
                n_processing = n_sims;
                is_running = true;
                /* send ACK to all registered sims */
                for (size_t i=0; i<n_sims; ++i) {
                    set<string> &keys = name_to_keys[simulators[i].name];
                    simulators[i].processing = true;
                    zstr_sendm(server, simulators[i].name.c_str());
                    zstr_sendm(server, fncs::ACK);
                    zstr_sendfm(server, "%llu", (unsigned long long)i);
                    zstr_sendfm(server, "%llu", (unsigned long long)n_sims);
                    zstr_sendfm(server, "%llu", (unsigned long long)keys.size());
                    for (set<string>::iterator it=keys.begin(); it!=keys.end(); ++it) {
                        zstr_sendm(server, it->c_str());
                    }
                    /* smallest delta of any clients */
                    {
                        fncs::time time_peer = 0;
                        TimeVec &peertimes = name_to_peertimes[simulators[i].name];
                        set<string> &peers = name_to_peers[simulators[i].name];
                        for (set<string>::iterator it=peers.begin();
                                it!=peers.end(); ++it) {
                            assert(name_to_index.count(*it));
                            size_t index = name_to_index[*it];
                            peertimes.push_back(simulators[index].time_delta);
                        }
                        if (!peertimes.empty()) {
                            time_peer = *min_element(
                                    peertimes.begin(),
                                    peertimes.end());
                        }
                        LDEBUG4 << "time_peer = " << time_peer;
                        LDEBUG4 << "time_delta= " << simulators[i].time_delta;
                        zstr_sendfm(server, "%llu", (unsigned long long)time_peer);
                    }
                    zstr_send(server, fncs::ACK);
                    LDEBUG4 << "ACK sent to '" << simulators[i].name;
                }
            }
        }
        else if (fncs::TIME_REQUEST == message_type
                || fncs::BYE == message_type) {
            size_t index = 0; /* index of sim state */
            fncs::time time_requested;

            if (fncs::TIME_REQUEST == message_type) {
                LDEBUG4 << "TIME_REQUEST received";
            }
            else if (fncs::BYE == message_type) {
                LDEBUG4 << "BYE received";
            }

            /* did we receive message from a connected sim? */
            if (name_to_index.count(sender) == 0) {
                LERROR << "simulator '" << sender << "' not connected";
                broker_die(simulators, server);
            }

            /* index of sim state */
            index = name_to_index[sender];

            if (fncs::BYE == message_type) {
                /* soft error if muliple byes received */
                if (byes.count(sender)) {
                    LWARNING << "duplicate BYE from '" << sender << "'";
                }

                /* add sender to list of leaving sims */
                byes.insert(sender);

                /* if all byes received, then exit */
                if (byes.size() == n_sims) {
                    is_running = false;
                    /* let all sims know that globally we are finished */
                    for (size_t i=0; i<n_sims; ++i) {
                        zstr_sendm(server, simulators[i].name.c_str());
                        zstr_send(server, fncs::BYE);
                        LDEBUG4 << "BYE sent to '" << simulators[i].name;
                    }
                    /* need to delete msg since we are breaking from loop */
                    zmsg_destroy(&msg);
                    return -1;
                }

                /* update sim state */
                simulators[index].time_requested = ULLONG_MAX;
            }
            else if (fncs::TIME_REQUEST == message_type) {
                ++count_time_request;
                /* next frame is time */
                frame = zmsg_next(msg);
                if (!frame) {
                    LERROR << "TIME_REQUEST message missing time frame";
                    broker_die(simulators, server);
                }
                /* convert time string */
                {
                    istringstream iss(fncs::to_string(frame));
                    iss >> time_requested;
                }

                /* update sim state */
                simulators[index].time_requested = time_requested;
            }

            /* update sim state */
            simulators[index].time_last_processed = time_granted;
            simulators[index].processing = false;

            --n_processing;

            /* if all sims are done, determine next time step */
            if (0 == n_processing) {
                vector< fncs::time> time_actionable(n_sims);
                for (size_t i=0; i<n_sims; ++i) {
                    if (simulators[i].messages_pending) {
                        time_actionable[i] = 
                            simulators[i].time_last_processed
                            + simulators[i].time_delta;
                    }
                    else {
                        time_actionable[i] = simulators[i].time_requested;
                    }
                }
                time_granted = *min_element(time_actionable.begin(),
                        time_actionable.end());
                LDEBUG4 << "time_granted = " << time_granted;
                for (size_t i=0; i<n_sims; ++i) {
                    if (time_granted == time_actionable[i]) {
                        LDEBUG4 << "granting " << time_granted
                            << " to " << simulators[i].name;
                        ++n_processing;
                        simulators[i].processing = true;
                        simulators[i].messages_pending = false;
                        zstr_sendm(server, simulators[i].name.c_str());
                        zstr_sendm(server, fncs::TIME_REQUEST);
                        zstr_sendf(server, "%llu", time_granted);
                    }
                    else {
                        /* fast forward time last processed */
                        fncs::time jump = (time_granted - simulators[i].time_last_processed) / simulators[i].time_delta;
                        simulators[i].time_last_processed += simulators[i].time_delta * jump;
                    }
                }
            }
        }
        else if (fncs::PUBLISH == message_type) {
            string topic = "";
            bool found_one = false;

            LDEBUG4 << "PUBLISH received";

            ++count_publish;

            /* did we receive message from a connected sim? */
            if (name_to_index.count(sender) == 0) {
                LERROR << "simulator '" << sender << "' not connected";
                broker_die(simulators, server);
            }

            /* next frame is topic */
            frame = zmsg_next(msg);
            if (!frame) {
                LERROR << "PUBLISH message missing topic";
                broker_die(simulators, server);
            }
            topic = fncs::to_string(frame);

            LDEBUG4 << "PUBLISH received topic " << topic;

            if (trace.is_open()) {
                /* next frame is value payload */
                frame = zmsg_next(msg);
                if (!frame) {
                    LERROR << "PUBLISH message missing value";
                    broker_die(simulators, server);
                }
                string value = fncs::to_string(frame);
                trace << time_granted
                    << "\t" << topic
                    << "\t" << value
                    << endl;
            }

            /* send the message to subscribed sims */
            {
                TopicMap::iterator iter = topic_to_indexes.find(topic);
                if (iter != topic_to_indexes.end()) {
                    IndexVec &iv = iter->second;
                    IndexVec::iterator index;
                    for (index=iv.begin(); index!=iv.end(); index++) {
                        size_t i = *index;
                        zmsg_t *msg_copy = zmsg_dup(msg);
                        if (!msg_copy) {
                            LERROR << "failed to copy pub message";
                            broker_die(simulators, server);
                        }
                        /* swap out original sender with new destiation */
                        zframe_reset(zmsg_first(msg_copy),
                                simulators[i].name.c_str(),
                                simulators[i].name.size());
                        /* send it on */
                        zmsg_send(&msg_copy, server);
                        found_one = true;
                        simulators[i].messages_pending = true;
                        LDEBUG4 << "pub to " << simulators[i].name;
                    }
                }
            }
            if (!found_one) {
                LDEBUG4 << "dropping PUBLISH message '" << topic << "'";
            }
        }
        else if (fncs::DIE == message_type) {
            LDEBUG4 << "DIE received";

            /* did we receive message from a connected sim? */
            if (name_to_index.count(sender) == 0) {
                LERROR << "simulator '" << sender << "' not connected";
                broker_die(simulators, server);
            }

            broker_die(simulators, server);
        }
        else if (fncs::TIME_DELTA == message_type) {
            size_t index = 0; /* index of sim state */
            fncs::time time_delta;

            LDEBUG4 << "TIME_DELTA received";

            /* did we receive message from a connected sim? */
            if (name_to_index.count(sender) == 0) {
                LERROR << "simulator '" << sender << "' not connected";
                broker_die(simulators, server);
            }

            /* index of sim state */
            index = name_to_index[sender];

            /* next frame is time */
            frame = zmsg_next(msg);
            if (!frame) {
                LERROR << "TIME_DELTA message missing time frame";
                broker_die(simulators, server);
            }
            /* convert time string */
            {
                istringstream iss(fncs::to_string(frame));
                iss >> time_delta;
            }

            /* update sim state */
            simulators[index].time_delta = time_delta;
        }
        else {
            LERROR << "received unknown message type '"
                << message_type << "'";
            broker_die(simulators, server);
        }

        zmsg_destroy(&msg);
    }

    return 0;
}


int status_handler(zloop_t *loop, int timer_id, void *arg)
{
    static fncs::time time_granted_last = 0;
    static unsigned long long count_publish_last = 0;
    static unsigned long long count_time_request_last = 0;

    int64_t time_zclock_now = zclock_mono();
    fncs::time time_granted_diff = time_granted - time_granted_last;
    int64_t time_zclock_diff = time_zclock_now - time_zclock_last;
    unsigned long long count_publish_diff = count_publish - count_publish_last;
    unsigned long long count_time_request_diff = count_time_request - count_time_request_last;

    if (is_running) {
        ofs_status << time_granted_diff
            << "," << time_zclock_diff
            << "," << count_publish_diff
            << "," << count_time_request_diff
            << endl;
    }

    time_zclock_last = time_zclock_now;
    time_granted_last = time_granted;
    count_publish_last = count_publish;
    count_time_request_last = count_time_request;
}


int main(int argc, char **argv)
{
    /* declare all variables */
    const char *endpoint = NULL;/* broker location */
    zsock_t *server = NULL;     /* the broker socket */
    bool do_trace = false;      /* whether to dump all received messages */
    fncs::time realtime_interval = 0;
    fncs::time status_interval = 0;
    int rc = 0;

    /* initialize global static counters */
    time_granted = 0;
    time_zclock_last = zclock_mono();
    count_publish = 0;
    count_time_request = 0;
    is_running = false;

    fncs::start_logging();
    fncs::replicate_logging(FNCSLog::ReportingLevel(),
            Output2Tee::Stream1(), Output2Tee::Stream2()); 

    /* how many simulators are connecting? */
    if (argc > 4) {
        LERROR << "too many command line args";
        exit(EXIT_FAILURE);
    }
    if (argc < 2) {
        LERROR << "missing command line arg for number of simulators";
        exit(EXIT_FAILURE);
    }
    if (argc >= 2) {
        int n_sims_signed = 0;
        istringstream iss(argv[1]);
        iss >> n_sims_signed;
        LDEBUG4 << "n_sims_signed = " << n_sims_signed;
        if (n_sims_signed <= 0) {
            LERROR << "number of simulators arg must be >= 1";
            exit(EXIT_FAILURE);
        }
        n_sims = static_cast<unsigned int>(n_sims_signed);
    }
    if (argc >= 3) {
        realtime_interval = fncs::parse_time(argv[2]);
        LDEBUG4 << "realtime_interval = " << realtime_interval << " ns";
    }
    if (argc == 4) {
        status_interval = fncs::parse_time(argv[3]);
        LDEBUG4 << "status_interval = " << status_interval << " ns";
    }

    {
        const char *env_do_trace = getenv("FNCS_TRACE");
        if (env_do_trace) {
            if (env_do_trace[0] == 'Y'
                    || env_do_trace[0] == 'y'
                    || env_do_trace[0] == 'T'
                    || env_do_trace[0] == 't') {
                do_trace = true;
            }
        }
    }

    if (do_trace) {
        LDEBUG4 << "tracing of all published messages enabled";
        trace.open("broker_trace.txt");
        if (!trace) {
            LERROR << "Could not open trace file 'broker_trace.txt'";
            exit(EXIT_FAILURE);
        }
        trace << "#nanoseconds\ttopic\tvalue" << endl;
    }

    /* broker endpoint may come from env var */
    endpoint = getenv("FNCS_BROKER");
    if (!endpoint) {
        endpoint = "tcp://*:5570";
    }

    server = zsock_new_router(endpoint);
    if (!server) {
        LERROR << "socket creation failed";
        exit(EXIT_FAILURE);
    }
    if (!(zsock_resolve(server) != server)) {
        LERROR << "socket failed to resolve";
        exit(EXIT_FAILURE);
    }
    LDEBUG4 << "broker socket bound to " << endpoint;

    zloop_t *loop = zloop_new();
    assert(loop);
    zloop_set_verbose(loop, true);

    rc = zloop_reader(loop, server, server_handler, NULL);
    assert(0 == rc);
    zloop_reader_set_tolerant(loop, server);

    if (0 != status_interval) {
        if (0 != status_interval%1000000) {
            LWARNING << "status interval must have millisecond resolution";
        }
        ofs_status.open("broker_status.csv");
        if (!ofs_status) {
            LERROR << "Could not open status file 'broker_status.csv'";
            exit(EXIT_FAILURE);
        }
        ofs_status << "model time (ns),wall time (ms),published count,time request count" << endl;
        zloop_timer(loop, status_interval/1000000, 0, status_handler, NULL);
    }

    zloop_start(loop);
    LDEBUG4 << "zloop finished";

    zsock_destroy(&server);
    zsys_shutdown(); /* without this, Windows will assert */

    if (trace.is_open()) {
        trace.close();
    }
    if (ofs_status.is_open()) {
        ofs_status.close();
    }

    return 0;
}

