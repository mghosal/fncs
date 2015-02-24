/* autoconf header */
#include "config.h"

/* C++  standard headers */
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

/* 3rd party headers */
#include "czmq.h"
#include "easylogging++.h"
_INITIALIZE_EASYLOGGINGPP

/* fncs headers */
#include "fncs.hpp"
#include "fncs_internal.hpp"

using namespace ::easyloggingpp;
using namespace ::std;


static string simulation_name = "";
static fncs::time time_delta_multiplier = 0;
static fncs::time time_delta = 0;
static fncs::time time_granted = 0;
static zsock_t *client = NULL;
static map<string,string> cache;

typedef map<string,vector<string> > clist_t;
static clist_t cache_list;

typedef map<zrex_t*,fncs::Subscription> zsub_t;
static zsub_t subscriptions;


void fncs::start_logging()
{
    const char *fncs_log_file = NULL;
    int rc;

    /* name for fncs log file from environment */
    fncs_log_file = getenv("FNCS_LOG_FILE");
    if (!fncs_log_file) {
        fncs_log_file = "fncs.log";
    }

    /* start our logger */
    Loggers::setFilename(fncs_log_file);
    Loggers::reconfigureAllLoggers(ConfigurationType::Format,
            "%datetime %level %log");
    //Loggers::reconfigureAllLoggers(ConfigurationType::ToStandardOutput, "false");

    LTRACE << "FNCS_LOG_FILE: " << fncs_log_file;
}


/* This version of initialize() checks for a config filename in the
 * environment and then defaults to a known name. */
void fncs::initialize()
{
    const char *fncs_config_file = NULL;
    zconfig_t *config = NULL;

    start_logging();

    /* name for fncs config file from environment */
    fncs_config_file = getenv("FNCS_CONFIG_FILE");
    if (!fncs_config_file) {
        fncs_config_file = "fncs.zpl";
    }
    LTRACE << "FNCS_CONFIG_FILE: " << fncs_config_file;

    /* open and parse fncs configuration */
    config = zconfig_load(fncs_config_file);
    if (!config) {
        LFATAL << "could not open " << fncs_config_file;
        die();
    }

    initialize(config);
    zconfig_destroy(&config);
}


/* This version of initialize() reads configuration parameters directly
 * from the given string. */
void fncs::initialize(const string &configuration)
{
    zchunk_t *zchunk = NULL;
    zconfig_t *config = NULL;

    start_logging();

    /* create a zchunk for parsing */
    zchunk = zchunk_new(configuration.c_str(), configuration.size());

    /* open and parse fncs configuration */
    config = zconfig_chunk_load(zchunk);
    if (!config) {
        LFATAL << "could not load configuration chunk";
        die();
    }
    zchunk_destroy(&zchunk);

    initialize(config);
    zconfig_destroy(&config);
}


