/*
 * ec_jitter.c
 *
 * Generic IgH EtherCAT cyclic timing / jitter / DC benchmark
 *
 * Output:
 *   - Running progress only
 *   - Final summary table only
 *
 * Build:
 *
 *   gcc -O2 -Wall -Wextra \
 *       -I/usr/local/include \
 *       -L/usr/local/lib \
 *       -Wl,-rpath,/usr/local/lib \
 *       -o ec_jitter ec_jitter.c \
 *       -lethercat -lm
 *
 * Run:
 *
 *   sudo ./ec_jitter
 */

#define _GNU_SOURCE

#include <ecrt.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>
#include <limits.h>


/* ============================================================
 * Configuration
 * ============================================================ */

#define MASTER_INDEX        0
#define RT_PRIORITY         80

#define WARMUP_SECONDS      2
#define TEST_SECONDS        10

#define DC_MONITOR_HZ       100

#define ENABLE_DC_SYNC      1


static const unsigned int test_frequencies[] =
{
    100,
    250,
    500,
    1000,
    2000,
    4000,
    5000,
    10000
};


#define NUM_TEST_FREQS \
    (sizeof(test_frequencies) / sizeof(test_frequencies[0]))


#define MAX_TEST_HZ 10000

#define MAX_SAMPLES \
    ((size_t)MAX_TEST_HZ * TEST_SECONDS)


#define EPOCH_2000_OFFSET_SEC 946684800ULL


/* ============================================================
 * Result
 * ============================================================ */

typedef struct
{
    unsigned int hz;

    uint64_t period_ns;

    size_t samples;
    size_t dc_samples;

    double jitter_p99_us;
    double jitter_p999_us;
    double jitter_max_us;

    double period_p999_us;
    double period_max_abs_us;

    double exec_p999_us;
    double exec_max_us;

    double dc_p99_us;
    double dc_max_us;

    uint64_t overruns;

    uint64_t receive_errors;
    uint64_t send_errors;
    uint64_t dc_errors;

} test_result_t;


/* ============================================================
 * Globals
 * ============================================================ */

static volatile sig_atomic_t stop_requested = 0;

static ec_master_t *master = NULL;


static int64_t *jitter_samples = NULL;
static int64_t *period_error_samples = NULL;

static uint64_t *exec_samples = NULL;

static uint32_t *dc_samples = NULL;


static test_result_t results[NUM_TEST_FREQS];

static size_t result_count = 0;


/* ============================================================
 * Signal
 * ============================================================ */

static void signal_handler(int sig)
{
    (void)sig;

    stop_requested = 1;
}


/* ============================================================
 * Time
 * ============================================================ */

static inline int64_t timespec_diff_ns(
    const struct timespec *a,
    const struct timespec *b)
{
    return
        ((int64_t)a->tv_sec - (int64_t)b->tv_sec)
        * 1000000000LL
        +
        ((int64_t)a->tv_nsec - (int64_t)b->tv_nsec);
}


static inline void timespec_add_ns(
    struct timespec *ts,
    uint64_t ns)
{
    ts->tv_nsec += (long)ns;

    while (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_nsec -= 1000000000L;

        ts->tv_sec++;
    }
}


static uint64_t get_ethercat_application_time(void)
{
    struct timespec ts;

    clock_gettime(
        CLOCK_REALTIME,
        &ts);

    uint64_t sec =
        (uint64_t)ts.tv_sec;


    if (sec < EPOCH_2000_OFFSET_SEC)
        return 0;


    return
        (sec - EPOCH_2000_OFFSET_SEC)
        * 1000000000ULL
        +
        (uint64_t)ts.tv_nsec;
}


/* ============================================================
 * Sorting
 * ============================================================ */

static int cmp_i64(
    const void *a,
    const void *b)
{
    int64_t aa =
        *(const int64_t *)a;

    int64_t bb =
        *(const int64_t *)b;


    if (aa < bb)
        return -1;

    if (aa > bb)
        return 1;

    return 0;
}


