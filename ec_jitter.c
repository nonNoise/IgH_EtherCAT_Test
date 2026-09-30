/*
 * ec_jitter.c
 *
 * Generic IgH EtherCAT timing / jitter / DC synchronization benchmark
 *
 * No PDO configuration
 * No Vendor ID / Product Code dependency
 * No slave-specific configuration
 *
 * Test frequencies:
 *   100 Hz
 *   250 Hz
 *   500 Hz
 *   1000 Hz
 *   2000 Hz
 *   4000 Hz
 *   5000 Hz
 *   10000 Hz
 *
 * Build example:
 *
 *   gcc -O2 -Wall -Wextra -o ec_jitter ec_jitter.c \
 *       -I/opt/etherlab/include \
 *       -L/opt/etherlab/lib \
 *       -lethercat -lm
 *
 * or depending on installation:
 *
 *   gcc -O2 -Wall -Wextra -o ec_jitter ec_jitter.c \
 *       $(pkg-config --cflags --libs ethercat) -lm
 *
 * Run:
 *
 *   sudo ./ec_jitter
 *
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
 * Fixed test configuration
 * ============================================================ */

#define MASTER_INDEX            0

#define RT_PRIORITY             80

#define WARMUP_SECONDS          2
#define TEST_SECONDS            10

/*
 * DC monitor itself adds an EtherCAT datagram.
 *
 * Keep monitoring at about 100 Hz regardless of cyclic frequency,
 * so measurement overhead does not grow excessively at 10 kHz.
 */
#define DC_MONITOR_HZ           100

/*
 * Synchronize reference/slave clocks every cycle.
 */
#define ENABLE_DC_SYNC          1

/*
 * Frequencies are intentionally fixed.
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

#define MAX_TEST_HZ             10000
#define MAX_SAMPLES             (MAX_TEST_HZ * TEST_SECONDS)

/* EtherCAT / Unix epoch difference:
 *
 * 1970-01-01 -> 2000-01-01
 */
#define EPOCH_2000_OFFSET_SEC    946684800ULL


/* ============================================================
 * Globals
 * ============================================================ */

static volatile sig_atomic_t stop_requested = 0;

static ec_master_t *master = NULL;

/*
 * Pre-allocated buffers.
 *
 * No malloc/free occurs inside the realtime measurement loop.
 */
static int64_t  *jitter_samples       = NULL;
static int64_t  *period_error_samples = NULL;
static uint64_t *exec_samples         = NULL;
static uint32_t *dc_samples           = NULL;


/* ============================================================
 * Utility functions
 * ============================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    stop_requested = 1;
}


static inline uint64_t ts_to_ns(const struct timespec *ts)
{
    return ((uint64_t)ts->tv_sec * 1000000000ULL)
         + (uint64_t)ts->tv_nsec;
}


static inline int64_t ts_diff_ns(
    const struct timespec *a,
    const struct timespec *b)
{
    return ((int64_t)a->tv_sec - (int64_t)b->tv_sec)
             * 1000000000LL
         + ((int64_t)a->tv_nsec - (int64_t)b->tv_nsec);
}


static inline void ts_add_ns(
    struct timespec *ts,
    uint64_t ns)
{
    ts->tv_nsec += ns;

    while (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}


/*
 * IgH application time:
 * nanoseconds since 2000-01-01 00:00:00.
 */
static uint64_t get_ethercat_application_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    return
        ((uint64_t)ts.tv_sec - EPOCH_2000_OFFSET_SEC)
            * 1000000000ULL
        + (uint64_t)ts.tv_nsec;
}


/* ============================================================
 * Sorting / statistics
 * ============================================================ */

static int cmp_i64(const void *a, const void *b)
{
    const int64_t aa = *(const int64_t *)a;
    const int64_t bb = *(const int64_t *)b;

    if (aa < bb)
        return -1;

    if (aa > bb)
        return 1;

    return 0;
}


static int cmp_u64(const void *a, const void *b)
{
    const uint64_t aa = *(const uint64_t *)a;
    const uint64_t bb = *(const uint64_t *)b;

    if (aa < bb)
        return -1;

    if (aa > bb)
        return 1;

    return 0;
}


static int cmp_u32(const void *a, const void *b)
{
    const uint32_t aa = *(const uint32_t *)a;
    const uint32_t bb = *(const uint32_t *)b;

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

    double pos = percentile * (double)(count - 1);

    size_t idx = (size_t)ceil(pos);

    if (idx >= count)
        idx = count - 1;

    return idx;
}


