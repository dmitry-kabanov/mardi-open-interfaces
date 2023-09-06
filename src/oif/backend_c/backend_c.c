#include <dlfcn.h>
#include <ffi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <oif/api.h>
#include <oif/backend_api.h>
#include <oif/dispatch.h>


BackendHandle load_backend(
    const char *operation,
    size_t version_major,
    size_t version_minor)
{
    return BACKEND_C;
}

const char *
resolve_service_name(const char method[static 1]) {
    if (strcmp(method, "solve_qeq") == 0) {
        return "./liboif_backend_c_qeq.so";
    }

    return NULL;
}


int run_interface_method(const char *method, OIFArgs *in_args, OIFArgs *out_args)
{
    const char *service_name = resolve_service_name(method);
    if (service_name == NULL) {
        fprintf(
            stderr,
            "[backend_c] Could not resolve the name of the library implementing method\n"
        );
        fprintf(stderr, "[backend_c] dlerror() = %s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    void *service_lib = dlopen(service_name, RTLD_LOCAL | RTLD_LAZY);
    if (service_lib == NULL) {
        fprintf(
            stderr,
            "[backend_c] Could not load service library '%s'\n",
            service_name
        );
        fprintf(stderr, "[backend_c] dlerror() = %s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    void *func = dlsym(service_lib, method);
    if (func == NULL)
    {
        fprintf(stderr, "[dispatch] Cannot load interface '%s'\n", method);
        fprintf(stderr, "[backend_c] dlerror() = %s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    int num_in_args = in_args->num_args;
    int num_out_args = out_args->num_args;
    int num_total_args = num_in_args + num_out_args;

    ffi_cif cif;
    ffi_type **arg_types = malloc(num_total_args * sizeof(ffi_type));
    void **arg_values = malloc(num_total_args * sizeof(void *));

    // Merge input and output argument types together in `arg_types` array.
    for (size_t i = 0; i < num_in_args; ++i)
    {
        if (in_args->arg_types[i] == OIF_FLOAT64)
        {
            arg_types[i] = &ffi_type_double;
        }
        else if (in_args->arg_types[i] == OIF_ARRAY_F64)
        {
            arg_types[i] = &ffi_type_pointer;
        }
        else
        {
            fflush(stdout);
            fprintf(stderr, "[dispatch] Unknown input arg type: %d\n", in_args->arg_types[i]);
            exit(EXIT_FAILURE);
        }
    }
    for (size_t i = num_in_args; i < num_total_args; ++i)
    {
        printf("Processing out_args[%zu] = %u\n", i - num_in_args, out_args->arg_types[i - num_in_args]);
        if (out_args->arg_types[i - num_in_args] == OIF_FLOAT64)
        {
            arg_types[i] = &ffi_type_double;
        }
        else if (out_args->arg_types[i - num_in_args] == OIF_ARRAY_F64)
        {
            arg_types[i] = &ffi_type_pointer;
        }
        else
        {
            fflush(stdout);
            fprintf(stderr, "[dispatch] Unknown output arg type: %d\n", out_args->arg_types[i - num_in_args]);
            exit(EXIT_FAILURE);
        }
    }

    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, num_total_args, &ffi_type_uint, arg_types);
    if (status == FFI_OK)
    {
        printf("[backend_c] ffi_prep_cif returned FFI_OK\n");
    }
    else
    {
        fflush(stdout);
        fprintf(stderr, "[dispatch] ffi_prep_cif was not OK");
        exit(EXIT_FAILURE);
    }

    // Merge input and output argument values together in `arg_values` array.
    for (size_t i = 0; i < num_in_args; ++i)
    {
        arg_values[i] = in_args->arg_values[i];
    }
    for (size_t i = num_in_args; i < num_total_args; ++i)
    {
        arg_values[i] = out_args->arg_values[i - num_in_args];
    }

    unsigned result;
    ffi_call(&cif, FFI_FN(func), &result, arg_values);

    printf("Result is %u\n", result);
    fflush(stdout);

    return 0;
}