static int cmp_u64(
    const void *a,
    const void *b)
{
    uint64_t aa =
        *(const uint64_t *)a;

    uint64_t bb =
        *(const uint64_t *)b;


    if (aa < bb)
        return -1;

    if (aa > bb)
        return 1;

    return 0;
}


static int cmp_u32(
    const void *a,
    const void *b)
{
    uint32_t aa =
        *(const uint32_t *)a;

    uint32_t bb =
        *(const uint32_t *)b;


    if (aa < bb)
        return -1;

    if (aa > bb)
        return 1;

    return 0;
}


/* ============================================================
 * Statistics
 * ============================================================ */

static size_t percentile_index(
    size_t count,
    double percentile)
{
    if (count == 0)
        return 0;


    double position =
        percentile
        *
        (double)(count - 1);


    size_t index =
        (size_t)ceil(position);


    if (index >= count)
        index = count - 1;


    return index;
}


static int64_t max_abs_i64(
    const int64_t *data,
    size_t count)
{
    int64_t max_value = 0;


    for (size_t i = 0; i < count; i++)
    {
        int64_t value = data[i];


        if (value == INT64_MIN)
        {
            value = INT64_MAX;
        }
        else if (value < 0)
        {
            value = -value;
        }


        if (value > max_value)
        {
            max_value = value;
        }
    }


    return max_value;
}


/* ============================================================
 * CPU affinity
 * ============================================================ */

static void configure_cpu_affinity(void)
{
    long cpu_count =
        sysconf(
            _SC_NPROCESSORS_ONLN);


    if (cpu_count <= 0)
        return;


    int cpu =
        (int)cpu_count - 1;


    cpu_set_t cpuset;

    CPU_ZERO(
        &cpuset);

    CPU_SET(
        cpu,
        &cpuset);


    sched_setaffinity(
        0,
        sizeof(cpuset),
        &cpuset);
}


/* ============================================================
 * Realtime scheduler
 * ============================================================ */

static void configure_realtime(void)
{
    struct sched_param sp;


    memset(
        &sp,
        0,
        sizeof(sp));


    sp.sched_priority =
        RT_PRIORITY;


    sched_setscheduler(
        0,
        SCHED_FIFO,
        &sp);
}


/* ============================================================
 * Frequency string
 * ============================================================ */

static void format_frequency(
    unsigned int hz,
    char *buffer,
    size_t size)
{
    if (
        hz >= 1000
        &&
        (hz % 1000) == 0)
    {
        snprintf(
            buffer,
            size,
            "%u kHz",
            hz / 1000);
    }
    else
    {
        snprintf(
            buffer,
            size,
            "%u Hz",
            hz);
    }
}


/* ============================================================
 * Test one frequency
 * ============================================================ */