static void print_i64_statistics(
    const char *name,
    int64_t *samples,
    size_t count)
{
    if (count == 0)
    {
        printf("%-22s : no samples\n", name);
        return;
    }

    long double sum = 0.0L;
    long double sq_sum = 0.0L;

    for (size_t i = 0; i < count; ++i)
    {
        long double x = (long double)samples[i];

        sum += x;
        sq_sum += x * x;
    }

    long double mean = sum / (long double)count;

    long double variance =
        (sq_sum / (long double)count) - mean * mean;

    if (variance < 0.0L)
        variance = 0.0L;

    long double stddev = sqrtl(variance);

    qsort(
        samples,
        count,
        sizeof(samples[0]),
        cmp_i64);

    size_t p95 =
        percentile_index(count, 0.95);

    size_t p99 =
        percentile_index(count, 0.99);

    size_t p999 =
        percentile_index(count, 0.999);

    printf("\n%s\n", name);

    printf("  Mean        : %10.3Lf us\n",
           mean / 1000.0L);

    printf("  StdDev      : %10.3Lf us\n",
           stddev / 1000.0L);

    printf("  Min         : %10.3f us\n",
           samples[0] / 1000.0);

    printf("  P95         : %10.3f us\n",
           samples[p95] / 1000.0);

    printf("  P99         : %10.3f us\n",
           samples[p99] / 1000.0);

    printf("  P99.9       : %10.3f us\n",
           samples[p999] / 1000.0);

    printf("  Max         : %10.3f us\n",
           samples[count - 1] / 1000.0);
}


static void print_u64_statistics(
    const char *name,
    uint64_t *samples,
    size_t count)
{
    if (count == 0)
    {
        printf("%-22s : no samples\n", name);
        return;
    }

    long double sum = 0.0L;
    long double sq_sum = 0.0L;

    for (size_t i = 0; i < count; ++i)
    {
        long double x = (long double)samples[i];

        sum += x;
        sq_sum += x * x;
    }

    long double mean =
        sum / (long double)count;

    long double variance =
        (sq_sum / (long double)count)
        - mean * mean;

    if (variance < 0.0L)
        variance = 0.0L;

    long double stddev =
        sqrtl(variance);

    qsort(
        samples,
        count,
        sizeof(samples[0]),
        cmp_u64);

    size_t p95 =
        percentile_index(count, 0.95);

    size_t p99 =
        percentile_index(count, 0.99);

    size_t p999 =
        percentile_index(count, 0.999);

    printf("\n%s\n", name);

    printf("  Mean        : %10.3Lf us\n",
           mean / 1000.0L);

    printf("  StdDev      : %10.3Lf us\n",
           stddev / 1000.0L);

    printf("  Min         : %10.3f us\n",
           samples[0] / 1000.0);

    printf("  P95         : %10.3f us\n",
           samples[p95] / 1000.0);

    printf("  P99         : %10.3f us\n",
           samples[p99] / 1000.0);

    printf("  P99.9       : %10.3f us\n",
           samples[p999] / 1000.0);

    printf("  Max         : %10.3f us\n",
           samples[count - 1] / 1000.0);
}


static void print_dc_statistics(
    uint32_t *samples,
    size_t count)
{
    printf("\nDC synchronization difference\n");

    if (count == 0)
    {
        printf("  No valid DC samples.\n");
        printf("  This is normal if no DC-capable slaves are present.\n");
        return;
    }

    uint64_t sum = 0;

    for (size_t i = 0; i < count; ++i)
        sum += samples[i];

    qsort(
        samples,
        count,
        sizeof(samples[0]),
        cmp_u32);

    size_t p95 =
        percentile_index(count, 0.95);

    size_t p99 =
        percentile_index(count, 0.99);

    size_t p999 =
        percentile_index(count, 0.999);

    double mean =
        (double)sum / (double)count;

    printf("  Samples     : %zu\n",
           count);

    printf("  Mean        : %10.3f us\n",
           mean / 1000.0);

    printf("  Min         : %10.3f us\n",
           samples[0] / 1000.0);

    printf("  P95         : %10.3f us\n",
           samples[p95] / 1000.0);

    printf("  P99         : %10.3f us\n",
           samples[p99] / 1000.0);

    printf("  P99.9       : %10.3f us\n",
           samples[p999] / 1000.0);

    printf("  Max         : %10.3f us\n",
           samples[count - 1] / 1000.0);
}


/* ============================================================
 * Realtime setup
 * ============================================================ */

