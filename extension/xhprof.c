/*
 *  Copyright (c) 2009 Facebook
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef linux
/* To enable CPU_ZERO and CPU_SET, etc.     */
# define _GNU_SOURCE
#endif

#include <curl/curl.h>
#include <curl/easy.h>
#include "ext/curl/php_curl.h"
#include "ext/pcre/php_pcre.h"
#include "ext/standard/url.h"
#include "ext/pdo/php_pdo_driver.h"
#include "zend_exceptions.h"

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"
#include "php_xhprof.h"
#include "zend_extensions.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __FreeBSD__
# if __FreeBSD_version >= 700110
#   include <sys/resource.h>
#   include <sys/cpuset.h>
#   define cpu_set_t cpuset_t
#   define SET_AFFINITY(pid, size, mask) \
           cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
#   define GET_AFFINITY(pid, size, mask) \
           cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, size, mask)
# else
#   error "This version of FreeBSD does not support cpusets"
# endif /* __FreeBSD_version */
#elif __APPLE__
/*
 * Patch for compiling in Mac OS X Leopard
 * @author Svilen Spasov <s.spasov@gmail.com>
 */
#    include <mach/mach_init.h>
#    include <mach/thread_policy.h>
#    define cpu_set_t thread_affinity_policy_data_t
#    define CPU_SET(cpu_id, new_mask) \
        (*(new_mask)).affinity_tag = (cpu_id + 1)
#    define CPU_ZERO(new_mask)                 \
        (*(new_mask)).affinity_tag = THREAD_AFFINITY_TAG_NULL
#   define SET_AFFINITY(pid, size, mask)       \
        thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY, mask, \
                          THREAD_AFFINITY_POLICY_COUNT)
#else
/* For sched_getaffinity, sched_setaffinity */
# include <sched.h>
# define SET_AFFINITY(pid, size, mask) sched_setaffinity(0, size, mask)
# define GET_AFFINITY(pid, size, mask) sched_getaffinity(0, size, mask)
#endif /* __FreeBSD__ */



/**
 * **********************
 * GLOBAL MACRO CONSTANTS
 * **********************
 */

/* XHProf version                           */
#define XHPROF_VERSION       "0.9.7"

/* Fictitious function name to represent top of the call tree. The paranthesis
 * in the name is to ensure we don't conflict with user function names.  */
#define ROOT_SYMBOL                "main()"

/* Size of a temp scratch buffer            */
#define SCRATCH_BUF_LEN            512

/* Various XHPROF modes. If you are adding a new mode, register the appropriate
 * callbacks in hp_begin() */
#define XHPROF_MODE_HIERARCHICAL            1
#define XHPROF_MODE_SAMPLED            620002      /* Rockfort's zip code */

/* Hierarchical profiling flags.
 *
 * Note: Function call counts and wall (elapsed) time are always profiled.
 * The following optional flags can be used to control other aspects of
 * profiling.
 */
#define XHPROF_FLAGS_NO_BUILTINS   0x0001         /* do not profile builtins */
#define XHPROF_FLAGS_CPU           0x0002      /* gather CPU times for funcs */
#define XHPROF_FLAGS_MEMORY        0x0004   /* gather memory usage for funcs */

/* Constants for XHPROF_MODE_SAMPLED        */
#define XHPROF_SAMPLING_INTERVAL       100000      /* In microsecs        */

/* Constant for ignoring functions, transparent to hierarchical profile */
#define XHPROF_MAX_FILTERED_FUNCTIONS  256
#define XHPROF_FILTERED_FUNCTION_SIZE                           \
               ((XHPROF_MAX_FILTERED_FUNCTIONS + 7)/8)
#define XHPROF_MAX_ARGUMENT_LEN 256

#if !defined(uint64)
typedef unsigned long long uint64;
#endif
#if !defined(uint32)
typedef unsigned int uint32;
#endif
#if !defined(uint8)
typedef unsigned char uint8;
#endif


/**
 * *****************************
 * GLOBAL DATATYPES AND TYPEDEFS
 * *****************************
 */

/* XHProf maintains a stack of entries being profiled. The memory for the entry
 * is passed by the layer that invokes BEGIN_PROFILING(), e.g. the hp_execute()
 * function. Often, this is just C-stack memory.
 *
 * This structure is a convenient place to track start time of a particular
 * profile operation, recursion depth, and the name of the function being
 * profiled. */
typedef struct hp_entry_t {
  char                   *name_hprof;                       /* function name */
  int                     rlvl_hprof;        /* recursion level for function */
  uint64                  tsc_start;         /* start value for TSC counter  */
  long int                mu_start_hprof;                    /* memory usage */
  long int                pmu_start_hprof;              /* peak memory usage */
  struct rusage           ru_start_hprof;             /* user/sys time start */
  struct hp_entry_t      *prev_hprof;    /* ptr to prev entry being profiled */
  uint8                   hash_code;     /* hash_code for the function name  */
} hp_entry_t;

/* Various types for XHPROF callbacks       */
typedef void (*hp_init_cb)           (TSRMLS_D);
typedef void (*hp_exit_cb)           (TSRMLS_D);
typedef void (*hp_begin_function_cb) (hp_entry_t **entries,
                                      hp_entry_t *current   TSRMLS_DC);
typedef void (*hp_end_function_cb)   (hp_entry_t **entries  TSRMLS_DC);

/* Struct to hold the various callbacks for a single xhprof mode */
typedef struct hp_mode_cb {
  hp_init_cb             init_cb;
  hp_exit_cb             exit_cb;
  hp_begin_function_cb   begin_fn_cb;
  hp_end_function_cb     end_fn_cb;
} hp_mode_cb;

/* Xhprof's global state.
 *
 * This structure is instantiated once.  Initialize defaults for attributes in
 * hp_init_profiler_state() Cleanup/free attributes in
 * hp_clean_profiler_state() */
typedef struct hp_global_t {

  /*       ----------   Global attributes:  -----------       */

  /* Indicates if xhprof is currently enabled */
  int              enabled;

  /* Indicates if xhprof was ever enabled during this request */
  int              ever_enabled;

  /* Holds all information about layer profiling */
  zval            *layers_count;

  /* A key=>value list of function calls to their respective layers. */
  HashTable       *layers_definition;

  /* Holds all the xhprof statistics */
  zval            *stats_count;

  /* Holds all the information about last error. */
  zval            *last_error;

  /* Holds the last exception message */
  zval            *last_exception_message;

  /* Holds the last exception class and code */
  zval            *last_exception_type;

  /* Indicates the current xhprof mode or level */
  int              profiler_level;

  /* Top of the profile stack */
  hp_entry_t      *entries;

  /* freelist of hp_entry_t chunks for reuse... */
  hp_entry_t      *entry_free_list;

  /* Callbacks for various xhprof modes */
  hp_mode_cb       mode_cb;

  /*       ----------   Mode specific attributes:  -----------       */

  /* Global to track the time of the last sample in time and ticks */
  struct timeval   last_sample_time;
  uint64           last_sample_tsc;
  /* XHPROF_SAMPLING_INTERVAL in ticks */
  uint64           sampling_interval_tsc;

  /* This array is used to store cpu frequencies for all available logical
   * cpus.  For now, we assume the cpu frequencies will not change for power
   * saving or other reasons. If we need to worry about that in the future, we
   * can use a periodical timer to re-calculate this arrary every once in a
   * while (for example, every 1 or 5 seconds). */
  double *cpu_frequencies;

  /* The number of logical CPUs this machine has. */
  uint32 cpu_num;

  /* The saved cpu affinity. */
  cpu_set_t prev_mask;

  /* The cpu id current process is bound to. (default 0) */
  uint32 cur_cpu_id;

  /* XHProf flags */
  uint32 xhprof_flags;

  /* counter table indexed by hash value of function names. */
  uint8  func_hash_counters[256];

  /* Table of filtered function names and their filter */
  int     filtered_type; // 1 = blacklist, 2 = whitelist, 0 = nothing
  char  **filtered_function_names;
  uint8   filtered_function_filter[XHPROF_FILTERED_FUNCTION_SIZE];

  /* Table of function which get extended with their arguments */
  char  **argument_function_names;
  uint8   argument_function_filter[XHPROF_FILTERED_FUNCTION_SIZE];

} hp_global_t;


/**
 * ***********************
 * GLOBAL STATIC VARIABLES
 * ***********************
 */
/* XHProf global state */
static hp_global_t       hp_globals;

#if PHP_VERSION_ID < 50500
/* Pointer to the original execute function */
static ZEND_DLEXPORT void (*_zend_execute) (zend_op_array *ops TSRMLS_DC);

/* Pointer to the origianl execute_internal function */
static ZEND_DLEXPORT void (*_zend_execute_internal) (zend_execute_data *data,
                           int ret TSRMLS_DC);
#else
/* Pointer to the original execute function */
static void (*_zend_execute_ex) (zend_execute_data *execute_data TSRMLS_DC);

/* Pointer to the origianl execute_internal function */
static void (*_zend_execute_internal) (zend_execute_data *data,
                      struct _zend_fcall_info *fci, int ret TSRMLS_DC);
#endif

/* Pointer to the original compile function */
static zend_op_array * (*_zend_compile_file) (zend_file_handle *file_handle,
                                              int type TSRMLS_DC);

/* Pointer to the original compile string function (used by eval) */
static zend_op_array * (*_zend_compile_string) (zval *source_string, char *filename TSRMLS_DC);

/* error callback replacement functions */
void (*xhprof_original_error_cb)(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);
void xhprof_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);
static void xhprof_throw_exception_hook(zval *exception TSRMLS_DC);

/* Bloom filter for function names to be ignored */
#define INDEX_2_BYTE(index)  (index >> 3)
#define INDEX_2_BIT(index)   (1 << (index & 0x7));


/**
 * ****************************
 * STATIC FUNCTION DECLARATIONS
 * ****************************
 */
static void hp_register_constants(INIT_FUNC_ARGS);

static void hp_begin(long level, long xhprof_flags TSRMLS_DC);
static void hp_stop(TSRMLS_D);
static void hp_end(TSRMLS_D);

static inline uint64 cycle_timer();
static double get_cpu_frequency();
static void clear_frequencies();

static void hp_free_the_free_list();
static hp_entry_t *hp_fast_alloc_hprof_entry();
static void hp_fast_free_hprof_entry(hp_entry_t *p);
static inline uint8 hp_inline_hash(char * str);
static void get_all_cpu_frequencies();
static long get_us_interval(struct timeval *start, struct timeval *end);
static void incr_us_interval(struct timeval *start, uint64 incr);

static void hp_parse_options_from_arg(zval *args);
static void hp_filtered_functions_filter_clear();
static void hp_filtered_functions_filter_init();

static void hp_argument_functions_filter_clear();
static void hp_argument_functions_filter_init();

static inline zval  *hp_zval_at_key(char  *key,
                                    zval  *values);
