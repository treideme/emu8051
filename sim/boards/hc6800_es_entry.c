/* hc6800_es_entry.c - the shared object's entry point (see ../plugin.h)
 * Copyright 2026 Thomas Reidemeister, MIT License (see ../devices/hc573.h)
 *
 * Only the shared object (build/hc6800_es.so, hc6800_es.dll) is built from
 * this file. A program that links the board in (libem8051sim does) leaves
 * it out and calls hc6800_es_plugin_init() through sim_add_plugin(), so
 * two plugins linked into one program never both define
 * emu8051_plugin_init.
 */
#include "hc6800_es_plugin.h"

EMU8051_PLUGIN_EXPORT int emu8051_plugin_init(const emu8051_host_t *aHost, emu8051_plugin_t *aPlugin,
                                              const char *aArgs)
{
    return hc6800_es_plugin_init(aHost, aPlugin, aArgs);
}