void fncs::initialize(zconfig_t *config)
{
    const char *name = NULL;
    const char *broker_endpoint = NULL;
    const char *time_delta_string = NULL;
    int rc;
    zmsg_t *msg = NULL;
    zchunk_t *zchunk = NULL;
    zconfig_t *config_values = NULL;

    /* name from env var is tried first */
    name = getenv("FNCS_NAME");
    if (!name) {
        /* read sim name from config */
        name = zconfig_resolve(config, "/name", NULL);
        if (!name) {
            LFATAL << "FNCS_NAME env var not set and";
            LFATAL << "fncs config does not contain 'name'";
            die();
        }
    }
    else {
        LTRACE << "FNCS_NAME env var sets the name";
    }
    simulation_name = name;
    LTRACE << "name = '" << name << "'";

    /* read broker location from config */
    broker_endpoint = zconfig_resolve(config, "/broker", NULL);
    if (!broker_endpoint) {
        LFATAL << "fncs config does not contain 'broker'";
        die();
    }
    LTRACE << "broker = " << broker_endpoint;

    /* read time delta from config */
    time_delta_string = zconfig_resolve(config, "/time_delta", NULL);
    if (!time_delta_string) {
        LFATAL << "fncs config does not contain 'time_delta'";
        die();
    }
    LTRACE << "time_delta_string = " << time_delta_string;
    time_delta = time_parse(time_delta_string);
    LTRACE << "time_delta = " << time_delta;
    time_delta_multiplier = time_unit_to_multiplier(time_delta_string);
    LTRACE << "time_delta_multiplier = " << time_delta_multiplier;

    /* parse subscriptions */
    config_values = zconfig_locate(config, "/values");
    if (config_values) {
        vector<fncs::Subscription> subs =
            fncs::subscriptions_parse(config_values);
        for (size_t i=0; i<subs.size(); ++i) {
            LTRACE << "compiling re'" << subs[i].topic << "'";
            subscriptions.insert(make_pair(
                        zrex_new(subs[i].topic.c_str()),
                        subs[i]));
            LTRACE << "initializing cache for '" << subs[i].key << "'='"
                << subs[i].def << "'";
            if (subs[i].is_list()) {
                cache_list[subs[i].key] = vector<string>(1, subs[i].def);
            }
            else {
                cache[subs[i].key] = subs[i].def;
            }
        }
    }
    else {
        LTRACE << "no subscriptions";
    }

    /* create zmq context and client socket */
    client = zsock_new(ZMQ_DEALER);
    if (!client) {
        LFATAL << "socket creation failed";
        die();
    }
    if (!(zsock_resolve(client) != client)) {
        LFATAL << "socket failed to resolve";
    }
    /* set client identity */
    rc = zmq_setsockopt(zsock_resolve(client), ZMQ_IDENTITY, name, strlen(name));
    if (rc) {
        LFATAL << "socket identity failed";
        die();
    }
    /* finally connect to broker */
    rc = zsock_attach(client, broker_endpoint, false);
    if (rc) {
        LFATAL << "socket connection to broker failed";
        die();
    }

    /* construct HELLO message; entire config goes with it */
    msg = zmsg_new();
    if (!msg) {
        LFATAL << "could not construct HELLO message";
        die();
    }
    rc = zmsg_addstr(msg, HELLO);
    if (rc) {
        LFATAL << "failed to append HELLO to message";
        die();
    }
    zchunk = zconfig_chunk_save(config);
    if (!zchunk) {
        LFATAL << "failed to save config for HELLO message";
        die();
    }
    rc = zmsg_addmem(msg, zchunk_data(zchunk), zchunk_size(zchunk));
    if (rc) {
        LFATAL << "failed to add config to HELLO message";
        die();
    }
    zchunk_destroy(&zchunk);
    LTRACE << "sending HELLO";
    rc = zmsg_send(&msg, client);
    if (rc) {
        LFATAL << "failed to send HELLO message";
        die();
    }

    /* receive ack */
    msg = zmsg_recv(client);
    if (!msg) {
        LFATAL << "null message received";
        die();
    }
    /* first frame is type identifier */
    zframe_t *frame = zmsg_first(msg);
    if (!zframe_streq(frame, ACK)) {
        LFATAL << "ACK expected, got " << frame;
        die();
    }
    LTRACE << "received ACK";
    zmsg_destroy(&msg);
}