static inline char **hp_strings_in_zval(zval  *values);
static inline void   hp_array_del(char **name_array);
static inline int  hp_argument_entry(uint8 hash_code, char *curr_func);

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_xhprof_enable, 0, 0, 0)
  ZEND_ARG_INFO(0, flags)
  ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_disable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_last_fatal_error, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_enable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_xhprof_sample_disable, 0)
ZEND_END_ARG_INFO()
/* }}} */

/**
 * *********************
 * FUNCTION PROTOTYPES
 * *********************
 */
int restore_cpu_affinity(cpu_set_t * prev_mask);
int bind_to_cpu(uint32 cpu_id);

/**
 * *********************
 * PHP EXTENSION GLOBALS
 * *********************
 */
/* List of functions implemented/exposed by xhprof */
zend_function_entry xhprof_functions[] = {
  PHP_FE(xhprof_enable, arginfo_xhprof_enable)
  PHP_FE(xhprof_disable, arginfo_xhprof_disable)
  PHP_FE(xhprof_last_fatal_error, arginfo_xhprof_last_fatal_error)
  PHP_FE(xhprof_sample_enable, arginfo_xhprof_sample_enable)
  PHP_FE(xhprof_sample_disable, arginfo_xhprof_sample_disable)
  {NULL, NULL, NULL}
};

/* Callback functions for the xhprof extension */
zend_module_entry xhprof_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
  STANDARD_MODULE_HEADER,
#endif
  "xhprof",                        /* Name of the extension */
  xhprof_functions,                /* List of functions exposed */
  PHP_MINIT(xhprof),               /* Module init callback */
  PHP_MSHUTDOWN(xhprof),           /* Module shutdown callback */
  PHP_RINIT(xhprof),               /* Request init callback */
  PHP_RSHUTDOWN(xhprof),           /* Request shutdown callback */
  PHP_MINFO(xhprof),               /* Module info callback */
#if ZEND_MODULE_API_NO >= 20010901
  XHPROF_VERSION,
#endif
  STANDARD_MODULE_PROPERTIES
};

PHP_INI_BEGIN()

/* output directory:
 * Currently this is not used by the extension itself.
 * But some implementations of iXHProfRuns interface might
 * choose to save/restore XHProf profiler runs in the
 * directory specified by this ini setting.
 */
PHP_INI_ENTRY("xhprof.output_dir", "", PHP_INI_ALL, NULL)

PHP_INI_END()

/* Init module */
ZEND_GET_MODULE(xhprof)


/**
 * **********************************
 * PHP EXTENSION FUNCTION DEFINITIONS
 * **********************************
 */

/**
 * Start XHProf profiling in hierarchical mode.
 *
 * @param  long $flags  flags for hierarchical mode
 * @return void
 * @author kannan
 */
PHP_FUNCTION(xhprof_enable) {
  long  xhprof_flags = 0;                                    /* XHProf flags */
  zval *optional_array = NULL;         /* optional array arg: for future use */

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC,
                            "|lz", &xhprof_flags, &optional_array) == FAILURE) {
    return;
  }

  hp_parse_options_from_arg(optional_array);

  hp_begin(XHPROF_MODE_HIERARCHICAL, xhprof_flags TSRMLS_CC);
}

/**
 * Stops XHProf from profiling in hierarchical mode anymore and returns the
 * profile info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author kannan, hzhao
 */
PHP_FUNCTION(xhprof_disable) {
  if (hp_globals.enabled) {
    hp_stop(TSRMLS_C);

    if (hp_globals.layers_count) {
        zval *tmp, *value;
        void *data;

        if (zend_hash_find(Z_ARRVAL_P(hp_globals.stats_count), ROOT_SYMBOL, strlen(ROOT_SYMBOL) + 1, &data) == SUCCESS) {
            tmp = *(zval **) data;

            MAKE_STD_ZVAL(value);
            ZVAL_ZVAL(value, hp_globals.layers_count, 1, 0);
            add_assoc_zval(tmp, "layers", value);
        }
    }

    RETURN_ZVAL(hp_globals.stats_count, 1, 0);
  }
  /* else null is returned */
}

PHP_FUNCTION(xhprof_last_fatal_error) {
    if (hp_globals.enabled) {
      RETURN_ZVAL(hp_globals.last_error, 1, 0);
    }
}

/**
 * Start XHProf profiling in sampling mode.
 *
 * @return void
 * @author cjiang
 */
PHP_FUNCTION(xhprof_sample_enable) {
  long  xhprof_flags = 0;                                    /* XHProf flags */
  hp_parse_options_from_arg(NULL);
  hp_begin(XHPROF_MODE_SAMPLED, xhprof_flags TSRMLS_CC);
}

/**
 * Stops XHProf from profiling in sampling mode anymore and returns the profile
 * info.
 *
 * @param  void
 * @return array  hash-array of XHProf's profile info
 * @author cjiang
 */
PHP_FUNCTION(xhprof_sample_disable) {
  if (hp_globals.enabled) {
    hp_stop(TSRMLS_C);
    RETURN_ZVAL(hp_globals.stats_count, 1, 0);
  }
  /* else null is returned */
}

/**
 * Module init callback.
 *
 * @author cjiang
 */
PHP_MINIT_FUNCTION(xhprof) {
  int i;

  REGISTER_INI_ENTRIES();

  hp_register_constants(INIT_FUNC_ARGS_PASSTHRU);

  /* Get the number of available logical CPUs. */
  hp_globals.cpu_num = sysconf(_SC_NPROCESSORS_CONF);

  /* Get the cpu affinity mask. */
#ifndef __APPLE__
  if (GET_AFFINITY(0, sizeof(cpu_set_t), &hp_globals.prev_mask) < 0) {
    perror("getaffinity");
    return FAILURE;
  }
#else
  CPU_ZERO(&(hp_globals.prev_mask));
#endif

  /* Initialize cpu_frequencies and cur_cpu_id. */
  hp_globals.cpu_frequencies = NULL;
  hp_globals.cur_cpu_id = 0;

  hp_globals.stats_count = NULL;
  hp_globals.last_error = NULL;
  hp_globals.last_exception_message = NULL;
  hp_globals.last_exception_type = NULL;

  /* no free hp_entry_t structures to start with */
  hp_globals.entry_free_list = NULL;

  for (i = 0; i < 256; i++) {
    hp_globals.func_hash_counters[i] = 0;
  }

  hp_filtered_functions_filter_clear();
  hp_argument_functions_filter_clear();

#if defined(DEBUG)
  /* To make it random number generator repeatable to ease testing. */
  srand(0);
#endif
  return SUCCESS;
}

/**
 * Module shutdown callback.
 */
PHP_MSHUTDOWN_FUNCTION(xhprof) {
  /* Make sure cpu_frequencies is free'ed. */
  clear_frequencies();

  /* free any remaining items in the free list */
  hp_free_the_free_list();

  UNREGISTER_INI_ENTRIES();

  return SUCCESS;
}

/**
 * Request init callback. Nothing to do yet!
 */
PHP_RINIT_FUNCTION(xhprof) {
  return SUCCESS;
}

/**
 * Request shutdown callback. Stop profiling and return.
 */
PHP_RSHUTDOWN_FUNCTION(xhprof) {
  hp_end(TSRMLS_C);
  return SUCCESS;
}

/**
 * Module info callback. Returns the xhprof version.
 */
PHP_MINFO_FUNCTION(xhprof)
{
  char buf[SCRATCH_BUF_LEN];
  char tmp[SCRATCH_BUF_LEN];
  int i;
  int len;

  php_info_print_table_start();
  php_info_print_table_header(2, "xhprof", XHPROF_VERSION);
  len = snprintf(buf, SCRATCH_BUF_LEN, "%d", hp_globals.cpu_num);
  buf[len] = 0;
  php_info_print_table_header(2, "CPU num", buf);

  if (hp_globals.cpu_frequencies) {
    /* Print available cpu frequencies here. */
    php_info_print_table_header(2, "CPU logical id", " Clock Rate (MHz) ");
    for (i = 0; i < hp_globals.cpu_num; ++i) {
      len = snprintf(buf, SCRATCH_BUF_LEN, " CPU %d ", i);
      buf[len] = 0;
      len = snprintf(tmp, SCRATCH_BUF_LEN, "%f", hp_globals.cpu_frequencies[i]);
      tmp[len] = 0;
      php_info_print_table_row(2, buf, tmp);
    }
  }

  php_info_print_table_end();
}


/**
 * ***************************************************
 * COMMON HELPER FUNCTION DEFINITIONS AND LOCAL MACROS
 * ***************************************************
 */

static void hp_register_constants(INIT_FUNC_ARGS) {
  REGISTER_LONG_CONSTANT("XHPROF_FLAGS_NO_BUILTINS",
                         XHPROF_FLAGS_NO_BUILTINS,
                         CONST_CS | CONST_PERSISTENT);

  REGISTER_LONG_CONSTANT("XHPROF_FLAGS_CPU",
                         XHPROF_FLAGS_CPU,
                         CONST_CS | CONST_PERSISTENT);

  REGISTER_LONG_CONSTANT("XHPROF_FLAGS_MEMORY",
                         XHPROF_FLAGS_MEMORY,
                         CONST_CS | CONST_PERSISTENT);
}

/**
 * A hash function to calculate a 8-bit hash code for a function name.
 * This is based on a small modification to 'zend_inline_hash_func' by summing
 * up all bytes of the ulong returned by 'zend_inline_hash_func'.
 *
 * @param str, char *, string to be calculated hash code for.
 *
 * @author cjiang
 */
static inline uint8 hp_inline_hash(char * str) {
  ulong h = 5381;
  uint i = 0;
  uint8 res = 0;

  while (*str) {
    h += (h << 5);
    h ^= (ulong) *str++;
  }

  for (i = 0; i < sizeof(ulong); i++) {
    res += ((uint8 *)&h)[i];
  }
  return res;
}