static int run_frequency_test(
    unsigned int hz,
    test_result_t *result)
{
    memset(
        result,
        0,
        sizeof(*result));


    result->hz =
        hz;


    result->period_ns =
        1000000000ULL / hz;


    uint64_t period_ns =
        result->period_ns;


    size_t warmup_cycles =
        (size_t)hz
        *
        WARMUP_SECONDS;


    size_t measurement_cycles =
        (size_t)hz
        *
        TEST_SECONDS;


    size_t total_cycles =
        warmup_cycles
        +
        measurement_cycles;


    unsigned int monitor_div;


    if (hz <= DC_MONITOR_HZ)
    {
        monitor_div = 1;
    }
    else
    {
        monitor_div =
            hz / DC_MONITOR_HZ;


        if (monitor_div == 0)
            monitor_div = 1;
    }


    size_t sample_count = 0;

    size_t period_sample_count = 0;

    size_t dc_count = 0;


    uint64_t overruns = 0;

    uint64_t receive_errors = 0;

    uint64_t send_errors = 0;

    uint64_t dc_errors = 0;


    struct timespec target;

    struct timespec wake;

    struct timespec previous_wake;

    struct timespec exec_end;


    bool previous_valid = false;

    bool monitor_pending = false;


    clock_gettime(
        CLOCK_MONOTONIC,
        &target);


    /*
     * Start after 100 ms.
     */
    timespec_add_ns(
        &target,
        100000000ULL);


    for (
        size_t cycle = 0;
        cycle < total_cycles;
        cycle++)
    {
        if (stop_requested)
            return -1;


        int sleep_ret;


        do
        {
            sleep_ret =
                clock_nanosleep(
                    CLOCK_MONOTONIC,
                    TIMER_ABSTIME,
                    &target,
                    NULL);
        }
        while (
            sleep_ret == EINTR
            &&
            !stop_requested);


        if (stop_requested)
            return -1;


        if (sleep_ret != 0)
        {
            errno = sleep_ret;

            return -1;
        }


        clock_gettime(
            CLOCK_MONOTONIC,
            &wake);


        int64_t jitter_ns =
            timespec_diff_ns(
                &wake,
                &target);


        if (
            jitter_ns >=
            (int64_t)period_ns)
        {
            overruns++;
        }


        /*
         * EtherCAT receive
         */
        if (
            ecrt_master_receive(
                master) != 0)
        {
            receive_errors++;
        }


        /*
         * Previous DC monitor result
         */
        if (monitor_pending)
        {
            uint32_t dc =
                ecrt_master_sync_monitor_process(
                    master);


            if (dc == UINT32_MAX)
            {
                dc_errors++;
            }
            else
            {
                if (
                    cycle >= warmup_cycles
                    &&
                    dc_count < MAX_SAMPLES)
                {
                    dc_samples[
                        dc_count++]
                        =
                        dc;
                }
            }


            monitor_pending = false;
        }


#if ENABLE_DC_SYNC

        uint64_t app_time =
            get_ethercat_application_time();


        ecrt_master_application_time(
            master,
            app_time);


        ecrt_master_sync_reference_clock(
            master);


        ecrt_master_sync_slave_clocks(
            master);

#endif


        /*
         * DC monitor request
         */
        if (
            (cycle % monitor_div) == 0)
        {
            if (
                ecrt_master_sync_monitor_queue(
                    master) == 0)
            {
                monitor_pending = true;
            }
            else
            {
                dc_errors++;
            }
        }


        /*
         * EtherCAT send
         */
        if (
            ecrt_master_send(
                master) != 0)
        {
            send_errors++;
        }


        clock_gettime(
            CLOCK_MONOTONIC,
            &exec_end);


        uint64_t exec_ns =
            (uint64_t)
            timespec_diff_ns(
                &exec_end,
                &wake);


        /*
         * Measurement
         */
        if (
            cycle >=
            warmup_cycles)
        {
            if (
                sample_count
                <
                MAX_SAMPLES)
            {
                jitter_samples[
                    sample_count]
                    =
                    jitter_ns;


                exec_samples[
                    sample_count]
                    =
                    exec_ns;


                sample_count++;
            }


            if (
                previous_valid
                &&
                period_sample_count
                <
                MAX_SAMPLES)
            {
                int64_t actual_period_ns =
                    timespec_diff_ns(
                        &wake,
                        &previous_wake);


                period_error_samples[
                    period_sample_count]
                    =
                    actual_period_ns
                    -
                    (int64_t)period_ns;


                period_sample_count++;
            }
        }


        previous_wake =
            wake;


        previous_valid =
            true;


        timespec_add_ns(
            &target,
            period_ns);
    }


    /* ========================================================
     * Results
     * ======================================================== */

    result->samples =
        sample_count;


    result->dc_samples =
        dc_count;


    result->overruns =
        overruns;


    result->receive_errors =
        receive_errors;


    result->send_errors =
        send_errors;


    result->dc_errors =
        dc_errors;


    /*
     * Jitter
     */
    if (sample_count > 0)
    {
        qsort(
            jitter_samples,
            sample_count,
            sizeof(jitter_samples[0]),
            cmp_i64);


        result->jitter_p99_us =
            jitter_samples[
                percentile_index(
                    sample_count,
                    0.99)]
            /
            1000.0;


        result->jitter_p999_us =
            jitter_samples[
                percentile_index(
                    sample_count,
                    0.999)]
            /
            1000.0;


        result->jitter_max_us =
            jitter_samples[
                sample_count - 1]
            /
            1000.0;
    }


    /*
     * Period error
     */
    if (period_sample_count > 0)
    {
        result->period_max_abs_us =
            max_abs_i64(
                period_error_samples,
                period_sample_count)
            /
            1000.0;


        qsort(
            period_error_samples,
            period_sample_count,
            sizeof(period_error_samples[0]),
            cmp_i64);


        result->period_p999_us =
            period_error_samples[
                percentile_index(
                    period_sample_count,
                    0.999)]
            /
            1000.0;
    }


    /*
     * EtherCAT execution
     */
    if (sample_count > 0)
    {
        qsort(
            exec_samples,
            sample_count,
            sizeof(exec_samples[0]),
            cmp_u64);


        result->exec_p999_us =
            exec_samples[
                percentile_index(
                    sample_count,
                    0.999)]
            /
            1000.0;


        result->exec_max_us =
            exec_samples[
                sample_count - 1]
            /
            1000.0;
    }


    /*
     * DC
     */
    if (dc_count > 0)
    {
        qsort(
            dc_samples,
            dc_count,
            sizeof(dc_samples[0]),
            cmp_u32);


        result->dc_p99_us =
            dc_samples[
                percentile_index(
                    dc_count,
                    0.99)]
            /
            1000.0;


        result->dc_max_us =
            dc_samples[
                dc_count - 1]
            /
            1000.0;
    }


    return 0;
}


