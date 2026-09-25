# -*- coding: utf-8 -*-

"""
Goldfish backend.
"""

__author__     = "Gemini CLI"
__copyright__  = "Copyright 2026, Google LLC"
__license__    = "GPL version 2 or (at your option) any later version"

from tracetool import out

PUBLIC = True

def generate_h_begin(events, group):
    out('#include "qemu/osdep.h"',
        '#include "trace/goldfish.h"',
        '')
    for event in events:
        out('void _goldfish_%(api)s(%(args)s);',
            api=event.api(),
            args=event.args)
    out('')

def generate_h(event, group):
    out('    _goldfish_%(api)s(%(args)s);',
        api=event.api(),
        args=", ".join(event.args.names()))

def generate_h_backend_dstate(event, group):
    out('    trace_event_get_state_dynamic_by_id(%(event_id)s) || \\',
        event_id="TRACE_" + event.name.upper())

def generate_c_begin(events, group):
    out('#include "qemu/osdep.h"',
        '#include "trace/control.h"',
        '#include "trace/goldfish.h"',
        '')

def generate_c(event, group):
    out('void _goldfish_%(api)s(%(args)s)',
        '{',
        api=event.api(),
        args=event.args)

    cond = "trace_event_get_state(%s)" % ("TRACE_" + event.name.upper())
    out('    if (%(cond)s) {',
        '        GoldfishTraceData _trace_data;',
        cond=cond)

    out('        _trace_data.type = GOLDFISH_TRACE_EVENT_START;',
        '        _trace_data.name = "%(name)s";',
        '        goldfish_trace_callback(&_trace_data);',
        name=event.name)

    for type_, name in event.args:
        out('        _trace_data.name = "%s";' % name)
        # Pointers
        if '*' in type_:
            if 'char*' in type_ or 'char *' in type_:
                # Check for string array (char**)
                if 'char**' in type_ or 'char **' in type_:
                     out('        _trace_data.type = GOLDFISH_TRACE_ARG_STRING_ARRAY;',
                         '        _trace_data.value.sa = (const char* const*)%s;' % name)
                else:
                     out('        _trace_data.type = GOLDFISH_TRACE_ARG_STRING;',
                         '        _trace_data.value.s = %s ? %s : "NULL";' % (name, name))
            else:
                out('        _trace_data.type = GOLDFISH_TRACE_ARG_POINTER;',
                    '        _trace_data.value.p = %s;' % name)
        # Booleans
        elif 'bool' in type_:
            out('        _trace_data.type = GOLDFISH_TRACE_ARG_BOOL;',
                '        _trace_data.value.b = %s;' % name)
        # Unsigned Integers
        elif 'uint' in type_ or 'unsigned' in type_ or 'size_t' in type_:
            out('        _trace_data.type = GOLDFISH_TRACE_ARG_UINT64;',
                '        _trace_data.value.u = (uint64_t)%s;' % name)
        # Signed Integers and others
        else:
            out('        _trace_data.type = GOLDFISH_TRACE_ARG_INT64;',
                '        _trace_data.value.i = (int64_t)%s;' % name)

        out('        goldfish_trace_callback(&_trace_data);')

    out('        _trace_data.type = GOLDFISH_TRACE_EVENT_END;',
        '        _trace_data.name = "%(name)s";',
        '        goldfish_trace_callback(&_trace_data);',
        name=event.name)

    out('    }')
    out('}',
        '')