static void hp_parse_layers_options_from_arg(zval *layers) {
    if (Z_TYPE_P(layers) != IS_ARRAY) {
        return;
    }

    HashTable *ht;
    zval *tmp;

    ALLOC_HASHTABLE(hp_globals.layers_definition);
    zend_hash_init(hp_globals.layers_definition, 0, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_copy(
        hp_globals.layers_definition,
        Z_ARRVAL_P(layers),
        (copy_ctor_func_t)zval_add_ref,
        (void*)&tmp,
        sizeof(zval *)
    );
}

/**
 * Parse the list of ignored functions from the zval argument.
 *
 * @author mpal
 */
static void hp_parse_options_from_arg(zval *args) {
  if (args == NULL) {
      hp_globals.filtered_function_names = NULL;
      hp_globals.argument_function_names = NULL;

      return;
  }

  zval  *zresult = NULL;

  zresult = hp_zval_at_key("ignored_functions", args);

  if (zresult == NULL) {
      zresult = hp_zval_at_key("functions", args);
      if (zresult != NULL) {
          hp_globals.filtered_type = 2;
      }
  } else {
      hp_globals.filtered_type = 1;
  }

  hp_globals.filtered_function_names = hp_strings_in_zval(zresult);

  zresult = hp_zval_at_key("argument_functions", args);
  hp_globals.argument_function_names = hp_strings_in_zval(zresult);

  zresult = hp_zval_at_key("layers", args);

  if (zresult && Z_TYPE_P(zresult) == IS_ARRAY) {
      hp_parse_layers_options_from_arg(zresult);
  }
}

/**
 * Clear filter for functions which may be ignored during profiling.
 *
 * @author mpal
 */
static void hp_filtered_functions_filter_clear() {
  hp_globals.filtered_type = 0;
  memset(hp_globals.filtered_function_filter, 0,
         XHPROF_FILTERED_FUNCTION_SIZE);
}

/**
 * Initialize filter for ignored functions using bit vector.
 *
 * @author mpal
 */
static void hp_filtered_functions_filter_init() {
  if (hp_globals.filtered_function_names != NULL) {
    int i = 0;
    for(; hp_globals.filtered_function_names[i] != NULL; i++) {
      char *str  = hp_globals.filtered_function_names[i];
      uint8 hash = hp_inline_hash(str);
      int   idx  = INDEX_2_BYTE(hash);
      hp_globals.filtered_function_filter[idx] |= INDEX_2_BIT(hash);
    }
  }
}

/**
 * Check if function collides in filter of functions to be ignored.
 *
 * @author mpal
 */
int hp_filtered_functions_filter_collision(uint8 hash) {
  uint8 mask = INDEX_2_BIT(hash);
  return hp_globals.filtered_function_filter[INDEX_2_BYTE(hash)] & mask;
}

/**
 * Initialize profiler state
 *
 * @author kannan, veeve
 */
void hp_init_profiler_state(int level TSRMLS_DC) {
  /* Setup globals */
  if (!hp_globals.ever_enabled) {
    hp_globals.ever_enabled  = 1;
    hp_globals.entries = NULL;
  }
  hp_globals.profiler_level  = (int) level;

  /* Init stats_count */
  if (hp_globals.stats_count) {
    zval_dtor(hp_globals.stats_count);
    FREE_ZVAL(hp_globals.stats_count);
  }
  MAKE_STD_ZVAL(hp_globals.stats_count);
  array_init(hp_globals.stats_count);

  if (hp_globals.layers_count) {
      zval_dtor(hp_globals.layers_count);
      FREE_ZVAL(hp_globals.layers_count);
  }

  if (hp_globals.layers_definition) {
      MAKE_STD_ZVAL(hp_globals.layers_count);
      array_init(hp_globals.layers_count);
  }

  if (hp_globals.last_error) {
    zval_dtor(hp_globals.last_error);
    FREE_ZVAL(hp_globals.last_error);
  }
  MAKE_STD_ZVAL(hp_globals.last_error);
  array_init(hp_globals.last_error);

  if (hp_globals.last_exception_message) {
    zval_dtor(hp_globals.last_exception_message);
    FREE_ZVAL(hp_globals.last_exception_message);
  }

  if (hp_globals.last_exception_type) {
    zval_dtor(hp_globals.last_exception_type);
    FREE_ZVAL(hp_globals.last_exception_type);
  }

  /* NOTE(cjiang): some fields such as cpu_frequencies take relatively longer
   * to initialize, (5 milisecond per logical cpu right now), therefore we
   * calculate them lazily. */
  if (hp_globals.cpu_frequencies == NULL) {
    get_all_cpu_frequencies();
    restore_cpu_affinity(&hp_globals.prev_mask);
  }

  /* bind to a random cpu so that we can use rdtsc instruction. */
  bind_to_cpu((int) (rand() % hp_globals.cpu_num));

  /* Call current mode's init cb */
  hp_globals.mode_cb.init_cb(TSRMLS_C);

  /* Set up filter of functions which may be ignored during profiling */
  hp_filtered_functions_filter_init();
  hp_argument_functions_filter_init();
}

/**
 * Cleanup profiler state
 *
 * @author kannan, veeve
 */
void hp_clean_profiler_state(TSRMLS_D) {
  /* Call current mode's exit cb */
  hp_globals.mode_cb.exit_cb(TSRMLS_C);

  /* Clear globals */
  if (hp_globals.stats_count) {
    zval_dtor(hp_globals.stats_count);
    FREE_ZVAL(hp_globals.stats_count);
    hp_globals.stats_count = NULL;
  }

  if (hp_globals.layers_count) {
      zval_dtor(hp_globals.layers_count);
      FREE_ZVAL(hp_globals.layers_count);
  }

  if (hp_globals.last_error) {
    zval_dtor(hp_globals.last_error);
    FREE_ZVAL(hp_globals.last_error);
    hp_globals.last_error = NULL;
  }

  if (hp_globals.last_exception_message) {
    zval_ptr_dtor(&hp_globals.last_exception_message);
    hp_globals.last_exception_message = NULL;
  }

  if (hp_globals.last_exception_type) {
    FREE_ZVAL(hp_globals.last_exception_type);
    hp_globals.last_exception_type = NULL;
  }

  hp_globals.entries = NULL;
  hp_globals.profiler_level = 1;
  hp_globals.ever_enabled = 0;

  /* Delete the array storing ignored function names */
  hp_array_del(hp_globals.filtered_function_names);
  hp_globals.filtered_function_names = NULL;

  hp_array_del(hp_globals.argument_function_names);
  hp_globals.argument_function_names = NULL;

  if (hp_globals.layers_definition) {
      zend_hash_destroy(hp_globals.layers_definition);
      FREE_HASHTABLE(hp_globals.layers_definition);
  }
}

/*
 * Start profiling - called just before calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define BEGIN_PROFILING(entries, symbol, profile_curr)                  \
  do {                                                                  \
    /* Use a hash code to filter most of the string comparisons. */     \
    uint8 hash_code  = hp_inline_hash(symbol);                          \
    profile_curr = !hp_filter_entry(hash_code, symbol);                 \
    if (profile_curr) {                                                 \
      hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();              \
      (cur_entry)->hash_code = hash_code;                               \
      (cur_entry)->name_hprof = symbol;                                 \
      (cur_entry)->prev_hprof = (*(entries));                           \
      /* Call the universal callback */                                 \
      hp_mode_common_beginfn((entries), (cur_entry) TSRMLS_CC);         \
      /* Call the mode's beginfn callback */                            \
      hp_globals.mode_cb.begin_fn_cb((entries), (cur_entry) TSRMLS_CC); \
      /* Update entries linked list */                                  \
      (*(entries)) = (cur_entry);                                       \
    }                                                                   \
  } while (0)

/*
 * Stop profiling - called just after calling the actual function
 * NOTE:  PLEASE MAKE SURE TSRMLS_CC IS AVAILABLE IN THE CONTEXT
 *        OF THE FUNCTION WHERE THIS MACRO IS CALLED.
 *        TSRMLS_CC CAN BE MADE AVAILABLE VIA TSRMLS_DC IN THE
 *        CALLING FUNCTION OR BY CALLING TSRMLS_FETCH()
 *        TSRMLS_FETCH() IS RELATIVELY EXPENSIVE.
 */
#define END_PROFILING(entries, profile_curr)                            \
  do {                                                                  \
    if (profile_curr) {                                                 \
      hp_entry_t *cur_entry;                                            \
      /* Call the mode's endfn callback. */                             \
      /* NOTE(cjiang): we want to call this 'end_fn_cb' before */       \
      /* 'hp_mode_common_endfn' to avoid including the time in */       \
      /* 'hp_mode_common_endfn' in the profiling results.      */       \
      hp_globals.mode_cb.end_fn_cb((entries) TSRMLS_CC);                \
      cur_entry = (*(entries));                                         \
      /* Call the universal callback */                                 \
      hp_mode_common_endfn((entries), (cur_entry) TSRMLS_CC);           \
      /* Free top entry and update entries linked list */               \
      (*(entries)) = (*(entries))->prev_hprof;                          \
      hp_fast_free_hprof_entry(cur_entry);                              \
    }                                                                   \
  } while (0)


/**
 * Returns formatted function name
 *
 * @param  entry        hp_entry
 * @param  result_buf   ptr to result buf
 * @param  result_len   max size of result buf
 * @return total size of the function name returned in result_buf
 * @author veeve
 */
size_t hp_get_entry_name(hp_entry_t  *entry,
                         char           *result_buf,
                         size_t          result_len) {

  /* Validate result_len */
  if (result_len <= 1) {
    /* Insufficient result_bug. Bail! */
    return 0;
  }

  /* Add '@recurse_level' if required */
  /* NOTE:  Dont use snprintf's return val as it is compiler dependent */
  if (entry->rlvl_hprof) {
    snprintf(result_buf, result_len,
             "%s@%d",
             entry->name_hprof, entry->rlvl_hprof);
  }
  else {
    snprintf(result_buf, result_len,
             "%s",
             entry->name_hprof);
  }

  /* Force null-termination at MAX */
  result_buf[result_len - 1] = 0;

  return strlen(result_buf);
}

/**
 * Check if this entry should be filtered (positive or negative), first with a
 * conservative Bloomish filter then with an exact check against the function
 * names.
 *
 * @author mpal
 */
int  hp_filter_entry_work(uint8 hash_code, char *curr_func) {
  int filter = 0;
  if (hp_filtered_functions_filter_collision(hash_code)) {
      int i = 0;
      for (; hp_globals.filtered_function_names[i] != NULL; i++) {
          char *name = hp_globals.filtered_function_names[i];
          if (strcmp(curr_func, name) == 0) {
              filter++;
              break;
          }
      }
  }

  if (hp_globals.filtered_type == 2) {
      // always include main() in profiling result.
      if (strcmp(curr_func, ROOT_SYMBOL) == 0) {
          filter = 0;
      } else {
          filter = abs(filter - 1);
      }
  }

  return filter;
}

static inline int  hp_filter_entry(uint8 hash_code, char *curr_func) {
  /* First check if ignoring functions is enabled */
  return hp_globals.filtered_function_names != NULL &&
         hp_filter_entry_work(hash_code, curr_func);
}

/**
 * Build a caller qualified name for a callee.
 *
 * For example, if A() is caller for B(), then it returns "A==>B".
 * Recursive invokations are denoted with @<n> where n is the recursion
 * depth.
 *
 * For example, "foo==>foo@1", and "foo@2==>foo@3" are examples of direct
 * recursion. And  "bar==>foo@1" is an example of an indirect recursive
 * call to foo (implying the foo() is on the call stack some levels
 * above).
 *
 * @author kannan, veeve
 */
