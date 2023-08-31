#pragma once
#include <stddef.h>
#include <stdint.h>

// Identifier of the used backend.
typedef size_t BackendHandle;

typedef enum {
    OIF_INT = 1,
    OIF_FLOAT32 = 2,
    OIF_FLOAT64 = 3,
    OIF_FLOAT32_P = 4,
    OIF_FLOAT64_P = 5,
    OIF_STR = 6,
} OIFArgType;


typedef struct {
    size_t num_args;
    OIFArgType *arg_types;
    void **arg_values;
} OIFArgs;

// This structure closely follows PyArray_Object that describes NumPy arrays.
typedef struct {
    // Number of dimensions in the array.
    int nd;
    // Size of each axis, i = 0, .., nd-1.
    intptr_t *dimensions;
    // Pointer to actual data.
    char *data;
} OIFArray;

OIFArray *create_array_f64(int nd, intptr_t *dimensions);
void free_array(OIFArray *x);

BackendHandle
oif_init_backend(
    const char *backend, const char *interface, int major, int minor
);
