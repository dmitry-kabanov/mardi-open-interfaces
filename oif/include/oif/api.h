#pragma once
#include <stddef.h>
#include <stdint.h>

// Handle to an instantiated implementation.
typedef int ImplHandle;

typedef enum {
    OIF_INT = 1,
    OIF_FLOAT32 = 2,
    OIF_FLOAT64 = 3,
    OIF_FLOAT32_P = 4,
    OIF_ARRAY_F64 = 5,
    OIF_STR = 6,
    OIF_CALLBACK = 7,
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
    double *data;
} OIFArrayF64;

// This structure is used for callback functions.
typedef struct {
    int src;        // Language of the function (one of OIF_LANG_* constants)
    void *fn_p_py;  // Function pointer in Python
    void *fn_p_c;   // Function pointer in C
} OIFCallback;

enum {
    OIF_ERROR = -1,
    OIF_IMPL_INIT_ERROR = -2,
};
