#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include <julia.h>

#include <oif/api.h>
#include <oif/dispatch_api.h>
#include <oif/_platform.h>

static char *prefix_ = "dispatch_julia";

enum {
    BUFFER_SIZE_ = 32,
    JULIA_MAX_MODULE_NAME_,
};

static const char *OIF_IMPL_ROOT_DIR = NULL;

static bool INITIALIZED_ = false;

static jl_module_t *CALLBACK_MODULE_;

typedef struct {
    ImplInfo base;
    char module_name[64];
    jl_module_t *module;
    jl_value_t *self;
} JuliaImplInfo;


static void
handle_exception_(void)
{
    jl_value_t *exc = jl_exception_occurred();
    jl_value_t *sprint_fn = jl_get_function(jl_base_module, "sprint");
    jl_value_t *showerror_fn = jl_get_function(jl_base_module, "showerror");
    jl_value_t *catch_backtrace_fn = jl_get_function(jl_base_module, "catch_backtrace");

    jl_value_t *backtrace = jl_call0(catch_backtrace_fn);
    const char *exc_msg = jl_string_ptr(jl_call3(sprint_fn, showerror_fn, exc, backtrace));
    printf("[%s] ERROR: %s\n", prefix_, exc_msg);

    jl_exception_clear();
}

static int
init_module_(void)
{
    OIF_IMPL_ROOT_DIR = getenv("OIF_IMPL_ROOT_DIR");
    if (OIF_IMPL_ROOT_DIR == NULL) {
        fprintf(stderr,
                "[dispatch] Environment variable 'OIF_IMPL_ROOT_DIR' must be "
                "set so that implementations can be found. Cannot proceed\n");
        return -1;
    }

    jl_init();
    static_assert(sizeof(int) == 4, "The code is written in assumption that C int is 32-bit");

    INITIALIZED_ = true;
    return 0;
}

/**
 * Build a Julia tuple from an `intptr_t` array.
 *
 * Due to complicated Julia C API, the tuple is created via `jl_eval_string`.
 *
 * @param dimensions Array with elements that are non-negative numbers
 * @param ndims Number of the elements in the array
 * @return Julia tuple on success, `NULL` otherwise
 */
static jl_value_t *
build_julia_tuple_from_size_t_array(intptr_t *dimensions, size_t ndims)
{
    char *tuple_string = malloc(sizeof(char) * 1024);
    char *cursor = tuple_string;
    char *right_bound = &tuple_string[1024 - 1];
    intptr_t diff = right_bound - cursor;
    snprintf(tuple_string, diff, "%s", "(");
    cursor++;
    char buffer[BUFFER_SIZE_];
    int chars_written;
    jl_value_t *tuple = NULL;

    for (size_t i = 0; i < ndims; ++i) {
        chars_written = snprintf(buffer, BUFFER_SIZE_, "%zu", dimensions[i]);
        assert(chars_written >= 1);
        if (chars_written >= BUFFER_SIZE_) {
            goto report_too_long;
        }
        diff = right_bound - cursor;
        chars_written = snprintf(cursor, diff, "%s", buffer);
        assert(chars_written >= 1);
        if (chars_written >= diff) {
            goto report_too_long;
        }
        cursor += chars_written;
        diff = right_bound - cursor;
        if (i < ndims - 1 || ndims == 1) {
            chars_written = snprintf(cursor, diff, "%s", ",");
            assert(chars_written >= 1);
            if (chars_written >= diff) {
                goto report_too_long;
            }
            cursor++;
        }
    }
    diff = right_bound - cursor;
    chars_written = snprintf(cursor, diff, "%s", ")");
    if (chars_written >= diff) {
        fprintf(stderr, "ERROR: the string to copy is too long\n");
        exit(1);
    }

    tuple = jl_eval_string(tuple_string);
    if (jl_exception_occurred()) {
        const char *p = jl_string_ptr(
            jl_eval_string("sprint(showerror, ccall(:jl_exception_occurred, Any, ()))"));
        fprintf(stderr, "%s\n", p);
    }
    goto cleanup;

report_too_long:
    fprintf(stderr,
            "[build_julia_tuple_from_size_t_array] The string representation of the julia "
            "tuple does not fit in the buffer\n");

cleanup:
    free(tuple_string);

    return tuple;
}