size_t hp_get_function_stack(hp_entry_t *entry,
                             int            level,
                             char          *result_buf,
                             size_t         result_len) {
  size_t         len = 0;

  /* End recursion if we dont need deeper levels or we dont have any deeper
   * levels */
  if (!entry->prev_hprof || (level <= 1)) {
    return hp_get_entry_name(entry, result_buf, result_len);
  }

  /* Take care of all ancestors first */
  len = hp_get_function_stack(entry->prev_hprof,
                              level - 1,
                              result_buf,
                              result_len);

  /* Append the delimiter */
# define    HP_STACK_DELIM        "==>"
# define    HP_STACK_DELIM_LEN    (sizeof(HP_STACK_DELIM) - 1)

  if (result_len < (len + HP_STACK_DELIM_LEN)) {
    /* Insufficient result_buf. Bail out! */
    return len;
  }

  /* Add delimiter only if entry had ancestors */
  if (len) {
    strncat(result_buf + len,
            HP_STACK_DELIM,
            result_len - len);
    len += HP_STACK_DELIM_LEN;
  }

# undef     HP_STACK_DELIM_LEN
# undef     HP_STACK_DELIM

  /* Append the current function name */
  return len + hp_get_entry_name(entry,
                                 result_buf + len,
                                 result_len - len);
}

/**
 * Takes an input of the form /a/b/c/d/foo.php and returns
 * a pointer to one-level directory and basefile name
 * (d/foo.php) in the same string.
 */
static const char *hp_get_base_filename(const char *filename) {
  const char *ptr;
  int   found = 0;

  if (!filename)
    return "";

  /* reverse search for "/" and return a ptr to the next char */
  for (ptr = filename + strlen(filename) - 1; ptr >= filename; ptr--) {
    if (*ptr == '/') {
      found++;
    }
    if (found == 2) {
      return ptr + 1;
    }
  }

  /* no "/" char found, so return the whole string */
  return filename;
}

static char *hp_get_sql_summary(char *sql, int len TSRMLS_DC) {
    zval *parts, **data;
    HashTable *arrayParts;
    pcre_cache_entry	*pce;			/* Compiled regular expression */
    HashPosition pointer;
    int array_count, result_len, found;
    char *result, *token;

    found = 0;
    result = "";
    MAKE_STD_ZVAL(parts);

    if ((pce = pcre_get_compiled_regex_cache("(([\\s]+))", 8 TSRMLS_CC)) == NULL) {
        return "";
    }

    php_pcre_split_impl(pce, sql, len, parts, -1, 0 TSRMLS_CC);

    arrayParts = Z_ARRVAL_P(parts);

    result_len = XHPROF_MAX_ARGUMENT_LEN;
    result = emalloc(result_len);

    for(zend_hash_internal_pointer_reset_ex(arrayParts, &pointer);
        zend_hash_get_current_data_ex(arrayParts, (void**) &data, &pointer) == SUCCESS;
        zend_hash_move_forward_ex(arrayParts, &pointer)) {

        char *key;
        int key_len;
        long index;

        zend_hash_get_current_key_ex(arrayParts, &key, &key_len, &index, 0, &pointer);

        token = Z_STRVAL_PP(data);
        php_strtolower(token, Z_STRLEN_PP(data));

        if ((strcmp(token, "insert") == 0 || strcmp(token, "delete") == 0) &&
            zend_hash_index_exists(arrayParts, index+2)) {
            snprintf(result, result_len, "%s", token);

            zend_hash_index_find(arrayParts, index+2, (void**) &data);
            snprintf(result, result_len, "%s %s", result, Z_STRVAL_PP(data));
            found = 1;

            break;
        } else if (strcmp(token, "update") == 0 && zend_hash_index_exists(arrayParts, index+1)) {
            snprintf(result, result_len, "%s", token);

            zend_hash_index_find(arrayParts, index+1, (void**) &data);
            snprintf(result, result_len, "%s %s", result, Z_STRVAL_PP(data));
            found = 1;

            break;
        } else if (strcmp(token, "select") == 0) {
            snprintf(result, result_len, "%s", token);
        } else if (strcmp(token, "from") == 0) {
            zend_hash_index_find(arrayParts, index+1, (void**) &data);
            snprintf(result, result_len, "%s %s", result, Z_STRVAL_PP(data));
            found = 1;

            break;
        }
    }

    zval_ptr_dtor(&parts);

    if (found == 0) {
        snprintf(result, result_len, "%s", "other");
    }

    return result;
}

static char *hp_get_file_summary(char *filename, int filename_len) {
    php_url *url;
    char *ret;
    int len;

    len = XHPROF_MAX_ARGUMENT_LEN;
    ret = emalloc(len);
    snprintf(ret, len, "");

    url = php_url_parse_ex(filename, filename_len);

    if (url->scheme) {
        snprintf(ret, len, "%s%s://", ret, url->scheme);
    }

    if (url->host) {
        snprintf(ret, len, "%s%s", ret, url->host);
    }

    if (url->port) {
        snprintf(ret, len, "%s%d", ret, url->port);
    }

    if (url->path) {
        snprintf(ret, len, "%s%s", ret, url->path);
    }

    /*
     * We assume the stream will be opened,
     * pointing to the next free element in resource list.
     *
     * This does not reliably work however, the first opened stream
     * (http,file) will open two entries, creating an offset. We can
     * handle this in the profiler parsing for now.
     */
    snprintf(ret, len, "%s#%d", ret, EG(regular_list).nNextFreeElement);

    php_url_free(url);

    return ret;
}

static char *hp_get_function_argument_summary(char *ret, int len, zend_execute_data *data TSRMLS_DC) {
    void **p;
    int arg_count = 0;
    int i;
    zval *argument_element;
    /* oldret holding function name or class::function. We will reuse the string and free it after */
    char *oldret = ret;
    char *summary;

    p = data->function_state.arguments;

#if PHP_VERSION_ID >= 50500
    /*
     * With PHP 5.5 zend_execute cannot be overwritten by extensions anymore.
     * instead zend_execute_ex has to be used. That however does not have
     * function_state.arguments populated for non-internal functions.
     * As per UPGRADING.INTERNALS we are accessing prev_execute_data which
     * has this information (for whatever reasons).
     */
    if (p == NULL) {
        p = (*data).prev_execute_data->function_state.arguments;
    }
#endif

    arg_count = (int)(zend_uintptr_t) *p;       /* this is the amount of arguments passed to function */

    len = XHPROF_MAX_ARGUMENT_LEN;
    ret = emalloc(len);
    snprintf(ret, len, "%s#", oldret);
    efree(oldret);

    if (strcmp(ret, "fgets#") == 0 ||
        strcmp(ret, "fgetcsv#") == 0 ||
        strcmp(ret, "fread#") == 0 ||
        strcmp(ret, "fwrite#") == 0 ||
        strcmp(ret, "fputs#") == 0 ||
        strcmp(ret, "fputcsv#") == 0 ||
        strcmp(ret, "stream_get_contents#") == 0 ||
        strcmp(ret, "fclose#") == 0
    ) {

        php_stream *stream;

        argument_element = *(p-arg_count);

        php_stream_from_zval_no_verify(stream, &argument_element);

        if (stream != NULL && stream->orig_path) {
            snprintf(ret, len, "%s%d", ret, stream->rsrc_id);
        }
    } else if (strcmp(ret, "fopen#") == 0 || strcmp(ret, "file_get_contents#") == 0 || strcmp(ret, "file_put_contents#") == 0) {
        argument_element = *(p-arg_count);

        if (argument_element->type == IS_STRING) {
            summary = hp_get_file_summary(Z_STRVAL_P(argument_element), Z_STRLEN_P(argument_element));

            snprintf(ret, len, "%s%s", ret, summary);

            efree(summary);
        }
    } else if (strcmp(ret, "curl_exec#") == 0) {
        php_curl *ch;
        int  le_curl;
        char *s_code;

        le_curl = zend_fetch_list_dtor_id("curl");

        argument_element = *(p-arg_count);

        ZEND_FETCH_RESOURCE_NO_RETURN(ch, php_curl *, &argument_element, -1, "cURL handle", le_curl);

        if (ch && curl_easy_getinfo(ch->cp, CURLINFO_EFFECTIVE_URL, &s_code) == CURLE_OK) {
            summary = hp_get_file_summary(s_code, strlen(s_code));
            snprintf(ret, len, "%s%s", ret, summary);
            efree(summary);
        }
    } else if (strcmp(ret, "PDO::exec#") == 0 ||
               strcmp(ret, "PDO::query#") == 0 ||
               strcmp(ret, "mysql_query#") == 0 ||
               strcmp(ret, "mysqli_query#") == 0 ||
               strcmp(ret, "mysqli::query#") == 0) {

        if (strcmp(ret, "mysqli_query#") == 0) {
            argument_element = *(p-arg_count+1);
        } else {
            argument_element = *(p-arg_count);
        }

        summary = hp_get_sql_summary(argument_element->value.str.val, argument_element->value.str.len TSRMLS_CC);

        snprintf(ret, len, "%s%s", ret, summary);
        efree(summary);

    } else if (strcmp(ret, "PDOStatement::execute#") == 0) {
        pdo_stmt_t *stmt = (pdo_stmt_t*)zend_object_store_get_object_by_handle( (((*((*data).object)).value).obj).handle TSRMLS_CC);

        summary = hp_get_sql_summary(stmt->query_string, stmt->query_stringlen TSRMLS_CC);

        snprintf(ret, len, "%s%s", ret, summary);

        efree(summary);
    } else if (strcmp(ret, "Twig_Template::render#") == 0 || strcmp(ret, "Twig_Template::display#") == 0) {
        zval fname, *retval_ptr;

        ZVAL_STRING(&fname, "getTemplateName", 0);

        if (SUCCESS == call_user_function_ex(EG(function_table), &((*((*data).prev_execute_data)).object), &fname, &retval_ptr, 0, NULL, 1, NULL TSRMLS_CC)) {
            snprintf(ret, len, "%s%s", ret, Z_STRVAL_P(retval_ptr));
        }

        FREE_ZVAL(retval_ptr);
    } else if (strcmp(ret, "Smarty::fetch#") == 0 || strcmp(ret, "Smarty_Internal_TemplateBase::fetch#") == 0) {
        argument_element = *(p-arg_count);

        if (argument_element->type == IS_STRING) {
            snprintf(ret, len, "%s%s", ret, Z_STRVAL_P(argument_element));
        }
    } else {
        for (i=0; i < arg_count; i++) {
          argument_element = *(p-(arg_count-i));
          switch(argument_element->type) {
            case IS_STRING:
              snprintf(ret, len, "%s%s", ret, argument_element->value.str.val);
              break;

            case IS_LONG:
            case IS_BOOL:
              snprintf(ret, len, "%s%ld", ret, argument_element->value.lval);
              break;

            case IS_DOUBLE:
              snprintf(ret, len, "%s%f", ret, argument_element->value.str.val);
              break;

            case IS_ARRAY:
              snprintf(ret, len, "%s%s", ret, "[...]");
              break;

            case IS_NULL:
              snprintf(ret, len, "%s%s", ret, "NULL");
              break;

            default:
              snprintf(ret, len, "%s%s", ret, "object");
          }

          if (i < arg_count-1) {
              snprintf(ret, len, "%s, ", ret);
          }
        }
    }

    return ret;
}

/**
 * Get the name of the current function. The name is qualified with
 * the class name if the function is in a class.
 *
 * @author kannan, hzhao
 */