/* ============================================================
 * Summary table
 * ============================================================ */

static void print_summary_table(void)
{
    printf("\n");

    printf(
        "============================================================================================================\n");

    printf(
        " EtherCAT Timing / DC Benchmark Result\n");

    printf(
        "============================================================================================================\n");


    printf(
        "%-9s "
        "%10s "
        "%10s "
        "%10s "
        "%11s "
        "%11s "
        "%10s "
        "%9s "
        "%8s\n",

        "Rate",
        "Period",
        "P99.9 Jit",
        "Max Jit",
        "Max PerErr",
        "Max Exec",
        "DC Max",
        "Overrun",
        "Errors");


    printf(
        "%-9s "
        "%10s "
        "%10s "
        "%10s "
        "%11s "
        "%11s "
        "%10s "
        "%9s "
        "%8s\n",

        "",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "",
        "");


    printf(
        "------------------------------------------------------------------------------------------------------------\n");


    for (
        size_t i = 0;
        i < result_count;
        i++)
    {
        test_result_t *r =
            &results[i];


        char freq[32];


        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        uint64_t errors =
            r->receive_errors
            +
            r->send_errors
            +
            r->dc_errors;


        printf(
            "%-9s "
            "%10.1f "
            "%10.2f "
            "%10.2f "
            "%11.2f "
            "%11.2f ",

            freq,

            r->period_ns /
            1000.0,

            r->jitter_p999_us,

            r->jitter_max_us,

            r->period_max_abs_us,

            r->exec_max_us);


        if (r->dc_samples > 0)
        {
            printf(
                "%10.2f ",
                r->dc_max_us);
        }
        else
        {
            printf(
                "%10s ",
                "-");
        }


        printf(
            "%9" PRIu64 " "
            "%8" PRIu64 "\n",

            r->overruns,

            errors);
    }


    printf(
        "============================================================================================================\n");


    printf(
        "\n"
        "Jit     = Linux wake-up delay\n"
        "PerErr  = Actual cycle period error\n"
        "Exec    = EtherCAT receive/DC/send execution time\n"
        "DC Max  = Maximum Distributed Clock synchronization difference\n"
        "Errors  = RX + TX + DC monitor errors\n");
}


