/*
 * ec_jitter.c
 *
 * Generic IgH EtherCAT cyclic timing / jitter / DC benchmark
 *
 * - No PDO dependency
 * - No Vendor ID dependency
 * - No Product Code dependency
 * - No CSV
 * - Fixed test frequencies
 * - Final comparison table
 *
 * Tested concept:
 *      Linux realtime wake-up timing
 *          +
 *      IgH EtherCAT send/receive execution timing
 *          +
 *      EtherCAT DC synchronization difference
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

/*
 * DC monitor frequency.
 *
 * The benchmark itself can operate at 10 kHz, but there is
 * little reason to insert the DC monitor datagram at 10 kHz.
 *
 * About 100 measurements / second is sufficient for this
 * diagnostic tool.
 */
#define DC_MONITOR_HZ       100

#define ENABLE_DC_SYNC      1


/*
 * Fixed frequencies.
 *
 * No command line options are required.
 */
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


#define MAX_TEST_HZ         10000

#define MAX_SAMPLES \
    ((size_t)MAX_TEST_HZ * TEST_SECONDS)


/*
 * EtherCAT epoch:
 *
 * EtherCAT application time:
 *     2000-01-01 00:00:00
 *
 * Linux CLOCK_REALTIME:
 *     1970-01-01 00:00:00
 */
#define EPOCH_2000_OFFSET_SEC 946684800ULL


/* ============================================================
 * Result structure
 * ============================================================ */