static int configure_cpu_affinity(void)
{
    long cpu_count =
        sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0)
        return -1;

    /*
     * Use the last CPU automatically.
     *
     * RPi4:
     *   CPU 3
     *
     * RPi5:
     *   CPU 3
     *
     * Larger x86:
     *   highest numbered CPU
     */
    int cpu =
        (int)cpu_count - 1;

    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    if (sched_setaffinity(
            0,
            sizeof(cpuset),
            &cpuset) != 0)
    {
        perror("sched_setaffinity");
        return -1;
    }

    printf("CPU affinity           : CPU %d\n",
           cpu);

    return cpu;
}


static int configure_realtime(void)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof(sp));

    sp.sched_priority =
        RT_PRIORITY;

    if (sched_setscheduler(
            0,
            SCHED_FIFO,
            &sp) != 0)
    {
        perror("sched_setscheduler");

        printf(
            "WARNING: SCHED_FIFO could not be enabled.\n"
            "Run with sudo/root.\n");

        return -1;
    }

    printf("Scheduling             : SCHED_FIFO\n");
    printf("RT priority            : %d\n",
           RT_PRIORITY);

    return 0;
}


/* ============================================================
 * EtherCAT status
 * ============================================================ */

static void print_master_state(void)
{
    ec_master_state_t state;

    memset(&state, 0, sizeof(state));

    if (ecrt_master_state(
            master,
            &state) != 0)
    {
        printf(
            "Unable to read master state.\n");

        return;
    }

    printf("\nEtherCAT master\n");
    printf("----------------------------------------\n");

    printf(
        "Slaves responding      : %u\n",
        state.slaves_responding);

    printf(
        "AL states              : 0x%02X\n",
        state.al_states);

    printf(
        "Link                   : %s\n",
        state.link_up ? "UP" : "DOWN");
}


/* ============================================================
 * One frequency test
 * ============================================================ */

static int run_frequency_test(
    unsigned int hz)
{
    const uint64_t period_ns =
        1000000000ULL / hz;

    const size_t warmup_cycles =
        (size_t)hz * WARMUP_SECONDS;

    const size_t test_cycles =
        (size_t)hz * TEST_SECONDS;

    const size_t total_cycles =
        warmup_cycles + test_cycles;

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

    printf(
        "\n"
        "============================================================\n");

    printf(
        " Test: %u Hz\n",
        hz);

    printf(
        " Target period : %.3f us\n",
        (double)period_ns / 1000.0);

    printf(
        " Warm-up       : %d s\n",
        WARMUP_SECONDS);

    printf(
        " Measurement   : %d s\n",
        TEST_SECONDS);

    printf(
        " DC monitor    : approx. %u Hz\n",
        hz / monitor_div);

    printf(
        "============================================================\n");

    fflush(stdout);

    size_t sample_count = 0;
    size_t dc_count = 0;

    uint64_t overruns = 0;

    uint64_t receive_errors = 0;
    uint64_t send_errors = 0;

    uint64_t dc_monitor_errors = 0;

    uint64_t max_lateness_ns = 0;

    struct timespec target;
    struct timespec wake;
    struct timespec previous_wake;
    struct timespec exec_end;

    bool have_previous_wake = false;

    bool monitor_pending = false;

    clock_gettime(
        CLOCK_MONOTONIC,
        &target);

    /*
     * Give the first cycle 100 ms setup margin.
     */
    ts_add_ns(
        &target,
        100000000ULL);

    for (size_t cycle = 0;
         cycle < total_cycles;
         ++cycle)
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

        } while (
            sleep_ret == EINTR
            && !stop_requested);

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
         * Linux wake-up latency / scheduling jitter.
         *
         * Positive:
         *   woke up late
         */
        int64_t jitter_ns =
            ts_diff_ns(
                &wake,
                &target);

        if (jitter_ns > 0
            && (uint64_t)jitter_ns
                > max_lateness_ns)
        {
            max_lateness_ns =
                (uint64_t)jitter_ns;
        }

        /*
         * If wake-up itself exceeds one full cycle,
         * count this as an overrun.
         */
        if (jitter_ns >=
            (int64_t)period_ns)
        {
            overruns++;
        }

        /*
         * Receive frames from previous cycle.
         */
        if (ecrt_master_receive(master) != 0)
        {
            receive_errors++;
        }

        /*
         * Process the DC monitor datagram queued
         * in a previous cycle.
         */
        if (monitor_pending)
        {
            uint32_t dc =
                ecrt_master_sync_monitor_process(
                    master);

            if (dc == UINT32_MAX)
            {
                dc_monitor_errors++;
            }
            else
            {
                if (cycle >= warmup_cycles
                    && dc_count < MAX_SAMPLES)
                {
                    dc_samples[dc_count++] =
                        dc;
                }
            }

            monitor_pending = false;
        }

