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
        '        GoldfishTraceData data;',
        cond=cond)

    out('        data.type = GOLDFISH_TRACE_EVENT_START;',
        '        data.name = "%(name)s";',
        '        goldfish_trace_callback(&data);',
        name=event.name)

    for type_, name in event.args:
        out('        data.name = "%s";' % name)
        # Pointers
        if '*' in type_:
            if 'char*' in type_ or 'char *' in type_:
                # Check for string array (char**)
                if 'char**' in type_ or 'char **' in type_:
                     out('        data.type = GOLDFISH_TRACE_ARG_STRING_ARRAY;',
                         '        data.value.sa = (const char* const*)%s;' % name)
                else:
                     out('        data.type = GOLDFISH_TRACE_ARG_STRING;',
                         '        data.value.s = %s ? %s : "NULL";' % (name, name))
            else:
                out('        data.type = GOLDFISH_TRACE_ARG_POINTER;',
                    '        data.value.p = %s;' % name)
        # Booleans
        elif 'bool' in type_:
            out('        data.type = GOLDFISH_TRACE_ARG_BOOL;',
                '        data.value.b = %s;' % name)
        # Unsigned Integers
        elif 'uint' in type_ or 'unsigned' in type_ or 'size_t' in type_:
            out('        data.type = GOLDFISH_TRACE_ARG_UINT64;',
                '        data.value.u = (uint64_t)%s;' % name)
        # Signed Integers and others
        else:
            out('        data.type = GOLDFISH_TRACE_ARG_INT64;',
                '        data.value.i = (int64_t)%s;' % name)

        out('        goldfish_trace_callback(&data);')

    out('        data.type = GOLDFISH_TRACE_EVENT_END;',
        '        data.name = "%(name)s";',
        '        goldfish_trace_callback(&data);',
        name=event.name)

    out('    }')
    out('}',
        '')