fncs::time fncs::time_request(fncs::time next)
{
    fncs::time granted;

    /* send TIME_REQUEST */
    next *= time_delta_multiplier;
    LTRACE << "sending TIME_REQUEST of " << next;
    zstr_sendm(client, fncs::TIME_REQUEST);
    zstr_sendf(client, "%lu", next);

    /* sending of the time request implies we are done with the cache
     * list, but the other cache remains as a last value cache */
    /* only clear the vectors associated with cache list keys because
     * the keys should remain valid i.e. empty lists are meaningful */
    for (clist_t::iterator it=cache_list.begin(); it!=cache_list.end(); ++it) {
        it->second.clear();
    }

    /* receive TIME_REQUEST and perhaps other message types */
    zmq_pollitem_t items[] = { { zsock_resolve(client), 0, ZMQ_POLLIN, 0 } };
    while (true) {
        int rc = 0;

        LTRACE << "entering blocking poll";
        rc = zmq_poll(items, 1, -1);
        if (rc == -1) {
            LTRACE << "client polling error: " << strerror(errno);
            die(); /* interrupted */
        }

        if (items[0].revents & ZMQ_POLLIN) {
            zmsg_t *msg = NULL;
            zframe_t *frame = NULL;
            string message_type;

            LTRACE << "incoming message";
            msg = zmsg_recv(client);
            if (!msg) {
                LFATAL << "null message received";
                die();
            }

            /* first frame is message type identifier */
            frame = zmsg_first(msg);
            if (!frame) {
                LFATAL << "message missing type identifier";
                die();
            }
            LTRACE << frame;
            message_type = fncs::to_string(frame);

            /* dispatcher */
            if (fncs::TIME_REQUEST == message_type) {
                char *time_str = NULL;

                LTRACE << "TIME_REQUEST received";

                /* next frame is time */
                frame = zmsg_next(msg);
                if (!frame) {
                    LFATAL << "message missing time";
                    die();
                }
                LTRACE << frame;
                /* convert time string */
                {
                    istringstream iss(fncs::to_string(frame));
                    iss >> granted;
                }

                /* destroy message early since a returned TIME_REQUEST
                 * indicates we can move on with the break */
                zmsg_destroy(&msg);
                break;
            }
            else if (fncs::PUBLISH == message_type) {
                string topic;
                string value;
                bool found = false;

                LTRACE << "PUBLISH received";

                /* next frame is topic */
                frame = zmsg_next(msg);
                if (!frame) {
                    LFATAL << "message missing topic";
                    die();
                }
                LTRACE << frame;
                topic = fncs::to_string(frame);

                /* next frame is value payload */
                frame = zmsg_next(msg);
                if (!frame) {
                    LFATAL << "message missing value";
                    die();
                }
                LTRACE << frame;
                value = fncs::to_string(frame);

                /* find cache short key */
                for (zsub_t::const_iterator it=subscriptions.begin();
                        it!=subscriptions.end(); ++it) {
                    if (zrex_matches(it->first, topic.c_str())) {
                        found = true;
                        /* store in cache */
                        if (it->second.is_list()) {
                            cache_list[it->second.key].push_back(value);
                        }
                        else {
                            cache[it->second.key] = value;
                        }
                        LTRACE << "updated cache topic='" << topic
                            << "' '" << it->second.key << "=" << value << "'";
                    }
                }
                if (!found) {
                    LWARNING << "dropping PUBLISH message topic='"
                        << topic << "'";
                }
            }
            else {
                LFATAL << "unrecognized message type";
                die();
            }

            zmsg_destroy(&msg);
        }
    }

    LTRACE << "granted " << granted;
    return granted;
}


void fncs::publish(const string &key, const string &value)
{
    string new_key = simulation_name + '/' + key;
    zstr_sendm(client, fncs::PUBLISH);
    zstr_sendm(client, new_key.c_str());
    zstr_send(client, value.c_str());
    LTRACE << "sent PUBLISH '" << new_key << "'='" << value << "'";
}


void fncs::route(
        const string &from,
        const string &to,
        const string &key,
        const string &value)
{
    string new_key = simulation_name + '/' + from + ':' + to + '/' + key;
    zstr_sendm(client, fncs::PUBLISH);
    zstr_sendm(client, new_key.c_str());
    zstr_send(client, value.c_str());
    LTRACE << "sent PUBLISH '" << new_key << "'='" << value << "'";
}


void fncs::die()
{
    if (client) {
        zstr_send(client, fncs::DIE);
        zsock_destroy(&client);
    }
    exit(EXIT_FAILURE);
}


void fncs::finalize()
{
    zstr_send(client, fncs::BYE);
    zsock_destroy(&client);
}


ostream& operator << (ostream& os, zframe_t *self) {
    assert (self);
    assert (zframe_is (self));

    byte *data = zframe_data (self);
    size_t size = zframe_size (self);

    //  Probe data to check if it looks like unprintable binary
    int is_bin = 0;
    uint char_nbr;
    for (char_nbr = 0; char_nbr < size; char_nbr++)
        if (data [char_nbr] < 9 || data [char_nbr] > 127)
            is_bin = 1;

    size_t buffer_size = 31;
    buffer_size += is_bin? size*2 : size;
    char *buffer = (char*)zmalloc(buffer_size);
    snprintf (buffer, 30, "[%03d] ", (int) size);
    for (char_nbr = 0; char_nbr < size; char_nbr++) {
        if (is_bin)
            sprintf (buffer + strlen (buffer), "%02X", (unsigned char) data [char_nbr]);
        else
            sprintf (buffer + strlen (buffer), "%c", data [char_nbr]);
    }

    os << buffer;
}