typedef struct
{
    unsigned int hz;

    uint64_t period_ns;

    size_t samples;
    size_t dc_samples;

    double jitter_mean_us;
    double jitter_p99_us;
    double jitter_p999_us;
    double jitter_max_us;

    double period_p999_us;
    double period_max_abs_us;

    double exec_mean_us;
    double exec_p999_us;
    double exec_max_us;

    double dc_mean_us;
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
 * Time functions
 * ============================================================ */

static inline int64_t timespec_diff_ns(
    const struct timespec *a,
    const struct timespec *b)
{
    return

        ((int64_t)a->tv_sec -
         (int64_t)b->tv_sec)
        * 1000000000LL

        +

        ((int64_t)a->tv_nsec -
         (int64_t)b->tv_nsec);
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


/*
 * Convert CLOCK_REALTIME to EtherCAT application time.
 *
 * Unit:
 *     nanoseconds since 2000-01-01.
 */
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
    const int64_t aa =
        *(const int64_t *)a;

    const int64_t bb =
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
    const uint64_t aa =
        *(const uint64_t *)a;

    const uint64_t bb =
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
    const uint32_t aa =
        *(const uint32_t *)a;

    const uint32_t bb =
        *(const uint32_t *)b;

    if (aa < bb)
        return -1;

    if (aa > bb)
        return 1;

    return 0;
}


static size_t percentile_index(
    size_t count,
    double percentile)
{
    if (count == 0)
        return 0;

    double pos =
        percentile *
        (double)(count - 1);

    size_t index =
        (size_t)ceil(pos);

    if (index >= count)
        index = count - 1;

    return index;
}


/* ============================================================
 * Statistics helpers
 * ============================================================ */

static double mean_i64_us(
    const int64_t *data,
    size_t count)
{
    if (count == 0)
        return 0.0;

    long double sum = 0.0L;

    for (size_t i = 0; i < count; i++)
    {
        sum +=
            (long double)data[i];
    }

    return

        (double)
        (
            sum /
            (long double)count /
            1000.0L
        );
}


static double mean_u64_us(
    const uint64_t *data,
    size_t count)
{
    if (count == 0)
        return 0.0;

    long double sum = 0.0L;

    for (size_t i = 0; i < count; i++)
    {
        sum +=
            (long double)data[i];
    }

    return

        (double)
        (
            sum /
            (long double)count /
            1000.0L
        );
}


static double mean_u32_us(
    const uint32_t *data,
    size_t count)
{
    if (count == 0)
        return 0.0;

    long double sum = 0.0L;

    for (size_t i = 0; i < count; i++)
    {
        sum +=
            (long double)data[i];
    }

    return

        (double)
        (
            sum /
            (long double)count /
            1000.0L
        );
}


static double stddev_i64_us(
    const int64_t *data,
    size_t count)
{
    if (count == 0)
        return 0.0;

    long double sum = 0.0L;
    long double sq_sum = 0.0L;

    for (size_t i = 0; i < count; i++)
    {
        long double x =
            (long double)data[i];

        sum += x;

        sq_sum += x * x;
    }

    long double mean =
        sum / (long double)count;

    long double variance =

        sq_sum /
        (long double)count

        -

        mean * mean;

    if (variance < 0.0L)
        variance = 0.0L;

    return

        (double)
        (
            sqrtl(variance) /
            1000.0L
        );
}


/*
 * Maximum absolute period error.
 */
static int64_t max_abs_i64(
    const int64_t *data,
    size_t count)
{
    int64_t max_value = 0;

    for (size_t i = 0; i < count; i++)
    {
        int64_t x = data[i];

        if (x == INT64_MIN)
            x = INT64_MAX;
        else if (x < 0)
            x = -x;

        if (x > max_value)
            max_value = x;
    }

    return max_value;
}


/* ============================================================
 * Detailed statistics printing
 * ============================================================ */

static void print_i64_stats(
    const char *name,
    int64_t *data,
    size_t count)
{
    printf("\n%-30s\n", name);
    printf("----------------------------------------------\n");

    if (count == 0)
    {
        printf("No samples\n");
        return;
    }

    double mean =
        mean_i64_us(data, count);

    double stddev =
        stddev_i64_us(data, count);

    qsort(
        data,
        count,
        sizeof(data[0]),
        cmp_i64);

    size_t p95 =
        percentile_index(
            count,
            0.95);

    size_t p99 =
        percentile_index(
            count,
            0.99);

    size_t p999 =
        percentile_index(
            count,
            0.999);

    printf(
        "Mean                 : %10.3f us\n",
        mean);

    printf(
        "StdDev               : %10.3f us\n",
        stddev);

    printf(
        "Min                  : %10.3f us\n",
        data[0] / 1000.0);

    printf(
        "P95                  : %10.3f us\n",
        data[p95] / 1000.0);

    printf(
        "P99                  : %10.3f us\n",
        data[p99] / 1000.0);

    printf(
        "P99.9                : %10.3f us\n",
        data[p999] / 1000.0);

    printf(
        "Max                  : %10.3f us\n",
        data[count - 1] / 1000.0);
}


static void print_u64_stats(
    const char *name,
    uint64_t *data,
    size_t count)
{
    printf("\n%-30s\n", name);
    printf("----------------------------------------------\n");

    if (count == 0)
    {
        printf("No samples\n");
        return;
    }

    double mean =
        mean_u64_us(
            data,
            count);

    qsort(
        data,
        count,
        sizeof(data[0]),
        cmp_u64);

    size_t p95 =
        percentile_index(
            count,
            0.95);

    size_t p99 =
        percentile_index(
            count,
            0.99);

    size_t p999 =
        percentile_index(
            count,
            0.999);

    printf(
        "Mean                 : %10.3f us\n",
        mean);

    printf(
        "Min                  : %10.3f us\n",
        data[0] / 1000.0);

    printf(
        "P95                  : %10.3f us\n",
        data[p95] / 1000.0);

    printf(
        "P99                  : %10.3f us\n",
        data[p99] / 1000.0);

    printf(
        "P99.9                : %10.3f us\n",
        data[p999] / 1000.0);

    printf(
        "Max                  : %10.3f us\n",
        data[count - 1] / 1000.0);
}


static void print_dc_stats(
    uint32_t *data,
    size_t count)
{
    printf(
        "\nDC synchronization difference\n");

    printf(
        "----------------------------------------------\n");

    if (count == 0)
    {
        printf(
            "No valid DC samples.\n");

        printf(
            "The connected slave may not support DC.\n");

        return;
    }

    double mean =
        mean_u32_us(
            data,
            count);

    qsort(
        data,
        count,
        sizeof(data[0]),
        cmp_u32);

    size_t p95 =
        percentile_index(
            count,
            0.95);

    size_t p99 =
        percentile_index(
            count,
            0.99);

    size_t p999 =
        percentile_index(
            count,
            0.999);

    printf(
        "Samples              : %10zu\n",
        count);

    printf(
        "Mean                 : %10.3f us\n",
        mean);

    printf(
        "Min                  : %10.3f us\n",
        data[0] / 1000.0);

    printf(
        "P95                  : %10.3f us\n",
        data[p95] / 1000.0);

    printf(
        "P99                  : %10.3f us\n",
        data[p99] / 1000.0);

    printf(
        "P99.9                : %10.3f us\n",
        data[p999] / 1000.0);

    printf(
        "Max                  : %10.3f us\n",
        data[count - 1] / 1000.0);
}


/* ============================================================
 * CPU / realtime setup
 * ============================================================ */

static int configure_cpu_affinity(void)
{
    long cpu_count =
        sysconf(
            _SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0)
    {
        printf(
            "WARNING: Cannot detect CPU count.\n");

        return -1;
    }

    /*
     * Automatically select the last online CPU.
     *
     * Raspberry Pi 4/5:
     *     normally CPU 3
     */
    int cpu =
        (int)cpu_count - 1;

    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);

    CPU_SET(
        cpu,
        &cpuset);

    if (sched_setaffinity(
            0,
            sizeof(cpuset),
            &cpuset) != 0)
    {
        perror(
            "sched_setaffinity");

        return -1;
    }

    printf(
        "CPU affinity           : CPU %d\n",
        cpu);

    return cpu;
}


