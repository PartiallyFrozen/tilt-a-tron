#pragma once

typedef struct {
    const char *name;
    const void *addr;
} loader_export_t;

// The address the console exports under `name`, or NULL if it exports no such thing.
const void *loader_lookup_export(const char *name);
