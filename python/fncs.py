import ctypes
import sys

_libname = "libfncs.so"

if sys.platform == 'darwin':
    _libname = "libfncs.dylib"
elif sys.platform == 'Windows':
    _libname = "libfncs"

_lib = ctypes.CDLL(_libname)

_initialize = _lib.fncs_initialize
_initialize.argtypes = []
_initialize.restype = None

_initialize_config = _lib.fncs_initialize_config
_initialize_config.argtypes = [ctypes.c_char_p]
_initialize_config.restype = None

def initialize(config=None):
    if config:
        _initialize_config(config)
    else:
        _initialize()

time_request = _lib.fncs_time_request
time_request.argtypes = [ctypes.c_ulonglong]
time_request.restype = ctypes.c_ulonglong

publish = _lib.fncs_publish
publish.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
publish.restype = None

route = _lib.fncs_route
route.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
route.restype = None

die = _lib.fncs_die
die.argtypes = []
die.restype = None

finalize = _lib.fncs_finalize
finalize.argtypes = []
finalize.restype = None

update_time_delta = _lib.fncs_update_time_delta
update_time_delta.argtypes = [ctypes.c_ulonglong]
update_time_delta.restype = None

_free_char_p = _lib._fncs_free_char_p
_free_char_p.argtypes = [ctypes.c_char_p]
_free_char_p.restype = None

_free_char_pp = _lib._fncs_free_char_pp
_free_char_pp.argtypes = [ctypes.POINTER(ctypes.c_char_p), ctypes.c_size_t]
_free_char_pp.restype = None

get_events_size = _lib.fncs_get_events_size
get_events_size.argtypes = []
get_events_size.restype = ctypes.c_size_t

_get_events = _lib.fncs_get_events
_get_events.argtypes = []
_get_events.restype = ctypes.POINTER(ctypes.c_char_p)

def get_events():
    _events = _get_events()
    size = get_events_size()
    events = [_events[i].value for i in xrange(size)]
    _free_char_pp(_events, size)
    return events

_get_value = _lib.fncs_get_value
_get_value.argtypes = [ctypes.c_char_p]
_get_value.restype = ctypes.POINTER(ctypes.c_char)

def get_value(key):
    raw = _get_value(key)
    cast = ctypes.cast(raw, ctypes.c_char_p)
    string = cast.value
    _lib._fncs_free_char_p(cast)
    return string

get_values_size = _lib.fncs_get_values_size
get_values_size.argtypes = [ctypes.c_char_p]
get_values_size.restype = ctypes.c_size_t

_get_values = _lib.fncs_get_values
_get_values.argtypes = [ctypes.c_char_p]
_get_values.restype = ctypes.POINTER(ctypes.c_char_p)
def get_values(key):
    _values = _get_values(key)
    size = get_values_size(key)
    values = [_values[i].value for i in xrange(size)]
    _free_char_pp(_values, size)
    return values

get_name = _lib.fncs_get_name
get_name.argtypes = []
get_name.restype = ctypes.c_char_p

get_id =_lib.fncs_get_id
get_id.argtypes = []
get_id.restype = ctypes.c_int

get_simulator_count =_lib.fncs_get_simulator_count
get_simulator_count.argtypes = []
get_simulator_count.restype = ctypes.c_int