static int configure_realtime(void)
{
    struct sched_param sp;

    memset(
        &sp,
        0,
        sizeof(sp));

    sp.sched_priority =
        RT_PRIORITY;

    if (sched_setscheduler(
            0,
            SCHED_FIFO,
            &sp) != 0)
    {
        perror(
            "sched_setscheduler");

        printf(
            "WARNING: SCHED_FIFO unavailable.\n");

        return -1;
    }

    printf(
        "Scheduler              : SCHED_FIFO\n");

    printf(
        "RT priority            : %d\n",
        RT_PRIORITY);

    return 0;
}


/* ============================================================
 * EtherCAT status
 * ============================================================ */

static void print_master_state(void)
{
    ec_master_state_t state;

    memset(
        &state,
        0,
        sizeof(state));

    if (ecrt_master_state(
            master,
            &state) != 0)
    {
        printf(
            "Unable to read master state.\n");

        return;
    }

    printf(
        "\nEtherCAT master state\n");

    printf(
        "----------------------------------------------\n");

    printf(
        "Slaves responding      : %u\n",
        state.slaves_responding);

    printf(
        "AL state               : 0x%02X\n",
        state.al_states);

    printf(
        "Link                   : %s\n",
        state.link_up
            ? "UP"
            : "DOWN");
}


/* ============================================================
 * Frequency label
 * ============================================================ */