static jl_value_t *
make_wrapper_over_c_callback(OIFCallback *p)
{
    jl_value_t *wrapper = NULL;
    if (CALLBACK_MODULE_ == NULL) {
        char include_statement[512];
        int nchars_written = snprintf(include_statement, 512, "include(\"%s/oif_impl/lang_julia/callback.jl\")", OIF_IMPL_ROOT_DIR);
        if (nchars_written >= 512-1) {
            fprintf(
                stderr,
                "[%s] Could not execute include statement for `callback.jl` "
                "while the provided buffer is only 512 characters, and %d "
                "characters are supposed to be written\n",
                prefix_, nchars_written + 1
            );
        }
        jl_eval_string(include_statement);
        if (jl_exception_occurred()) {
            handle_exception_();
            goto cleanup;
        }
        jl_eval_string("import .CallbackWrapper");
        if (jl_exception_occurred()) {
            handle_exception_();
            goto cleanup;
        }
        CALLBACK_MODULE_ = (jl_module_t *)jl_eval_string("CallbackWrapper");
        if (jl_exception_occurred()) {
            handle_exception_();
            goto cleanup;
        }
    }

    jl_function_t *fn_callback = jl_get_function(CALLBACK_MODULE_, "make_wrapper_over_c_callback");   
    assert(fn_callback != NULL);
    assert(p->fn_p_c != NULL);
    jl_value_t *fn_p_c_wrapped = jl_box_voidpointer(p->fn_p_c);
    wrapper = jl_call1(fn_callback, fn_p_c_wrapped);

cleanup:
    return wrapper;
}

ImplInfo *
load_impl(const char *impl_details, size_t version_major, size_t version_minor)
{
    int status = 0;
    if (! INITIALIZED_) {
        status = init_module_();
        if (status) {
            return NULL;
        }
    }
    (void)version_major;
    (void)version_minor;
    JuliaImplInfo *result = NULL;

    char module_filename[512] = "\0";
    char module_name[512] = "\0";
    size_t i;
    for (i = 0; i < strlen(impl_details); ++i) {
        if (impl_details[i] != ' ' && impl_details[i] != '\0') {
            module_filename[i] = impl_details[i];
        }
        else {
            module_filename[i] = '\0';
            break;
        }
    }
    size_t offset = i + 1;
    for (; i < strlen(impl_details); ++i) {
        if (impl_details[i] != ' ' && impl_details[i] != '\0') {
            module_name[i - offset] = impl_details[i];
        }
        else {
            module_name[i] = '\0';
        }
    }

    fprintf(stderr, "[%s] Provided module filename: '%s'\n", prefix_, module_filename);
    fprintf(stderr, "[%s] Provided module name: '%s'\n", prefix_, module_name);

    char include_statement[1024];
    sprintf(include_statement, "include(\"%s/oif_impl/impl/%s\")", OIF_IMPL_ROOT_DIR, module_filename);
    char import_statement[1024];
    sprintf(import_statement, "import .%s", module_name);

    jl_value_t *retval;
    retval = jl_eval_string(include_statement);
    if (jl_exception_occurred()) {
        goto catch;
    }

    retval = jl_eval_string(import_statement);
    if (jl_exception_occurred()) {
        goto catch;
    }

    jl_module_t *module = (jl_module_t *)jl_eval_string(module_name);
    if (jl_exception_occurred()) {
        goto catch;
    }

    result = malloc(sizeof *result);
    if (result == NULL) {
        fprintf(stderr,
                "[%s] Could not allocate memory for Julia implementation information\n",
                prefix_);
        goto finally;
    }
    status = snprintf(result->module_name, JULIA_MAX_MODULE_NAME_, "%s", module_name);
    if (status < 0 || status >= JULIA_MAX_MODULE_NAME_) {
        fprintf(
            stderr, "[%s] Module names in Julia cannot be longer than %d characters\n",
            prefix_, JULIA_MAX_MODULE_NAME_ - 1
        );
        goto catch;
    }
    result->module = module;

    char self_statement[512];
    strcpy(self_statement, module_name);
    strcat(self_statement, ".Self()");
    jl_value_t *self = jl_eval_string(self_statement);
    if (jl_exception_occurred()) {
        goto catch;
    }
    result->self = self;

    goto finally;

catch:
    handle_exception_();

    free(result);
    result = NULL;

finally:
    return (ImplInfo *)result;
}

int
unload_impl(ImplInfo *impl_info_)
{
    assert(impl_info_->dh == OIF_LANG_JULIA);
    JuliaImplInfo *impl_info = (JuliaImplInfo *)impl_info_;
    free(impl_info);

    jl_atexit_hook(0);
    return 0;
}