static char *hp_get_function_name(zend_op_array *ops TSRMLS_DC) {
  zend_execute_data *data;
  const char        *func = NULL;
  const char        *cls = NULL;
  char              *ret = NULL;
  int                len;
  zend_function      *curr_func;
  uint8 hash_code;

  data = EG(current_execute_data);

  if (data) {
    /* shared meta data for function on the call stack */
    curr_func = data->function_state.function;

    /* extract function name from the meta info */
    func = curr_func->common.function_name;

    if (func) {
      /* previously, the order of the tests in the "if" below was
       * flipped, leading to incorrect function names in profiler
       * reports. When a method in a super-type is invoked the
       * profiler should qualify the function name with the super-type
       * class name (not the class name based on the run-time type
       * of the object.
       */
      if (curr_func->common.scope) {
        cls = curr_func->common.scope->name;
      } else if (data->object) {
        cls = Z_OBJCE(*data->object)->name;
      }

      if (cls) {
        len = strlen(cls) + strlen(func) + 10;
        ret = (char*)emalloc(len);
        snprintf(ret, len, "%s::%s", cls, func);
      } else {
        ret = estrdup(func);
      }

      hash_code  = hp_inline_hash(ret);

      if (hp_argument_entry(hash_code, ret)) {
        ret = hp_get_function_argument_summary(ret, len, data TSRMLS_CC);
      }

    } else {
      long     curr_op;
      int      add_filename = 1;

      /* we are dealing with a special directive/function like
       * include, eval, etc.
       */
#if ZEND_EXTENSION_API_NO >= 220100525
      curr_op = data->opline->extended_value;
#else
      curr_op = data->opline->op2.u.constant.value.lval;
#endif

      /* For some operations, we'll add the filename as part of the function
       * name to make the reports more useful. So rather than just "include"
       * you'll see something like "run_init::foo.php" in your reports.
       */
      if (curr_op == ZEND_EVAL){
          ret = estrdup(func);
      } else {
          const char *filename;
          int   len;
          filename = hp_get_base_filename((curr_func->op_array).filename);
          len      = strlen("run_init") + strlen(filename) + 3;
          ret      = (char *)emalloc(len);
          snprintf(ret, len, "run_init::%s", filename);
      }
    }
  }
  return ret;
}

/**
 * Free any items in the free list.
 */
static void hp_free_the_free_list() {
  hp_entry_t *p = hp_globals.entry_free_list;
  hp_entry_t *cur;

  while (p) {
    cur = p;
    p = p->prev_hprof;
    free(cur);
  }
}

/**
 * Fast allocate a hp_entry_t structure. Picks one from the
 * free list if available, else does an actual allocate.
 *
 * Doesn't bother initializing allocated memory.
 *
 * @author kannan
 */
static hp_entry_t *hp_fast_alloc_hprof_entry() {
  hp_entry_t *p;

  p = hp_globals.entry_free_list;

  if (p) {
    hp_globals.entry_free_list = p->prev_hprof;
    return p;
  } else {
    return (hp_entry_t *)malloc(sizeof(hp_entry_t));
  }
}

/**
 * Fast free a hp_entry_t structure. Simply returns back
 * the hp_entry_t to a free list and doesn't actually
 * perform the free.
 *
 * @author kannan
 */
static void hp_fast_free_hprof_entry(hp_entry_t *p) {

  /* we use/overload the prev_hprof field in the structure to link entries in
   * the free list. */
  p->prev_hprof = hp_globals.entry_free_list;
  hp_globals.entry_free_list = p;
}

/**
 * Increment the count of the given stat with the given count
 * If the stat was not set before, inits the stat to the given count
 *
 * @param  zval *counts   Zend hash table pointer
 * @param  char *name     Name of the stat
 * @param  long  count    Value of the stat to incr by
 * @return void
 * @author kannan
 */
void hp_inc_count(zval *counts, char *name, long count TSRMLS_DC) {
  HashTable *ht;
  void *data;

  if (!counts) return;
  ht = HASH_OF(counts);
  if (!ht) return;

  if (zend_hash_find(ht, name, strlen(name) + 1, &data) == SUCCESS) {
    ZVAL_LONG(*(zval**)data, Z_LVAL_PP((zval**)data) + count);
  } else {
    add_assoc_long(counts, name, count);
  }
}

/**
 * Looksup the hash table for the given symbol
 * Initializes a new array() if symbol is not present
 *
 * @author kannan, veeve
 */
zval * hp_hash_lookup(zval *hash, char *symbol  TSRMLS_DC) {
  HashTable   *ht;
  void        *data;
  zval        *counts = (zval *) 0;

  /* Bail if something is goofy */
  if (!hash || !(ht = HASH_OF(hash))) {
    return (zval *) 0;
  }

  /* Lookup our hash table */
  if (zend_hash_find(ht, symbol, strlen(symbol) + 1, &data) == SUCCESS) {
    /* Symbol already exists */
    counts = *(zval **) data;
  }
  else {
    /* Add symbol to hash table */
    MAKE_STD_ZVAL(counts);
    array_init(counts);
    add_assoc_zval(hash, symbol, counts);
  }

  return counts;
}

/**
 * Truncates the given timeval to the nearest slot begin, where
 * the slot size is determined by intr
 *
 * @param  tv       Input timeval to be truncated in place
 * @param  intr     Time interval in microsecs - slot width
 * @return void
 * @author veeve
 */
void hp_trunc_time(struct timeval *tv,
                   uint64          intr) {
  uint64 time_in_micro;

  /* Convert to microsecs and trunc that first */
  time_in_micro = (tv->tv_sec * 1000000) + tv->tv_usec;
  time_in_micro /= intr;
  time_in_micro *= intr;

  /* Update tv */
  tv->tv_sec  = (time_in_micro / 1000000);
  tv->tv_usec = (time_in_micro % 1000000);
}

/**
 * Sample the stack. Add it to the stats_count global.
 *
 * @param  tv            current time
 * @param  entries       func stack as linked list of hp_entry_t
 * @return void
 * @author veeve
 */
void hp_sample_stack(hp_entry_t  **entries  TSRMLS_DC) {
  char key[SCRATCH_BUF_LEN];
  char symbol[SCRATCH_BUF_LEN * 1000];

  /* Build key */
  snprintf(key, sizeof(key),
           "%d.%06d",
           hp_globals.last_sample_time.tv_sec,
           hp_globals.last_sample_time.tv_usec);

  /* Init stats in the global stats_count hashtable */
  hp_get_function_stack(*entries,
                        INT_MAX,
                        symbol,
                        sizeof(symbol));

  add_assoc_string(hp_globals.stats_count,
                   key,
                   symbol,
                   1);
  return;
}

/**
 * Checks to see if it is time to sample the stack.
 * Calls hp_sample_stack() if its time.
 *
 * @param  entries        func stack as linked list of hp_entry_t
 * @param  last_sample    time the last sample was taken
 * @param  sampling_intr  sampling interval in microsecs
 * @return void
 * @author veeve
 */
void hp_sample_check(hp_entry_t **entries  TSRMLS_DC) {
  /* Validate input */
  if (!entries || !(*entries)) {
    return;
  }

  /* See if its time to sample.  While loop is to handle a single function
   * taking a long time and passing several sampling intervals. */
  while ((cycle_timer() - hp_globals.last_sample_tsc)
         > hp_globals.sampling_interval_tsc) {

    /* bump last_sample_tsc */
    hp_globals.last_sample_tsc += hp_globals.sampling_interval_tsc;

    /* bump last_sample_time - HAS TO BE UPDATED BEFORE calling hp_sample_stack */
    incr_us_interval(&hp_globals.last_sample_time, XHPROF_SAMPLING_INTERVAL);

    /* sample the stack */
    hp_sample_stack(entries  TSRMLS_CC);
  }

  return;
}


/**
 * ***********************
 * High precision timer related functions.
 * ***********************
 */

/**
 * Get time stamp counter (TSC) value via 'rdtsc' instruction.
 *
 * @return 64 bit unsigned integer
 * @author cjiang
 */
static inline uint64 cycle_timer() {
  uint32 __a,__d;
  uint64 val;
  asm volatile("rdtsc" : "=a" (__a), "=d" (__d));
  (val) = ((uint64)__a) | (((uint64)__d)<<32);
  return val;
}

/**
 * Bind the current process to a specified CPU. This function is to ensure that
 * the OS won't schedule the process to different processors, which would make
 * values read by rdtsc unreliable.
 *
 * @param uint32 cpu_id, the id of the logical cpu to be bound to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int bind_to_cpu(uint32 cpu_id) {
  cpu_set_t new_mask;

  CPU_ZERO(&new_mask);
  CPU_SET(cpu_id, &new_mask);

  if (SET_AFFINITY(0, sizeof(cpu_set_t), &new_mask) < 0) {
    perror("setaffinity");
    return -1;
  }

  /* record the cpu_id the process is bound to. */
  hp_globals.cur_cpu_id = cpu_id;

  return 0;
}

/**
 * Get time delta in microseconds.
 */
static long get_us_interval(struct timeval *start, struct timeval *end) {
  return (((end->tv_sec - start->tv_sec) * 1000000)
          + (end->tv_usec - start->tv_usec));
}

/**
 * Incr time with the given microseconds.
 */
static void incr_us_interval(struct timeval *start, uint64 incr) {
  incr += (start->tv_sec * 1000000 + start->tv_usec);
  start->tv_sec  = incr/1000000;
  start->tv_usec = incr%1000000;
  return;
}

/**
 * Convert from TSC counter values to equivalent microseconds.
 *
 * @param uint64 count, TSC count value
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author cjiang
 */
static inline double get_us_from_tsc(uint64 count, double cpu_frequency) {
  return count / cpu_frequency;
}

/**
 * Convert microseconds to equivalent TSC counter ticks
 *
 * @param uint64 microseconds
 * @param double cpu_frequency, the CPU clock rate (MHz)
 * @return 64 bit unsigned integer
 *
 * @author veeve
 */
static inline uint64 get_tsc_from_us(uint64 usecs, double cpu_frequency) {
  return (uint64) (usecs * cpu_frequency);
}

/**
 * This is a microbenchmark to get cpu frequency the process is running on. The
 * returned value is used to convert TSC counter values to microseconds.
 *
 * @return double.
 * @author cjiang
 */
static double get_cpu_frequency() {
  struct timeval start;
  struct timeval end;

  if (gettimeofday(&start, 0)) {
    perror("gettimeofday");
    return 0.0;
  }
  uint64 tsc_start = cycle_timer();
  /* Sleep for 5 miliseconds. Comparaing with gettimeofday's  few microseconds
   * execution time, this should be enough. */
  usleep(5000);
  if (gettimeofday(&end, 0)) {
    perror("gettimeofday");
    return 0.0;
  }
  uint64 tsc_end = cycle_timer();
  return (tsc_end - tsc_start) * 1.0 / (get_us_interval(&start, &end));
}

/**
 * Calculate frequencies for all available cpus.
 *
 * @author cjiang
 */