static void format_frequency(
    unsigned int hz,
    char *buffer,
    size_t size)
{
    if (hz >= 1000 &&
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
 * Single test
 * ============================================================ */

static int run_frequency_test(
    unsigned int hz,
    test_result_t *result)
{
    memset(
        result,
        0,
        sizeof(*result));

    result->hz = hz;

    result->period_ns =
        1000000000ULL / hz;


    const uint64_t period_ns =
        result->period_ns;


    const size_t warmup_cycles =
        (size_t)hz *
        WARMUP_SECONDS;


    const size_t test_cycles =
        (size_t)hz *
        TEST_SECONDS;


    const size_t total_cycles =
        warmup_cycles +
        test_cycles;


    unsigned int monitor_div;

    if (hz <= DC_MONITOR_HZ)
    {
        monitor_div = 1;
    }
    else
    {
        monitor_div =
            hz /
            DC_MONITOR_HZ;

        if (monitor_div == 0)
            monitor_div = 1;
    }


    char freq_text[32];

    format_frequency(
        hz,
        freq_text,
        sizeof(freq_text));


    printf(
        "\n"
        "============================================================\n");

    printf(
        " Test: %-10s  Period: %.3f us\n",
        freq_text,
        period_ns / 1000.0);

    printf(
        " Warmup: %d sec       Measurement: %d sec\n",
        WARMUP_SECONDS,
        TEST_SECONDS);

    printf(
        "============================================================\n");


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


    /*
     * Absolute scheduler start time.
     */
    clock_gettime(
        CLOCK_MONOTONIC,
        &target);


    /*
     * Start 100 ms in the future.
     */
    timespec_add_ns(
        &target,
        100000000ULL);


    for (size_t cycle = 0;
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
            sleep_ret == EINTR &&
            !stop_requested);


        if (stop_requested)
            return -1;


        if (sleep_ret != 0)
        {
            errno = sleep_ret;

            perror(
                "clock_nanosleep");

            return -1;
        }


        clock_gettime(
            CLOCK_MONOTONIC,
            &wake);


        /*
         * Scheduler wake-up lateness.
         *
         * Positive:
         *     Linux woke later than requested.
         */
        int64_t jitter_ns =
            timespec_diff_ns(
                &wake,
                &target);


        if (jitter_ns >=
            (int64_t)period_ns)
        {
            overruns++;
        }


        /*
         * EtherCAT RX
         */
        if (ecrt_master_receive(
                master) != 0)
        {
            receive_errors++;
        }


        /*
         * Process previous DC monitor request.
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
                if (cycle >= warmup_cycles &&
                    dc_count < MAX_SAMPLES)
                {
                    dc_samples[
                        dc_count++] = dc;
                }
            }


            monitor_pending = false;
        }


#if ENABLE_DC_SYNC

        /*
         * EtherCAT application time.
         */
        uint64_t app_time =
            get_ethercat_application_time();


        ecrt_master_application_time(
            master,
            app_time);


        /*
         * Reference -> application clock synchronization.
         */
        ecrt_master_sync_reference_clock(
            master);


        /*
         * Slave DC clocks -> reference clock.
         */
        ecrt_master_sync_slave_clocks(
            master);

#endif


        /*
         * DC monitor request.
         */
        if ((cycle % monitor_div) == 0)
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
         * EtherCAT TX
         */
        if (ecrt_master_send(
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
         * Measurement section.
         */
        if (cycle >= warmup_cycles)
        {
            if (sample_count <
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


            if (previous_valid &&
                period_sample_count <
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


        previous_wake = wake;

        previous_valid = true;


        /*
         * Absolute schedule.
         *
         * Do not use:
         *
         *     now + period
         *
         * because this would hide accumulated timing error.
         */
        timespec_add_ns(
            &target,
            period_ns);
    }


    /* ========================================================
     * Calculate result
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
        result->jitter_mean_us =
            mean_i64_us(
                jitter_samples,
                sample_count);


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
        /*
         * Save abs max before qsort.
         */
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
     * Execution time
     */
    if (sample_count > 0)
    {
        result->exec_mean_us =
            mean_u64_us(
                exec_samples,
                sample_count);


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
        result->dc_mean_us =
            mean_u32_us(
                dc_samples,
                dc_count);


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


    /* ========================================================
     * Detailed display
     * ======================================================== */

    printf(
        "\nRESULT: %s\n",
        freq_text);


    printf(
        "Target period          : %.3f us\n",
        period_ns / 1000.0);


    printf(
        "Samples                : %zu\n",
        sample_count);


    /*
     * These functions sort again.
     * That is harmless because the arrays are already sorted.
     */
    print_i64_stats(
        "Wake-up jitter",
        jitter_samples,
        sample_count);


    print_i64_stats(
        "Cycle period error",
        period_error_samples,
        period_sample_count);


    print_u64_stats(
        "EtherCAT loop execution",
        exec_samples,
        sample_count);


    print_dc_stats(
        dc_samples,
        dc_count);


    printf(
        "\nEvents / errors\n");

    printf(
        "----------------------------------------------\n");


    printf(
        "Cycle overruns          : %" PRIu64 "\n",
        overruns);


    printf(
        "Receive errors          : %" PRIu64 "\n",
        receive_errors);


    printf(
        "Send errors             : %" PRIu64 "\n",
        send_errors);


    printf(
        "DC monitor errors       : %" PRIu64 "\n",
        dc_errors);


    if (sample_count > 0)
    {
        double utilization =
            result->exec_max_us
            /
            (period_ns / 1000.0)
            *
            100.0;


        printf(
            "Worst loop utilization : %.1f %%\n",
            utilization);
    }


    return 0;
}


/* ============================================================
 * Final summary table
 * ============================================================ */

static void print_summary_table(void)
{
    printf(
        "\n\n"
        "============================================================================================================\n");

    printf(
        " EtherCAT Timing Summary\n");

    printf(
        "============================================================================================================\n");


    printf(
        "%-9s "
        "%10s "
        "%11s "
        "%10s "
        "%12s "
        "%10s "
        "%9s "
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
        "DC Err");


    printf(
        "%-9s "
        "%10s "
        "%11s "
        "%10s "
        "%12s "
        "%10s "
        "%9s "
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


    for (size_t i = 0;
         i < result_count;
         i++)
    {
        const test_result_t *r =
            &results[i];


        char freq[32];

        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        printf(
            "%-9s "
            "%10.1f "
            "%11.2f "
            "%10.2f "
            "%12.2f "
            "%10.2f ",

            freq,

            r->period_ns /
                1000.0,

            r->jitter_p999_us,

            r->jitter_max_us,

            r->period_max_abs_us,

            r->exec_max_us);


        /*
         * No DC-capable slave:
         * display "-" instead of 0.00.
         */
        if (r->dc_samples > 0)
        {
            printf(
                "%9.2f ",
                r->dc_max_us);
        }
        else
        {
            printf(
                "%9s ",
                "-");
        }


        printf(
            "%9" PRIu64 " "
            "%8" PRIu64 "\n",

            r->overruns,

            r->dc_errors);
    }


    printf(
        "============================================================================================================\n");


    printf(
        "\n"
        "Jit      : Linux scheduled wake-up lateness\n"
        "PerErr   : Difference between actual cycle period and target period\n"
        "Exec     : Time spent in EtherCAT receive/DC/send section\n"
        "DC Max   : Maximum EtherCAT distributed-clock synchronization difference\n"
        "Overrun  : Wake-up occurred at least one complete cycle late\n");


    /*
     * Extra compact table focused on practical cycle selection.
     */
    printf(
        "\n"
        "Cycle margin\n");

    printf(
        "--------------------------------------------------------------------------------\n");

    printf(
        "%-9s "
        "%10s "
        "%12s "
        "%12s "
        "%12s\n",

        "Rate",
        "Period",
        "Max Jitter",
        "Max Exec",
        "Exec/Period");


    printf(
        "--------------------------------------------------------------------------------\n");


    for (size_t i = 0;
         i < result_count;
         i++)
    {
        const test_result_t *r =
            &results[i];


        char freq[32];

        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        double period_us =
            r->period_ns /
            1000.0;


        double utilization = 0.0;

        if (period_us > 0.0)
        {
            utilization =
                r->exec_max_us /
                period_us *
                100.0;
        }


        printf(
            "%-9s "
            "%10.1f "
            "%12.2f "
            "%12.2f "
            "%11.1f%%\n",

            freq,
            period_us,
            r->jitter_max_us,
            r->exec_max_us,
            utilization);
    }


    printf(
        "--------------------------------------------------------------------------------\n");
}


/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "IgH EtherCAT Timing / DC Benchmark\n"
        "==================================\n\n");


    printf(
        "Master index           : %d\n",
        MASTER_INDEX);


    printf(
        "Warm-up                : %d sec / frequency\n",
        WARMUP_SECONDS);


    printf(
        "Measurement            : %d sec / frequency\n",
        TEST_SECONDS);


    printf(
        "DC monitor             : approx. %d Hz\n",
        DC_MONITOR_HZ);


    printf(
        "Test frequencies       : ");


    for (size_t i = 0;
         i < NUM_TEST_FREQS;
         i++)
    {
        printf(
            "%u%s",
            test_frequencies[i],
            (i + 1 < NUM_TEST_FREQS)
                ? ", "
                : " Hz\n");
    }


    /*
     * Signals
     */
    signal(
        SIGINT,
        signal_handler);

    signal(
        SIGTERM,
        signal_handler);


    /*
     * Allocate memory before entering realtime mode.
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


    if (!jitter_samples ||
        !period_error_samples ||
        !exec_samples ||
        !dc_samples)
    {
        fprintf(
            stderr,
            "ERROR: memory allocation failed.\n");

        return EXIT_FAILURE;
    }


    /*
     * Prevent memory paging.
     */
    if (mlockall(
            MCL_CURRENT |
            MCL_FUTURE) != 0)
    {
        perror(
            "mlockall");

        printf(
            "WARNING: memory lock failed.\n");
    }
    else
    {
        printf(
            "Memory locking         : enabled\n");
    }


    /*
     * CPU affinity
     */
    configure_cpu_affinity();


    /*
     * Get master.
     */
    master =
        ecrt_request_master(
            MASTER_INDEX);


    if (!master)
    {
        fprintf(
            stderr,
            "\nERROR: Could not request EtherCAT master %d\n",
            MASTER_INDEX);

        fprintf(
            stderr,
            "\nCheck:\n"
            "  sudo ethercat master\n"
            "  sudo ethercat slaves\n\n");

        return EXIT_FAILURE;
    }


    printf(
        "EtherCAT master        : acquired\n");


    /*
     * Let IgH automatically select the first DC-capable slave
     * as reference.
     */
    if (ecrt_master_select_reference_clock(
            master,
            NULL) != 0)
    {
        printf(
            "DC reference           : automatic selection unavailable\n");
    }
    else
    {
        printf(
            "DC reference           : automatic\n");
    }


    /*
     * Activate master.
     */
    if (ecrt_master_activate(
            master) != 0)
    {
        fprintf(
            stderr,
            "ERROR: ecrt_master_activate() failed.\n");

        ecrt_release_master(
            master);

        return EXIT_FAILURE;
    }


    printf(
        "EtherCAT master        : activated\n");


    /*
     * Short initialization delay.
     */
    usleep(
        500000);


    print_master_state();


    /*
     * Enter realtime scheduler.
     */
    configure_realtime();


    /*
     * Run fixed test sequence.
     */
    for (size_t i = 0;
         i < NUM_TEST_FREQS;
         i++)
    {
        if (stop_requested)
            break;


        memset(
            jitter_samples,
            0,
            MAX_SAMPLES *
            sizeof(*jitter_samples));


        memset(
            period_error_samples,
            0,
            MAX_SAMPLES *
            sizeof(*period_error_samples));


        memset(
            exec_samples,
            0,
            MAX_SAMPLES *
            sizeof(*exec_samples));


        memset(
            dc_samples,
            0,
            MAX_SAMPLES *
            sizeof(*dc_samples));


        if (run_frequency_test(
                test_frequencies[i],
                &results[result_count])
            != 0)
        {
            break;
        }


        result_count++;
    }


    /*
     * Leave realtime mode before terminal output / cleanup.
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
            "\nBenchmark interrupted by user.\n");
    }
    else
    {
        printf(
            "\nAll tests completed.\n");
    }


    /*
     * EtherCAT cleanup.
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