/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    signal(
        SIGINT,
        signal_handler);


    signal(
        SIGTERM,
        signal_handler);


    /*
     * Allocate buffers.
     */
    jitter_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*jitter_samples));


    period_error_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*period_error_samples));


    exec_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*exec_samples));


    dc_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*dc_samples));


    if (
        !jitter_samples
        ||
        !period_error_samples
        ||
        !exec_samples
        ||
        !dc_samples)
    {
        fprintf(
            stderr,
            "Memory allocation failed.\n");

        return EXIT_FAILURE;
    }


    /*
     * Lock memory.
     */
    if (
        mlockall(
            MCL_CURRENT
            |
            MCL_FUTURE) != 0)
    {
        perror(
            "mlockall");
    }


    /*
     * CPU affinity.
     */
    configure_cpu_affinity();


    /*
     * EtherCAT master.
     */
    master =
        ecrt_request_master(
            MASTER_INDEX);


    if (!master)
    {
        fprintf(
            stderr,
            "Cannot request EtherCAT master.\n");

        return EXIT_FAILURE;
    }


    /*
     * Automatic DC reference selection.
     */
    ecrt_master_select_reference_clock(
        master,
        NULL);


    /*
     * Activate master.
     */
    if (
        ecrt_master_activate(
            master) != 0)
    {
        fprintf(
            stderr,
            "Cannot activate EtherCAT master.\n");


        ecrt_release_master(
            master);


        return EXIT_FAILURE;
    }


    /*
     * Initial settle time.
     */
    usleep(
        500000);


    /*
     * Realtime scheduler.
     */
    configure_realtime();


    printf(
        "Running EtherCAT timing benchmark...\n\n");


    /*
     * Run all frequencies.
     */
    for (
        size_t i = 0;
        i < NUM_TEST_FREQS;
        i++)
    {
        if (stop_requested)
            break;


        memset(
            jitter_samples,
            0,
            MAX_SAMPLES
            *
            sizeof(*jitter_samples));


        memset(
            period_error_samples,
            0,
            MAX_SAMPLES
            *
            sizeof(*period_error_samples));


        memset(
            exec_samples,
            0,
            MAX_SAMPLES
            *
            sizeof(*exec_samples));


        memset(
            dc_samples,
            0,
            MAX_SAMPLES
            *
            sizeof(*dc_samples));


        char freq[32];


        format_frequency(
            test_frequencies[i],
            freq,
            sizeof(freq));


        printf(
            "[%zu/%zu] %-8s ... ",
            i + 1,
            (size_t)NUM_TEST_FREQS,
            freq);


        fflush(
            stdout);


        if (
            run_frequency_test(
                test_frequencies[i],
                &results[result_count])
            != 0)
        {
            printf(
                "STOP\n");

            break;
        }


        result_count++;


        printf(
            "done\n");
    }


    /*
     * Return scheduler to normal.
     */
    {
        struct sched_param sp;


        memset(
            &sp,
            0,
            sizeof(sp));


        sched_setscheduler(
            0,
            SCHED_OTHER,
            &sp);
    }


    /*
     * Final table.
     */
    if (result_count > 0)
    {
        print_summary_table();
    }


    if (stop_requested)
    {
        printf(
            "\nStopped by user.\n");
    }


    /*
     * Cleanup EtherCAT.
     */
    if (master)
    {
        ecrt_master_deactivate(
            master);


        ecrt_release_master(
            master);


        master = NULL;
    }


    munlockall();


    free(
        jitter_samples);


    free(
        period_error_samples);


    free(
        exec_samples);


    free(
        dc_samples);


    return EXIT_SUCCESS;
}