static void get_all_cpu_frequencies() {
  int id;
  double frequency;

  hp_globals.cpu_frequencies = malloc(sizeof(double) * hp_globals.cpu_num);
  if (hp_globals.cpu_frequencies == NULL) {
    return;
  }

  /* Iterate over all cpus found on the machine. */
  for (id = 0; id < hp_globals.cpu_num; ++id) {
    /* Only get the previous cpu affinity mask for the first call. */
    if (bind_to_cpu(id)) {
      clear_frequencies();
      return;
    }

    /* Make sure the current process gets scheduled to the target cpu. This
     * might not be necessary though. */
    usleep(0);

    frequency = get_cpu_frequency();
    if (frequency == 0.0) {
      clear_frequencies();
      return;
    }
    hp_globals.cpu_frequencies[id] = frequency;
  }
}

/**
 * Restore cpu affinity mask to a specified value. It returns 0 on success and
 * -1 on failure.
 *
 * @param cpu_set_t * prev_mask, previous cpu affinity mask to be restored to.
 * @return int, 0 on success, and -1 on failure.
 *
 * @author cjiang
 */
int restore_cpu_affinity(cpu_set_t * prev_mask) {
  if (SET_AFFINITY(0, sizeof(cpu_set_t), prev_mask) < 0) {
    perror("restore setaffinity");
    return -1;
  }
  /* default value ofor cur_cpu_id is 0. */
  hp_globals.cur_cpu_id = 0;
  return 0;
}

/**
 * Reclaim the memory allocated for cpu_frequencies.
 *
 * @author cjiang
 */
static void clear_frequencies() {
  if (hp_globals.cpu_frequencies) {
    free(hp_globals.cpu_frequencies);
    hp_globals.cpu_frequencies = NULL;
  }
  restore_cpu_affinity(&hp_globals.prev_mask);
}


/**
 * ***************************
 * XHPROF DUMMY CALLBACKS
 * ***************************
 */
void hp_mode_dummy_init_cb(TSRMLS_D) { }


void hp_mode_dummy_exit_cb(TSRMLS_D) { }


void hp_mode_dummy_beginfn_cb(hp_entry_t **entries,
                              hp_entry_t *current  TSRMLS_DC) { }

void hp_mode_dummy_endfn_cb(hp_entry_t **entries   TSRMLS_DC) { }


/**
 * ****************************
 * XHPROF COMMON CALLBACKS
 * ****************************
 */
/**
 * XHPROF universal begin function.
 * This function is called for all modes before the
 * mode's specific begin_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack)
 *                                  of hprof entries
 * @param  hp_entry_t  *current  hprof entry for the current fn
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_beginfn(hp_entry_t **entries,
                            hp_entry_t  *current  TSRMLS_DC) {
  hp_entry_t   *p;

  /* This symbol's recursive level */
  int    recurse_level = 0;

  if (hp_globals.func_hash_counters[current->hash_code] > 0) {
    /* Find this symbols recurse level */
    for(p = (*entries); p; p = p->prev_hprof) {
      if (!strcmp(current->name_hprof, p->name_hprof)) {
        recurse_level = (p->rlvl_hprof) + 1;
        break;
      }
    }
  }
  hp_globals.func_hash_counters[current->hash_code]++;

  /* Init current function's recurse level */
  current->rlvl_hprof = recurse_level;
}

/**
 * XHPROF universal end function.  This function is called for all modes after
 * the mode's specific end_function callback is called.
 *
 * @param  hp_entry_t **entries  linked list (stack) of hprof entries
 * @return void
 * @author kannan, veeve
 */
void hp_mode_common_endfn(hp_entry_t **entries, hp_entry_t *current TSRMLS_DC) {
  hp_globals.func_hash_counters[current->hash_code]--;
}


/**
 * *********************************
 * XHPROF INIT MODULE CALLBACKS
 * *********************************
 */
/**
 * XHPROF_MODE_SAMPLED's init callback
 *
 * @author veeve
 */
void hp_mode_sampled_init_cb(TSRMLS_D) {
  struct timeval  now;
  uint64 truncated_us;
  uint64 truncated_tsc;
  double cpu_freq = hp_globals.cpu_frequencies[hp_globals.cur_cpu_id];

  /* Init the last_sample in tsc */
  hp_globals.last_sample_tsc = cycle_timer();

  /* Find the microseconds that need to be truncated */
  gettimeofday(&hp_globals.last_sample_time, 0);
  now = hp_globals.last_sample_time;
  hp_trunc_time(&hp_globals.last_sample_time, XHPROF_SAMPLING_INTERVAL);

  /* Subtract truncated time from last_sample_tsc */
  truncated_us  = get_us_interval(&hp_globals.last_sample_time, &now);
  truncated_tsc = get_tsc_from_us(truncated_us, cpu_freq);
  if (hp_globals.last_sample_tsc > truncated_tsc) {
    /* just to be safe while subtracting unsigned ints */
    hp_globals.last_sample_tsc -= truncated_tsc;
  }

  /* Convert sampling interval to ticks */
  hp_globals.sampling_interval_tsc =
    get_tsc_from_us(XHPROF_SAMPLING_INTERVAL, cpu_freq);
}


/**
 * ************************************
 * XHPROF BEGIN FUNCTION CALLBACKS
 * ************************************
 */

/**
 * XHPROF_MODE_HIERARCHICAL's begin function callback
 *
 * @author kannan
 */
void hp_mode_hier_beginfn_cb(hp_entry_t **entries,
                             hp_entry_t  *current  TSRMLS_DC) {
  /* Get start tsc counter */
  current->tsc_start = cycle_timer();

  /* Get CPU usage */
  if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
    getrusage(RUSAGE_SELF, &(current->ru_start_hprof));
  }

  /* Get memory usage */
  if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
    current->mu_start_hprof  = zend_memory_usage(0 TSRMLS_CC);
    current->pmu_start_hprof = zend_memory_peak_usage(0 TSRMLS_CC);
  }
}


/**
 * XHPROF_MODE_SAMPLED's begin function callback
 *
 * @author veeve
 */
void hp_mode_sampled_beginfn_cb(hp_entry_t **entries,
                                hp_entry_t  *current  TSRMLS_DC) {
  /* See if its time to take a sample */
  hp_sample_check(entries  TSRMLS_CC);
}


/**
 * **********************************
 * XHPROF END FUNCTION CALLBACKS
 * **********************************
 */

/**
 * XHPROF shared end function callback
 *
 * @author kannan
 */
zval * hp_mode_shared_endfn_cb(hp_entry_t *top,
                               char          *symbol  TSRMLS_DC) {
  zval    *counts;
  uint64   tsc_end;
  double   wt;

  /* Get end tsc counter */
  tsc_end = cycle_timer();

  /* Get the stat array */
  if (!(counts = hp_hash_lookup(hp_globals.stats_count, symbol TSRMLS_CC))) {
    return (zval *) 0;
  }

  wt = get_us_from_tsc(tsc_end - top->tsc_start, hp_globals.cpu_frequencies[hp_globals.cur_cpu_id]);

  /* Bump stats in the counts hashtable */
  hp_inc_count(counts, "ct", 1  TSRMLS_CC);
  hp_inc_count(counts, "wt", wt TSRMLS_CC);

  if (hp_globals.layers_definition) {
      void **data;
      zval *layer, *layer_counts;
      char function_name[SCRATCH_BUF_LEN];

      hp_get_function_stack(top, 1, function_name, sizeof(function_name));

      if (zend_hash_find(hp_globals.layers_definition, function_name, strlen(function_name)+1, (void**)&data) == SUCCESS) {
          layer = *data;

          if (layer_counts = hp_hash_lookup(hp_globals.layers_count, Z_STRVAL_P(layer) TSRMLS_CC)) {
              hp_inc_count(layer_counts, "ct", 1  TSRMLS_CC);
              hp_inc_count(layer_counts, "wt", wt TSRMLS_CC);
          }
      }
  }

  return counts;
}

/**
 * XHPROF_MODE_HIERARCHICAL's end function callback
 *
 * @author kannan
 */
void hp_mode_hier_endfn_cb(hp_entry_t **entries  TSRMLS_DC) {
  hp_entry_t   *top = (*entries);
  zval            *counts;
  struct rusage    ru_end;
  char             symbol[SCRATCH_BUF_LEN];
  long int         mu_end;
  long int         pmu_end;

  /* Get the stat array */
  hp_get_function_stack(top, 2, symbol, sizeof(symbol));
  if (!(counts = hp_mode_shared_endfn_cb(top,
                                         symbol  TSRMLS_CC))) {
    return;
  }

  if (hp_globals.xhprof_flags & XHPROF_FLAGS_CPU) {
    /* Get CPU usage */
    getrusage(RUSAGE_SELF, &ru_end);

    /* Bump CPU stats in the counts hashtable */
    hp_inc_count(counts, "cpu", (get_us_interval(&(top->ru_start_hprof.ru_utime),
                                              &(ru_end.ru_utime)) +
                              get_us_interval(&(top->ru_start_hprof.ru_stime),
                                              &(ru_end.ru_stime)))
              TSRMLS_CC);
  }

  if (hp_globals.xhprof_flags & XHPROF_FLAGS_MEMORY) {
    /* Get Memory usage */
    mu_end  = zend_memory_usage(0 TSRMLS_CC);
    pmu_end = zend_memory_peak_usage(0 TSRMLS_CC);

    /* Bump Memory stats in the counts hashtable */
    hp_inc_count(counts, "mu",  mu_end - top->mu_start_hprof    TSRMLS_CC);
    hp_inc_count(counts, "pmu", pmu_end - top->pmu_start_hprof  TSRMLS_CC);
  }
}

/**
 * XHPROF_MODE_SAMPLED's end function callback
 *
 * @author veeve
 */
void hp_mode_sampled_endfn_cb(hp_entry_t **entries  TSRMLS_DC) {
  /* See if its time to take a sample */
  hp_sample_check(entries  TSRMLS_CC);
}


/**
 * ***************************
 * PHP EXECUTE/COMPILE PROXIES
 * ***************************
 */

/**
 * XHProf enable replaced the zend_execute function with this
 * new execute function. We can do whatever profiling we need to
 * before and after calling the actual zend_execute().
 *
 * @author hzhao, kannan
 */