fncs::time fncs::time_unit_to_multiplier(const string &value)
{
    fncs::time retval; 
    fncs::time ignore; 
    string unit;
    istringstream iss(value);
    int rc = 0;
    long unsigned _value = 0;
    char _unit[256] = {0};

    iss >> ignore;
    if (!iss) {
        LFATAL << "could not parse time value";
        die();
    }
    iss >> unit;
    if (!iss) {
        LFATAL << "could not parse time unit";
        die();
    }

    if ("h" == unit
            || "hour" == unit
            || "hours" == unit) {
        retval = 24U * 60U * 1000000000U;
    }
    else if ("m" == unit
            || "min" == unit
            || "minute" == unit
            || "minutes" == unit) {
        retval = 60U * 1000000000U;
    }
    else if ("s" == unit
            || "sec" == unit
            || "second" == unit
            || "seconds" == unit) {
        retval = 1000000000U;
    }
    else if ("ms" == unit
            || "msec" == unit
            || "millisec" == unit
            || "millisecond" == unit
            || "milliseconds" == unit) {
        retval = 1000000U;
    }
    else if ("us" == unit
            || "usec" == unit
            || "microsec" == unit
            || "microsecond" == unit
            || "microseconds" == unit) {
        retval = 1000U;
    }
    else if ("ns" == unit
            || "nsec" == unit
            || "nanosec" == unit
            || "nanosecond" == unit
            || "nanoseconds" == unit) {
        retval = 1U;
    }
    else {
        LFATAL << "unrecognized time unit '" << unit << "'";
    }

    return retval;
}


fncs::time fncs::time_parse(const string &value)
{
    fncs::time retval; 
    string unit;
    istringstream iss(value);

    iss >> retval;
    if (!iss) {
        LFATAL << "could not parse time value";
        die();
    }

    retval *= fncs::time_unit_to_multiplier(value);

    return retval;
}


fncs::Subscription fncs::subscription_parse(zconfig_t *config)
{
    fncs::Subscription sub;
    const char *value = NULL;

    sub.key = zconfig_name(config);

    value = zconfig_resolve(config, "topic", NULL);
    if (!value) {
        LFATAL << "error parsing value '" << sub.key << "', missing 'topic'";
        die();
    }
    sub.topic = value;

    value = zconfig_resolve(config, "default", NULL);
    if (!value) {
        LFATAL << "error parsing value '" << sub.key << "', missing 'default'";
        die();
    }
    sub.def = value;

    value = zconfig_resolve(config, "type", NULL);
    if (!value) {
        LDEBUG << "parsing value '" << sub.key << "', missing 'type'";
    }
    sub.type = value? value : "double";

    value = zconfig_resolve(config, "list", NULL);
    if (!value) {
        LDEBUG << "parsing value '" << sub.key << "', missing 'list'";
    }
    sub.list = value? value : "false";

    return sub;
}


vector<fncs::Subscription> fncs::subscriptions_parse(zconfig_t *config)
{
    vector<fncs::Subscription> subs;
    string name;
    zconfig_t *child = NULL;

    name = zconfig_name(config);
    if (name != "values") {
        LFATAL << "error parsing 'values', wrong config object '" << name << "'";
        die();
    }

    child = zconfig_child(config);
    while (child) {
        subs.push_back(subscription_parse(child));
        child = zconfig_next(child);
    }

    return subs;
}


string fncs::to_string(zframe_t *frame)
{
    return string((const char *)zframe_data(frame), zframe_size(frame));
}


/* I don't think the following behavior is what is wanted. */
#if 0

string fncs::get_value(const string &key)
{
    if (0 == cache.count(key)) {
        LWARNING << "key '" << key << "' not found in cache";
        if (0 == cache_list.count(key)) {
            LFATAL << "key '" << key << "' not found in cache or cache list";
            die();
        }
        else {
            return get_values(key).back();
        }
    }
    return cache[key];
}


vector<string> fncs::get_values(const string &key)
{
    if (0 == cache_list.count(key)) {
        LWARNING << "key '" << key << "' not found in cache list";
        if (0 == cache.count(key)) {
            LFATAL << "key '" << key << "' not found in cache list or cache";
            die();
        }
        else {
            return vector<string>(1, get_value(key));
        }
    }
    return cache_list[key];
}

#else
/* This seems like clearer semantics. */

string fncs::get_value(const string &key)
{
    if (0 == cache.count(key)) {
        LFATAL << "key '" << key << "' not found in cache";
        die();
    }
    return cache[key];
}


vector<string> fncs::get_values(const string &key)
{
    if (0 == cache_list.count(key)) {
        LFATAL << "key '" << key << "' not found in cache list";
        die();
    }
    return cache_list[key];
}
#endif