int
call_impl(ImplInfo *impl_info_, const char *method, OIFArgs *in_args, OIFArgs *out_args)
{
    int result = -1;

    if (impl_info_->dh != OIF_LANG_JULIA) {
        fprintf(stderr, "[%s] Provided implementation is not in Julia\n", prefix_);
        return -1;
    }
    JuliaImplInfo *impl_info = (JuliaImplInfo *)impl_info_;

    assert(in_args->num_args < INT32_MAX);
    assert(out_args->num_args < INT32_MAX);
    int32_t in_num_args = (int32_t)in_args->num_args;
    int32_t out_num_args = (int32_t)out_args->num_args;
    int32_t num_args = in_num_args + out_num_args + 1;

    jl_value_t **julia_args;
    JL_GC_PUSHARGS(julia_args, num_args);  // NOLINT

    julia_args[0] = impl_info->self;

    jl_value_t *cur_julia_arg;
    for (int32_t i = 0; i < in_num_args; ++i) {
        if (in_args->arg_types[i] == OIF_FLOAT64) {
            cur_julia_arg = jl_box_float64(*(double *)in_args->arg_values[i]);
        }
        else if (in_args->arg_types[i] == OIF_ARRAY_F64) {
            OIFArrayF64 *oif_array = *(OIFArrayF64 **)in_args->arg_values[i];
            jl_value_t *arr_type = jl_apply_array_type((jl_value_t *)jl_float64_type, 1);
            jl_value_t *dims =
                build_julia_tuple_from_size_t_array(oif_array->dimensions, oif_array->nd);
            bool own_buffer = false;
            cur_julia_arg =
                (jl_value_t *)jl_ptr_to_array(arr_type, oif_array->data, (jl_value_t *)dims, own_buffer);
        }
        else if (in_args->arg_types[i] == OIF_CALLBACK) {
            OIFCallback *p = in_args->arg_values[i];
            if (p->src == OIF_LANG_JULIA) {
                fprintf(
                    stderr,
                    "[%s] This code path is not implemented yet\n",
                    prefix_
                );
                exit(1);
            }
            else {
                jl_value_t *wrapper = make_wrapper_over_c_callback(p);
                cur_julia_arg = wrapper;
            }
        }
        else {
            fprintf(stderr,
                    "[%s] Cannot convert input argument #%d with "
                    "provided type id %d\n",
                    prefix_, i, in_args->arg_types[i]);
            goto cleanup;
        }
        julia_args[i + 1] = cur_julia_arg;
    }

    for (int32_t i = 0; i < out_num_args; ++i) {
        if (out_args->arg_types[i] == OIF_FLOAT64) {
            cur_julia_arg = jl_box_float64(*(float *)out_args->arg_values[i]);
        }
        else if (out_args->arg_types[i] == OIF_ARRAY_F64) {
            OIFArrayF64 *oif_array = *(OIFArrayF64 **)out_args->arg_values[i];
            jl_value_t *arr_type = jl_apply_array_type((jl_value_t *)jl_float64_type, 1);
            jl_value_t *dims =
                build_julia_tuple_from_size_t_array(oif_array->dimensions, oif_array->nd);
            bool own_buffer = false;
            cur_julia_arg = (jl_value_t *)jl_ptr_to_array(arr_type, oif_array->data,
                                                          (jl_value_t *)dims, own_buffer);
        }
        else {
            fprintf(stderr,
                    "[%s] Cannot convert output argument #%d with "
                    "provided type id %d\n",
                    prefix_, i, in_args->arg_types[i]);
            goto cleanup;
        }

        julia_args[i + 1 + in_num_args] = cur_julia_arg;
    }

    jl_function_t *fn;
    fn = jl_get_function(impl_info->module, method);
    if (fn == NULL) {
        fprintf(stderr, "[%s] Could not find method '%s' in implementation with id %d\n",
                prefix_, method, impl_info->base.implh);
        goto cleanup;
    }

    jl_value_t *retval_ = jl_call(fn, julia_args, num_args);
    if (jl_exception_occurred()) {
        handle_exception_();
        goto cleanup;
    }

    if (retval_ == jl_nothing) {
        result = 0;
    } else {
        if (jl_typeis(retval_, jl_int64_type)) {
            result = (int) jl_unbox_int64(retval_);
        }
        else if (jl_typeis(retval_, jl_int32_type)) {
            result = jl_unbox_int32(retval_);
        }
        else {
            fprintf(
                stderr,
                "[%s] Return value from calling a Julia implementation's "
                "method '%s' is not of one of the following types "
                "{nothing, int32, int64} and cannot be converted.\n",
                prefix_, method
            );
            goto cleanup;
        }
    }
    assert(result == 0);

cleanup:
    JL_GC_POP();

    return result;
}