#if PHP_VERSION_ID < 50500
ZEND_DLEXPORT void hp_execute (zend_op_array *ops TSRMLS_DC) {
#else
ZEND_DLEXPORT void hp_execute_ex (zend_execute_data *execute_data TSRMLS_DC) {
  zend_op_array *ops = execute_data->op_array;
#endif
  char          *func = NULL;
  int hp_profile_flag = 1;

  func = hp_get_function_name(ops TSRMLS_CC);
  if (!func) {
#if PHP_VERSION_ID < 50500
    _zend_execute(ops TSRMLS_CC);
#else
    _zend_execute_ex(execute_data TSRMLS_CC);
#endif
    return;
  }

  BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
#if PHP_VERSION_ID < 50500
  _zend_execute(ops TSRMLS_CC);
#else
  _zend_execute_ex(execute_data TSRMLS_CC);
#endif
  if (hp_globals.entries) {
    END_PROFILING(&hp_globals.entries, hp_profile_flag);
  }
  efree(func);
}

#undef EX
#define EX(element) ((execute_data)->element)

/**
 * Very similar to hp_execute. Proxy for zend_execute_internal().
 * Applies to zend builtin functions.
 *
 * @author hzhao, kannan
 */

#if PHP_VERSION_ID < 50500
#define EX_T(offset) (*(temp_variable *)((char *) EX(Ts) + offset))

ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data,
                                       int ret TSRMLS_DC) {
#else
#define EX_T(offset) (*EX_TMP_VAR(execute_data, offset))

ZEND_DLEXPORT void hp_execute_internal(zend_execute_data *execute_data,
                                       struct _zend_fcall_info *fci, int ret TSRMLS_DC) {
#endif
  zend_execute_data *current_data;
  char             *func = NULL;
  int    hp_profile_flag = 1;

  current_data = EG(current_execute_data);
  func = hp_get_function_name(current_data->op_array TSRMLS_CC);

  if (func) {
    BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
  }

  if (!_zend_execute_internal) {
    /* no old override to begin with. so invoke the builtin's implementation  */
#if ZEND_EXTENSION_API_NO >= 220121212
    if (fci != NULL) {
      ((zend_internal_function *) execute_data->function_state.function)->handler(fci->param_count,
                       *fci->retval_ptr_ptr, fci->retval_ptr_ptr, fci->object_ptr, 1 TSRMLS_CC);
    } else {
      zval **return_value_ptr = &EX_TMP_VAR(execute_data, execute_data->opline->result.var)->var.ptr;
      ((zend_internal_function *) execute_data->function_state.function)->handler(execute_data->opline->extended_value, *return_value_ptr,
                       (execute_data->function_state.function->common.fn_flags & ZEND_ACC_RETURN_REFERENCE)?return_value_ptr:NULL,
                       execute_data->object, ret TSRMLS_CC);
    }
#elif ZEND_EXTENSION_API_NO >= 220100525
    zend_op *opline = EX(opline);
    temp_variable *retvar = &EX_T(opline->result.var);
    ((zend_internal_function *) EX(function_state).function)->handler(
                       opline->extended_value,
                       retvar->var.ptr,
                       (EX(function_state).function->common.fn_flags & ZEND_ACC_RETURN_REFERENCE) ?
                       &retvar->var.ptr:NULL,
                       EX(object), ret TSRMLS_CC);
#else
    zend_op *opline = EX(opline);
    ((zend_internal_function *) EX(function_state).function)->handler(
                       opline->extended_value,
                       EX_T(opline->result.u.var).var.ptr,
                       EX(function_state).function->common.return_reference ?
                       &EX_T(opline->result.u.var).var.ptr:NULL,
                       EX(object), ret TSRMLS_CC);
#endif
  } else {
    /* call the old override */
#if PHP_VERSION_ID < 50500
    _zend_execute_internal(execute_data, ret TSRMLS_CC);
#else
    _zend_execute_internal(execute_data, fci, ret TSRMLS_CC);
#endif
  }

  if (func) {
    if (hp_globals.entries) {
      END_PROFILING(&hp_globals.entries, hp_profile_flag);
    }
    efree(func);
  }

}

/**
 * Proxy for zend_compile_file(). Used to profile PHP compilation time.
 *
 * @author kannan, hzhao
 */
ZEND_DLEXPORT zend_op_array* hp_compile_file(zend_file_handle *file_handle,
                                             int type TSRMLS_DC) {

  const char     *filename;
  char           *func;
  int             len;
  zend_op_array  *ret;
  int             hp_profile_flag = 1;

  filename = hp_get_base_filename(file_handle->filename);
  len      = strlen("load") + strlen(filename) + 3;
  func      = (char *)emalloc(len);
  snprintf(func, len, "load::%s", filename);

  BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
  ret = _zend_compile_file(file_handle, type TSRMLS_CC);
  if (hp_globals.entries) {
    END_PROFILING(&hp_globals.entries, hp_profile_flag);
  }

  efree(func);
  return ret;
}

/**
 * Proxy for zend_compile_string(). Used to profile PHP eval compilation time.
 */
ZEND_DLEXPORT zend_op_array* hp_compile_string(zval *source_string, char *filename TSRMLS_DC) {

    char          *func;
    int            len;
    zend_op_array *ret;
    int            hp_profile_flag = 1;

    len  = strlen("eval") + strlen(filename) + 3;
    func = (char *)emalloc(len);
    snprintf(func, len, "eval::%s", filename);

    BEGIN_PROFILING(&hp_globals.entries, func, hp_profile_flag);
    ret = _zend_compile_string(source_string, filename TSRMLS_CC);
    if (hp_globals.entries) {
        END_PROFILING(&hp_globals.entries, hp_profile_flag);
    }

    efree(func);
    return ret;
}

/**
 * **************************
 * MAIN XHPROF CALLBACKS
 * **************************
 */

/**
 * This function gets called once when xhprof gets enabled.
 * It replaces all the functions like zend_execute, zend_execute_internal,
 * etc that needs to be instrumented with their corresponding proxies.
 */
static void hp_begin(long level, long xhprof_flags TSRMLS_DC) {
  if (!hp_globals.enabled) {
    int hp_profile_flag = 1;

    hp_globals.enabled      = 1;
    hp_globals.xhprof_flags = (uint32)xhprof_flags;

    /* Replace zend_compile with our proxy */
    _zend_compile_file = zend_compile_file;
    zend_compile_file  = hp_compile_file;

    /* Replace zend_compile_string with our proxy */
    _zend_compile_string = zend_compile_string;
    zend_compile_string = hp_compile_string;

    /* Replace zend_execute with our proxy */
#if PHP_VERSION_ID < 50500
    _zend_execute = zend_execute;
    zend_execute  = hp_execute;
#else
    _zend_execute_ex = zend_execute_ex;
    zend_execute_ex  = hp_execute_ex;
#endif

    xhprof_original_error_cb = zend_error_cb;
    zend_error_cb = xhprof_error_cb;

    zend_throw_exception_hook = xhprof_throw_exception_hook;

    /* Replace zend_execute_internal with our proxy */
    _zend_execute_internal = zend_execute_internal;
    if (!(hp_globals.xhprof_flags & XHPROF_FLAGS_NO_BUILTINS)) {
      /* if NO_BUILTINS is not set (i.e. user wants to profile builtins),
       * then we intercept internal (builtin) function calls.
       */
      zend_execute_internal = hp_execute_internal;
    }

    /* Initialize with the dummy mode first Having these dummy callbacks saves
     * us from checking if any of the callbacks are NULL everywhere. */
    hp_globals.mode_cb.init_cb     = hp_mode_dummy_init_cb;
    hp_globals.mode_cb.exit_cb     = hp_mode_dummy_exit_cb;
    hp_globals.mode_cb.begin_fn_cb = hp_mode_dummy_beginfn_cb;
    hp_globals.mode_cb.end_fn_cb   = hp_mode_dummy_endfn_cb;

    /* Register the appropriate callback functions Override just a subset of
     * all the callbacks is OK. */
    switch(level) {
      case XHPROF_MODE_HIERARCHICAL:
        hp_globals.mode_cb.begin_fn_cb = hp_mode_hier_beginfn_cb;
        hp_globals.mode_cb.end_fn_cb   = hp_mode_hier_endfn_cb;
        break;
      case XHPROF_MODE_SAMPLED:
        hp_globals.mode_cb.init_cb     = hp_mode_sampled_init_cb;
        hp_globals.mode_cb.begin_fn_cb = hp_mode_sampled_beginfn_cb;
        hp_globals.mode_cb.end_fn_cb   = hp_mode_sampled_endfn_cb;
        break;
    }

    /* one time initializations */
    hp_init_profiler_state(level TSRMLS_CC);

    /* start profiling from fictitious main() */
    BEGIN_PROFILING(&hp_globals.entries, ROOT_SYMBOL, hp_profile_flag);
  }
}

/**
 * Called at request shutdown time. Cleans the profiler's global state.
 */
static void hp_end(TSRMLS_D) {
  /* Bail if not ever enabled */
  if (!hp_globals.ever_enabled) {
    return;
  }

  /* Stop profiler if enabled */
  if (hp_globals.enabled) {
    hp_stop(TSRMLS_C);
  }

  /* Clean up state */
  hp_clean_profiler_state(TSRMLS_C);
}

/**
 * Called from xhprof_disable(). Removes all the proxies setup by
 * hp_begin() and restores the original values.
 */
static void hp_stop(TSRMLS_D) {
  int   hp_profile_flag = 1;

  /* End any unfinished calls */
  while (hp_globals.entries) {
    END_PROFILING(&hp_globals.entries, hp_profile_flag);
  }

  /* Remove proxies, restore the originals */
#if PHP_VERSION_ID < 50500
  zend_execute          = _zend_execute;
#else
  zend_execute_ex       = _zend_execute_ex;
#endif
  zend_execute_internal = _zend_execute_internal;
  zend_compile_file     = _zend_compile_file;
  zend_compile_string   = _zend_compile_string;

  zend_error_cb = xhprof_original_error_cb;
  zend_throw_exception_hook = NULL;

  /* Resore cpu affinity. */
  restore_cpu_affinity(&hp_globals.prev_mask);

  /* Stop profiling */
  hp_globals.enabled = 0;
}


/**
 * *****************************
 * XHPROF ZVAL UTILITY FUNCTIONS
 * *****************************
 */

/** Look in the PHP assoc array to find a key and return the zval associated
 *  with it.
 *
 *  @author mpal
 **/
static zval *hp_zval_at_key(char  *key,
                            zval  *values) {
  zval *result = NULL;

  if (values->type == IS_ARRAY) {
    HashTable *ht;
    zval     **value;
    uint       len = strlen(key) + 1;

    ht = Z_ARRVAL_P(values);
    if (zend_hash_find(ht, key, len, (void**)&value) == SUCCESS) {
      result = *value;
    }
  } else {
    result = NULL;
  }

  return result;
}

/** Convert the PHP array of strings to an emalloced array of strings. Note,
 *  this method duplicates the string data in the PHP array.
 *
 *  @author mpal
 **/
static char **hp_strings_in_zval(zval  *values) {
  char   **result;
  size_t   count;
  size_t   ix = 0;

  if (!values) {
    return NULL;
  }

  if (values->type == IS_ARRAY) {
    HashTable *ht;

    ht    = Z_ARRVAL_P(values);
    count = zend_hash_num_elements(ht);

    if((result =
         (char**)emalloc(sizeof(char*) * (count + 1))) == NULL) {
      return result;
    }

    for (zend_hash_internal_pointer_reset(ht);
         zend_hash_has_more_elements(ht) == SUCCESS;
         zend_hash_move_forward(ht)) {
      char  *str;
      uint   len;
      ulong  idx;
      int    type;
      zval **data;

      type = zend_hash_get_current_key_ex(ht, &str, &len, &idx, 0, NULL);
      /* Get the names stored in a standard array */
      if(type == HASH_KEY_IS_LONG) {
        if ((zend_hash_get_current_data(ht, (void**)&data) == SUCCESS) &&
            Z_TYPE_PP(data) == IS_STRING &&
            strcmp(Z_STRVAL_PP(data), ROOT_SYMBOL)) { /* do not ignore "main" */
          result[ix] = estrdup(Z_STRVAL_PP(data));
          ix++;
        }
      }
    }
  } else if(values->type == IS_STRING) {
    if((result = (char**)emalloc(sizeof(char*) * 2)) == NULL) {
      return result;
    }
    result[0] = estrdup(Z_STRVAL_P(values));
    ix = 1;
  } else {
    result = NULL;
  }

  /* NULL terminate the array */
  if (result != NULL) {
    result[ix] = NULL;
  }

  return result;
}

/* Free this memory at the end of profiling */
static inline void hp_array_del(char **name_array) {
  if (name_array != NULL) {
    int i = 0;
    for(; name_array[i] != NULL && i < XHPROF_MAX_FILTERED_FUNCTIONS; i++) {
      efree(name_array[i]);
    }
    efree(name_array);
  }
}

/* for simpler maintainance of the code just copied these from ignored_functions */


/**
 * Parse the list of ignored functions from the zval argument.
 *
 * @author mpal
 */
static void hp_get_argument_functions_from_arg(zval *args) {
  if (args != NULL) {
    zval  *zresult = NULL;

    zresult = hp_zval_at_key("argument_functions", args);
    hp_globals.argument_function_names = hp_strings_in_zval(zresult);
  } else {
    hp_globals.argument_function_names = NULL;
  }
}

/**
 * Clear filter for functions which may be ignored during profiling.
 *
 * @author mpal
 */
static void hp_argument_functions_filter_clear() {
  memset(hp_globals.argument_function_filter, 0,
         XHPROF_FILTERED_FUNCTION_SIZE);
}

/**
 * Initialize filter for ignored functions using bit vector.
 *
 * @author mpal
 */
static void hp_argument_functions_filter_init() {
  if (hp_globals.argument_function_names != NULL) {
    int i = 0;
    for(; hp_globals.argument_function_names[i] != NULL; i++) {
      char *str  = hp_globals.argument_function_names[i];
      uint8 hash = hp_inline_hash(str);
      int   idx  = INDEX_2_BYTE(hash);
      hp_globals.argument_function_filter[idx] |= INDEX_2_BIT(hash);
    }
  }
}

/**
 * Check if function collides in filter of functions to be ignored.
 *
 * @author mpal
 */
static int hp_argument_functions_filter_collision(uint8 hash) {
  uint8 mask = INDEX_2_BIT(hash);
  return hp_globals.argument_function_filter[INDEX_2_BYTE(hash)] & mask;
}

/**
 * Check if this entry should be ignored, first with a conservative Bloomish
 * filter then with an exact check against the function names.
 *
 * @author mpal
 */
static int  hp_argument_entry_work(uint8 hash_code, char *curr_func) {
  int ignore = 0;
  if (hp_argument_functions_filter_collision(hash_code)) {
    int i = 0;
    for (; hp_globals.argument_function_names[i] != NULL; i++) {
      char *name = hp_globals.argument_function_names[i];
      if ( !strcmp(curr_func, name)) {
        ignore++;
        break;
      }
    }
  }

  return ignore;
}

static inline int  hp_argument_entry(uint8 hash_code, char *curr_func) {
  /* First check if argument functions is enabled */
  return hp_globals.argument_function_names != NULL &&
         hp_argument_entry_work(hash_code, curr_func);
}

/* {{{ gettraceasstring() macros */
#define XHPROF_TRACE_APPEND_CHR(chr)                                            \
    *str = (char*)erealloc(*str, *len + 1 + 1);                          \
    (*str)[(*len)++] = chr

#define XHPROF_TRACE_APPEND_STRL(val, vallen)                                   \
    {                                                                    \
        int l = vallen;                                                  \
        *str = (char*)erealloc(*str, *len + l + 1);                      \
        memcpy((*str) + *len, val, l);                                   \
        *len += l;                                                       \
    }

#define XHPROF_TRACE_APPEND_STR(val)                                            \
    XHPROF_TRACE_APPEND_STRL(val, sizeof(val)-1)

#define XHPROF_TRACE_APPEND_KEY(key)                                                   \
    if (zend_hash_find(ht, key, sizeof(key), (void**)&tmp) == SUCCESS) {    \
        if (Z_TYPE_PP(tmp) != IS_STRING) {                              \
            zend_error(E_WARNING, "Value for %s is no string", key); \
            XHPROF_TRACE_APPEND_STR("[unknown]");                          \
        } else {                                                        \
            XHPROF_TRACE_APPEND_STRL(Z_STRVAL_PP(tmp), Z_STRLEN_PP(tmp));  \
        }                                                               \
    }


#define TRACE_ARG_APPEND(vallen)								\
    *str = (char*)erealloc(*str, *len + 1 + vallen);					\
    memmove((*str) + *len - l_added + 1 + vallen, (*str) + *len - l_added + 1, l_added);

/* }}} */

static int xhprof_build_trace_string(zval **frame TSRMLS_DC, int num_args, va_list args, zend_hash_key *hash_key) /* {{{ */
{
    char *s_tmp, **str;
    int *len, *num;
    long line;
    HashTable *ht = Z_ARRVAL_PP(frame);
    zval **file, **tmp;

    if (Z_TYPE_PP(frame) != IS_ARRAY) {
        zend_error(E_WARNING, "Expected array for frame %lu", hash_key->h);
        return ZEND_HASH_APPLY_KEEP;
    }

    str = va_arg(args, char**);
    len = va_arg(args, int*);
    num = va_arg(args, int*);

    s_tmp = emalloc(1 + MAX_LENGTH_OF_LONG + 1 + 1);
    sprintf(s_tmp, "#%d ", (*num)++);
    XHPROF_TRACE_APPEND_STRL(s_tmp, strlen(s_tmp));
    efree(s_tmp);
    if (zend_hash_find(ht, "file", sizeof("file"), (void**)&file) == SUCCESS) {
        if (Z_TYPE_PP(file) != IS_STRING) {
            zend_error(E_WARNING, "Function name is no string");
            XHPROF_TRACE_APPEND_STR("[unknown function]");
        } else{
            if (zend_hash_find(ht, "line", sizeof("line"), (void**)&tmp) == SUCCESS) {
                if (Z_TYPE_PP(tmp) == IS_LONG) {
                    line = Z_LVAL_PP(tmp);
                } else {
                    zend_error(E_WARNING, "Line is no long");
                    line = 0;
                }
            } else {
                line = 0;
            }
            s_tmp = emalloc(Z_STRLEN_PP(file) + MAX_LENGTH_OF_LONG + 4 + 1);
            sprintf(s_tmp, "%s(%ld): ", Z_STRVAL_PP(file), line);
            XHPROF_TRACE_APPEND_STRL(s_tmp, strlen(s_tmp));
            efree(s_tmp);
        }
    } else {
        XHPROF_TRACE_APPEND_STR("[internal function]: ");
    }
    XHPROF_TRACE_APPEND_KEY("class");
    XHPROF_TRACE_APPEND_KEY("type");
    XHPROF_TRACE_APPEND_KEY("function");
    XHPROF_TRACE_APPEND_STR("()\n");
    return ZEND_HASH_APPLY_KEEP;
}
/* }}} */

void xhprof_store_error(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
    va_list new_args;
    char *buffer;
    int buffer_len;
    zval *trace, *traceString;
    char *res, **str, *s_tmp;
    int res_len = 0, *len = &res_len, num = 0;

    TSRMLS_FETCH();

    res = estrdup("");
    str = &res;

    ALLOC_ZVAL(trace);
    Z_UNSET_ISREF_P(trace);
    Z_SET_REFCOUNT_P(trace, 0);

    zend_fetch_debug_backtrace(trace, 1, 0, 0 TSRMLS_CC);
    zend_hash_apply_with_arguments(Z_ARRVAL_P(trace) TSRMLS_CC, (apply_func_args_t)xhprof_build_trace_string, 3, str, len, &num);

    s_tmp = emalloc(1 + MAX_LENGTH_OF_LONG + 7 + 1);
    sprintf(s_tmp, "#%d {main}", num);
    XHPROF_TRACE_APPEND_STRL(s_tmp, strlen(s_tmp));
    efree(s_tmp);

    res[res_len] = '\0';

    ALLOC_ZVAL(traceString);
    Z_UNSET_ISREF_P(traceString);
    ZVAL_STRINGL(traceString, res, res_len, 0);

    /* We have to copy the arglist otherwise it will segfault in original error cb */
    va_copy(new_args, args);

    buffer_len = vspprintf(&buffer, PG(log_errors_max_len), format, new_args);

    add_assoc_long(hp_globals.last_error, "line", (int)error_lineno);
    add_assoc_string(hp_globals.last_error, "file", error_filename, 1);

    /* We need to see if we have an uncaught exception fatal error now */
    if (type == E_ERROR && strncmp(buffer, "Uncaught exception", 18) == 0 && hp_globals.last_exception_type && hp_globals.last_exception_message) {
        add_assoc_zval(hp_globals.last_error, "type", hp_globals.last_exception_type);
        add_assoc_zval(hp_globals.last_error, "message", hp_globals.last_exception_message);
        add_assoc_string(hp_globals.last_error, "trace", buffer, 1);
    } else {
        add_assoc_long(hp_globals.last_error, "type", type);
        add_assoc_string(hp_globals.last_error, "message", buffer, 1);
        add_assoc_zval(hp_globals.last_error, "trace", traceString);
    }

    efree(buffer);
}

void xhprof_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
    error_handling_t  error_handling;

#if (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3) || PHP_MAJOR_VERSION >= 6
    error_handling  = EG(error_handling);
#else
    error_handling  = PG(error_handling);
#endif

    if (error_handling == EH_NORMAL) {
        switch (type) {
            case E_ERROR:
            case E_CORE_ERROR:
            case E_USER_ERROR:
                xhprof_store_error(type, error_filename, error_lineno, format, args);
        }
    }

    xhprof_original_error_cb(type, error_filename, error_lineno, format, args);
}

static void xhprof_throw_exception_hook(zval *exception TSRMLS_DC)
{
    zend_class_entry *default_ce, *exception_ce;

    if (!exception) {
        return;
    }

    default_ce = zend_exception_get_default(TSRMLS_C);
    exception_ce = zend_get_class_entry(exception TSRMLS_CC);

    hp_globals.last_exception_message = zend_read_property(default_ce, exception, "message", sizeof("message")-1, 0 TSRMLS_CC);

    ALLOC_ZVAL(hp_globals.last_exception_type);
    Z_UNSET_ISREF_P(hp_globals.last_exception_type);
    ZVAL_STRING(hp_globals.last_exception_type, exception_ce->name, 0);
}