#if ENABLE_DC_SYNC

        /*
         * IgH requires application time to be
         * supplied cyclically for DC operation.
         */
        uint64_t app_time =
            get_ethercat_application_time();

        ecrt_master_application_time(
            master,
            app_time);

        /*
         * Synchronize EtherCAT reference clock
         * to application time.
         */
        ecrt_master_sync_reference_clock(
            master);

        /*
         * Synchronize all DC-capable slaves
         * against the reference clock.
         */
        ecrt_master_sync_slave_clocks(
            master);

#endif

        /*
         * DC monitor at about 100 Hz.
         *
         * This avoids adding a monitor datagram
         * to every 10 kHz cycle.
         */
        if ((cycle % monitor_div) == 0)
        {
            if (ecrt_master_sync_monitor_queue(
                    master) == 0)
            {
                monitor_pending = true;
            }
            else
            {
                dc_monitor_errors++;
            }
        }

        /*
         * Transmit all queued EtherCAT datagrams.
         */
        if (ecrt_master_send(master) != 0)
        {
            send_errors++;
        }

        clock_gettime(
            CLOCK_MONOTONIC,
            &exec_end);

        uint64_t exec_ns =
            (uint64_t)
            ts_diff_ns(
                &exec_end,
                &wake);

        /*
         * Store only after warmup.
         */
        if (cycle >= warmup_cycles)
        {
            if (sample_count <
                MAX_SAMPLES)
            {
                jitter_samples[
                    sample_count]
                    = jitter_ns;

                exec_samples[
                    sample_count]
                    = exec_ns;

                if (have_previous_wake)
                {
                    int64_t actual_period =
                        ts_diff_ns(
                            &wake,
                            &previous_wake);

                    period_error_samples[
                        sample_count]
                        =
                        actual_period
                        - (int64_t)period_ns;
                }
                else
                {
                    period_error_samples[
                        sample_count]
                        = 0;
                }

                sample_count++;
            }
        }

        previous_wake = wake;
        have_previous_wake = true;

        /*
         * Absolute schedule.
         *
         * Do NOT calculate next time from "now".
         * That would hide accumulated timing errors.
         */
        ts_add_ns(
            &target,
            period_ns);
    }

    /*
     * Results
     */
    printf("\nRESULT\n");
    printf("----------------------------------------\n");

    printf(
        "Frequency             : %u Hz\n",
        hz);

    printf(
        "Target period         : %.3f us\n",
        (double)period_ns / 1000.0);

    printf(
        "Samples               : %zu\n",
        sample_count);

    print_i64_statistics(
        "Wake-up jitter",
        jitter_samples,
        sample_count);

    print_i64_statistics(
        "Cycle period error",
        period_error_samples,
        sample_count);

    print_u64_statistics(
        "EtherCAT loop execution",
        exec_samples,
        sample_count);

    print_dc_statistics(
        dc_samples,
        dc_count);

    printf("\nErrors / events\n");

    printf(
        "  Cycle overruns       : %" PRIu64 "\n",
        overruns);

    printf(
        "  Receive errors       : %" PRIu64 "\n",
        receive_errors);

    printf(
        "  Send errors          : %" PRIu64 "\n",
        send_errors);

    printf(
        "  DC monitor errors    : %" PRIu64 "\n",
        dc_monitor_errors);

    printf(
        "  Max wake lateness    : %.3f us\n",
        (double)max_lateness_ns / 1000.0);

    /*
     * Simple interpretation.
     *
     * This is intentionally descriptive,
     * not an arbitrary PASS/FAIL threshold.
     */
    printf("\nCycle utilization\n");

    if (sample_count > 0)
    {
        uint64_t max_exec =
            exec_samples[
                sample_count - 1];

        double utilization =
            100.0
            * (double)max_exec
            / (double)period_ns;

        printf(
            "  Worst execution/period : %.1f %%\n",
            utilization);

        if (max_exec >= period_ns)
        {
            printf(
                "  WARNING: EtherCAT loop execution exceeded one cycle.\n");
        }

        if (max_lateness_ns >= period_ns)
        {
            printf(
                "  WARNING: scheduler wake-up was later than one full cycle.\n");
        }
    }

    print_master_state();

    return 0;
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    printf(
        "\n"
        "IgH EtherCAT Timing / DC Benchmark\n"
        "=================================\n\n");

    printf(
        "Master index           : %d\n",
        MASTER_INDEX);

    printf(
        "Warm-up per frequency  : %d sec\n",
        WARMUP_SECONDS);

    printf(
        "Measurement per freq   : %d sec\n",
        TEST_SECONDS);

    printf(
        "DC monitor rate        : ~%d Hz\n",
        DC_MONITOR_HZ);

    printf(
        "Frequencies            : ");

    for (size_t i = 0;
         i < NUM_TEST_FREQS;
         ++i)
    {
        printf(
            "%u%s",
            test_frequencies[i],
            (i + 1 == NUM_TEST_FREQS)
                ? " Hz\n"
                : ", ");
    }

    /*
     * Signal handling.
     */
    signal(
        SIGINT,
        signal_handler);

    signal(
        SIGTERM,
        signal_handler);

    /*
     * Allocate all buffers before realtime operation.
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

    if (!jitter_samples
        || !period_error_samples
        || !exec_samples
        || !dc_samples)
    {
        fprintf(
            stderr,
            "Memory allocation failed.\n");

        return EXIT_FAILURE;
    }

    /*
     * Lock memory to prevent paging during realtime operation.
     */
    if (mlockall(
            MCL_CURRENT |
            MCL_FUTURE) != 0)
    {
        perror("mlockall");

        fprintf(
            stderr,
            "WARNING: memory could not be locked.\n");
    }
    else
    {
        printf(
            "Memory locking         : enabled\n");
    }

    /*
     * Bind process to one CPU.
     */
    configure_cpu_affinity();

    /*
     * Request EtherCAT master.
     */
    master =
        ecrt_request_master(
            MASTER_INDEX);

    if (!master)
    {
        fprintf(
            stderr,
            "\n"
            "ERROR: unable to request EtherCAT master %d.\n"
            "\n"
            "Check:\n"
            "  sudo ethercat master\n"
            "  sudo ethercat slaves\n"
            "\n",
            MASTER_INDEX);

        return EXIT_FAILURE;
    }

    printf(
        "EtherCAT master        : acquired\n");

    /*
     * If no explicit reference clock is selected,
     * IgH chooses the first DC-capable slave.
     */
    if (ecrt_master_select_reference_clock(
            master,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "WARNING: unable to select automatic DC reference clock.\n");
    }

    /*
     * Activate cyclic mode.
     *
     * From this point the userspace application is
     * responsible for cyclic send/receive.
     */
    if (ecrt_master_activate(
            master) != 0)
    {
        fprintf(
            stderr,
            "ERROR: ecrt_master_activate() failed.\n");

        ecrt_release_master(master);

        return EXIT_FAILURE;
    }

    printf(
        "EtherCAT master        : activated\n");

    /*
     * Give slaves / master a moment before realtime test.
     */
    usleep(500000);

    print_master_state();

    /*
     * Switch to realtime scheduling only after initialization.
     */
    configure_realtime();

    /*
     * Run all fixed frequencies.
     */
    for (size_t i = 0;
         i < NUM_TEST_FREQS;
         ++i)
    {
        if (stop_requested)
            break;

        memset(
            jitter_samples,
            0,
            MAX_SAMPLES
                * sizeof(*jitter_samples));

        memset(
            period_error_samples,
            0,
            MAX_SAMPLES
                * sizeof(*period_error_samples));

        memset(
            exec_samples,
            0,
            MAX_SAMPLES
                * sizeof(*exec_samples));

        memset(
            dc_samples,
            0,
            MAX_SAMPLES
                * sizeof(*dc_samples));

        if (run_frequency_test(
                test_frequencies[i]) != 0)
        {
            break;
        }
    }

    /*
     * Restore normal scheduling before cleanup.
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

    printf(
        "\n"
        "============================================================\n");

    if (stop_requested)
    {
        printf(
            "Benchmark interrupted by user.\n");
    }
    else
    {
        printf(
            "All tests completed.\n");
    }

    printf(
        "============================================================\n");

    if (master)
    {
        ecrt_master_deactivate(
            master);

        ecrt_release_master(
            master);

        master = NULL;
    }

    munlockall();

    free(jitter_samples);
    free(period_error_samples);
    free(exec_samples);
    free(dc_samples);

    return EXIT_SUCCESS;
}