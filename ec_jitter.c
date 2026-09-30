/*
 * ec_jitter.c
 *
 * IgH EtherCAT Timing Benchmark
 *
 * Fixed frequencies:
 *   1, 5, 10, 15, 20, 25, 50, 75, 100 kHz
 *
 * Modes:
 *   1. FreeRun
 *      - No master DC synchronization calls
 *
 *   2. DC Sync
 *      - ecrt_master_application_time() every cycle
 *      - DC clock correction at fixed 1 kHz
 *      - DC monitor at fixed 100 Hz
 *
 * Measurements:
 *   Wake Jit  : Linux wake-up lateness
 *   Send Jit  : interval error between ecrt_master_send() calls
 *   Exec      : receive -> processing -> send execution time
 *   DC Max    : maximum DC synchronization difference
 *   Overrun   : wake-up >= one target cycle late
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

#define MASTER_INDEX            0
#define RT_PRIORITY             80

#define WARMUP_SECONDS          2
#define TEST_SECONDS            10

/*
 * DC correction frequency is fixed.
 *
 * Important:
 * Do not execute DC correction at every benchmark cycle,
 * otherwise a 100 kHz test would perform 100x more DC
 * correction operations than a 1 kHz test.
 */
#define DC_SYNC_HZ              1000

/*
 * DC synchronization monitor frequency.
 */
#define DC_MONITOR_HZ           100


static const unsigned int test_frequencies[] =
{
    1000,
    5000,
    10000,
    15000,
    20000,
    25000,
    50000,
    75000,
    100000
};

#define NUM_TEST_FREQS \
    (sizeof(test_frequencies) / sizeof(test_frequencies[0]))

#define MAX_TEST_HZ 100000

#define MAX_SAMPLES \
    ((size_t)MAX_TEST_HZ * TEST_SECONDS)

#define EPOCH_2000_OFFSET_SEC 946684800ULL


/* ============================================================
 * Test mode
 * ============================================================ */

typedef enum
{
    MODE_FREERUN = 0,
    MODE_DC      = 1

} benchmark_mode_t;


/* ============================================================
 * Result
 * ============================================================ */

typedef struct
{
    unsigned int hz;

    uint64_t period_ns;

    size_t samples;
    size_t dc_samples;

    double wake_p999_us;
    double wake_max_us;

    double send_p999_us;
    double send_max_abs_us;

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


/*
 * Reused measurement buffers.
 *
 * 100 kHz * 10 sec = 1,000,000 samples.
 */
static int64_t  *wake_samples = NULL;
static int64_t  *send_interval_samples = NULL;
static uint64_t *exec_samples = NULL;
static uint32_t *dc_samples = NULL;


static test_result_t freerun_results[NUM_TEST_FREQS];
static test_result_t dc_results[NUM_TEST_FREQS];

static size_t freerun_result_count = 0;
static size_t dc_result_count = 0;


/* ============================================================
 * Signal
 * ============================================================ */

static void signal_handler(int sig)
{
    (void)sig;

    stop_requested = 1;
}


/* ============================================================
 * Time helpers
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
 * EtherCAT application time:
 *
 * ns since 2000-01-01.
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

    double pos =
        percentile *
        (double)(count - 1);

    size_t index =
        (size_t)ceil(pos);

    if (index >= count)
        index = count - 1;

    return index;
}


static int64_t abs_i64_safe(int64_t value)
{
    if (value == INT64_MIN)
        return INT64_MAX;

    if (value < 0)
        return -value;

    return value;
}


static int64_t max_abs_i64(
    const int64_t *data,
    size_t count)
{
    int64_t max_value = 0;

    for (size_t i = 0;
         i < count;
         i++)
    {
        int64_t value =
            abs_i64_safe(data[i]);

        if (value > max_value)
            max_value = value;
    }

    return max_value;
}


/* ============================================================
 * CPU affinity
 * ============================================================ */

static void configure_cpu_affinity(void)
{
    long cpu_count =
        sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0)
        return;

    /*
     * Use highest-numbered CPU.
     *
     * RPi4 / RPi5 normally -> CPU3.
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
    }
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

    if (sched_setscheduler(
            0,
            SCHED_FIFO,
            &sp) != 0)
    {
        perror("sched_setscheduler");
    }
}


static void restore_normal_scheduler(void)
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


/* ============================================================
 * Frequency display
 * ============================================================ */

static void format_frequency(
    unsigned int hz,
    char *buffer,
    size_t size)
{
    if ((hz % 1000) == 0)
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
 * Clear buffers
 * ============================================================ */

static void clear_measurement_buffers(void)
{
    memset(
        wake_samples,
        0,
        MAX_SAMPLES *
        sizeof(*wake_samples));

    memset(
        send_interval_samples,
        0,
        MAX_SAMPLES *
        sizeof(*send_interval_samples));

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
}


/* ============================================================
 * One benchmark
 * ============================================================ */

static int run_frequency_test(
    unsigned int hz,
    benchmark_mode_t mode,
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

    const uint64_t period_ns =
        result->period_ns;


    const size_t warmup_cycles =
        (size_t)hz *
        WARMUP_SECONDS;

    const size_t measurement_cycles =
        (size_t)hz *
        TEST_SECONDS;

    const size_t total_cycles =
        warmup_cycles +
        measurement_cycles;


    /*
     * Since benchmark starts at 1 kHz,
     * all frequencies are >= DC_SYNC_HZ.
     */
    unsigned int dc_sync_div =
        hz / DC_SYNC_HZ;

    if (dc_sync_div < 1)
        dc_sync_div = 1;


    unsigned int dc_monitor_div =
        hz / DC_MONITOR_HZ;

    if (dc_monitor_div < 1)
        dc_monitor_div = 1;


    size_t sample_count = 0;
    size_t send_sample_count = 0;
    size_t dc_count = 0;


    uint64_t overruns = 0;

    uint64_t receive_errors = 0;
    uint64_t send_errors = 0;
    uint64_t dc_errors = 0;


    struct timespec target;
    struct timespec wake;
    struct timespec exec_end;
    struct timespec send_time;
    struct timespec previous_send_time;


    bool previous_send_valid = false;
    bool monitor_pending = false;


    /*
     * Start first cycle 100 ms in the future.
     */
    clock_gettime(
        CLOCK_MONOTONIC,
        &target);

    timespec_add_ns(
        &target,
        100000000ULL);


    for (size_t cycle = 0;
         cycle < total_cycles;
         cycle++)
    {
        if (stop_requested)
            return -1;


        /*
         * Absolute-time sleep.
         */
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


        /*
         * Actual wake-up time.
         */
        clock_gettime(
            CLOCK_MONOTONIC,
            &wake);


        int64_t wake_jitter_ns =
            timespec_diff_ns(
                &wake,
                &target);


        if (wake_jitter_ns >=
            (int64_t)period_ns)
        {
            overruns++;
        }


        /*
         * EtherCAT RX.
         */
        if (ecrt_master_receive(
                master) != 0)
        {
            receive_errors++;
        }


        /*
         * DC mode only.
         */
        if (mode == MODE_DC)
        {
            /*
             * Retrieve previously queued monitor result.
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
                        cycle >= warmup_cycles &&
                        dc_count < MAX_SAMPLES)
                    {
                        dc_samples[
                            dc_count++] = dc;
                    }
                }

                monitor_pending = false;
            }


            /*
             * Application time should be supplied cyclically
             * while using distributed clocks.
             */
            uint64_t app_time =
                get_ethercat_application_time();

            if (ecrt_master_application_time(
                    master,
                    app_time) != 0)
            {
                dc_errors++;
            }


            /*
             * Queue actual clock corrections at FIXED 1 kHz.
             *
             * 1 kHz benchmark:
             *   every cycle
             *
             * 100 kHz benchmark:
             *   every 100 cycles
             */
            if ((cycle % dc_sync_div) == 0)
            {
                if (ecrt_master_sync_reference_clock(
                        master) != 0)
                {
                    dc_errors++;
                }

                if (ecrt_master_sync_slave_clocks(
                        master) != 0)
                {
                    dc_errors++;
                }
            }


            /*
             * Queue synchronization monitor at FIXED 100 Hz.
             */
            if ((cycle % dc_monitor_div) == 0)
            {
                if (!monitor_pending)
                {
                    if (ecrt_master_sync_monitor_queue(
                            master) == 0)
                    {
                        monitor_pending = true;
                    }
                    else
                    {
                        dc_errors++;
                    }
                }
            }
        }


        /*
         * Timestamp immediately before send().
         *
         * This is the application-side cyclic send point.
         */
        clock_gettime(
            CLOCK_MONOTONIC,
            &send_time);


        /*
         * Measure interval between send calls.
         *
         * This is useful for comparing:
         *
         *   bcmgenet
         *   vs
         *   Intel I210 / igb
         */
        if (
            previous_send_valid &&
            cycle >= warmup_cycles &&
            send_sample_count < MAX_SAMPLES)
        {
            int64_t send_interval_ns =
                timespec_diff_ns(
                    &send_time,
                    &previous_send_time);

            send_interval_samples[
                send_sample_count++]
                =
                send_interval_ns
                -
                (int64_t)period_ns;
        }


        previous_send_time =
            send_time;

        previous_send_valid =
            true;


        /*
         * EtherCAT TX.
         */
        if (ecrt_master_send(
                master) != 0)
        {
            send_errors++;
        }


        /*
         * End timestamp.
         */
        clock_gettime(
            CLOCK_MONOTONIC,
            &exec_end);


        uint64_t exec_ns =
            (uint64_t)
            timespec_diff_ns(
                &exec_end,
                &wake);


        /*
         * Store measurement after warmup.
         */
        if (
            cycle >= warmup_cycles &&
            sample_count < MAX_SAMPLES)
        {
            wake_samples[
                sample_count]
                =
                wake_jitter_ns;

            exec_samples[
                sample_count]
                =
                exec_ns;

            sample_count++;
        }


        /*
         * Absolute schedule.
         *
         * Important:
         * Never use "now + period".
         */
        timespec_add_ns(
            &target,
            period_ns);
    }


    /* ========================================================
     * Save statistics
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
     * Wake jitter
     */
    if (sample_count > 0)
    {
        qsort(
            wake_samples,
            sample_count,
            sizeof(wake_samples[0]),
            cmp_i64);

        result->wake_p999_us =
            wake_samples[
                percentile_index(
                    sample_count,
                    0.999)]
            /
            1000.0;

        result->wake_max_us =
            wake_samples[
                sample_count - 1]
            /
            1000.0;
    }


    /*
     * Send interval jitter
     */
    if (send_sample_count > 0)
    {
        result->send_max_abs_us =
            max_abs_i64(
                send_interval_samples,
                send_sample_count)
            /
            1000.0;

        /*
         * For P99.9 we compare absolute jitter.
         */
        for (size_t i = 0;
             i < send_sample_count;
             i++)
        {
            send_interval_samples[i] =
                abs_i64_safe(
                    send_interval_samples[i]);
        }

        qsort(
            send_interval_samples,
            send_sample_count,
            sizeof(send_interval_samples[0]),
            cmp_i64);

        result->send_p999_us =
            send_interval_samples[
                percentile_index(
                    send_sample_count,
                    0.999)]
            /
            1000.0;
    }


    /*
     * Loop execution
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
     * DC monitor
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
 * FreeRun table
 * ============================================================ */

static void print_freerun_table(void)
{
    printf(
        "\n"
        "============================================================================================================\n");

    printf(
        " FreeRun Benchmark - No Master DC Synchronization\n");

    printf(
        "============================================================================================================\n");


    printf(
        "%-8s "
        "%9s "
        "%10s "
        "%10s "
        "%11s "
        "%10s "
        "%10s "
        "%9s "
        "%8s\n",
        "Rate",
        "Period",
        "P99.9 Wake",
        "Max Wake",
        "P99.9 Send",
        "Max Send",
        "Max Exec",
        "Overrun",
        "Errors");


    printf(
        "%-8s "
        "%9s "
        "%10s "
        "%10s "
        "%11s "
        "%10s "
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


    for (size_t i = 0;
         i < freerun_result_count;
         i++)
    {
        const test_result_t *r =
            &freerun_results[i];

        char freq[32];

        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        uint64_t errors =
            r->receive_errors +
            r->send_errors;


        printf(
            "%-8s "
            "%9.2f "
            "%10.2f "
            "%10.2f "
            "%11.2f "
            "%10.2f "
            "%10.2f "
            "%9" PRIu64 " "
            "%8" PRIu64 "\n",

            freq,

            r->period_ns /
            1000.0,

            r->wake_p999_us,

            r->wake_max_us,

            r->send_p999_us,

            r->send_max_abs_us,

            r->exec_max_us,

            r->overruns,

            errors);
    }


    printf(
        "============================================================================================================\n");
}


/* ============================================================
 * DC table
 * ============================================================ */

static void print_dc_table(void)
{
    printf(
        "\n"
        "========================================================================================================================\n");

    printf(
        " DC Benchmark - DC Correction 1 kHz / Monitor 100 Hz\n");

    printf(
        "========================================================================================================================\n");


    printf(
        "%-8s "
        "%9s "
        "%10s "
        "%10s "
        "%11s "
        "%10s "
        "%10s "
        "%9s "
        "%9s "
        "%8s\n",

        "Rate",
        "Period",
        "P99.9 Wake",
        "Max Wake",
        "P99.9 Send",
        "Max Send",
        "Max Exec",
        "DC Max",
        "Overrun",
        "Errors");


    printf(
        "%-8s "
        "%9s "
        "%10s "
        "%10s "
        "%11s "
        "%10s "
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
        "(us)",
        "",
        "");


    printf(
        "------------------------------------------------------------------------------------------------------------------------\n");


    for (size_t i = 0;
         i < dc_result_count;
         i++)
    {
        const test_result_t *r =
            &dc_results[i];

        char freq[32];

        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        uint64_t errors =
            r->receive_errors +
            r->send_errors +
            r->dc_errors;


        printf(
            "%-8s "
            "%9.2f "
            "%10.2f "
            "%10.2f "
            "%11.2f "
            "%10.2f "
            "%10.2f ",

            freq,

            r->period_ns /
            1000.0,

            r->wake_p999_us,

            r->wake_max_us,

            r->send_p999_us,

            r->send_max_abs_us,

            r->exec_max_us);


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

            errors);
    }


    printf(
        "========================================================================================================================\n");
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
     * Allocate largest measurement buffers once.
     */
    wake_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*wake_samples));

    send_interval_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*send_interval_samples));

    exec_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*exec_samples));

    dc_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*dc_samples));


    if (
        !wake_samples ||
        !send_interval_samples ||
        !exec_samples ||
        !dc_samples)
    {
        fprintf(
            stderr,
            "Memory allocation failed.\n");

        return EXIT_FAILURE;
    }


    /*
     * Avoid paging during realtime benchmark.
     */
    if (mlockall(
            MCL_CURRENT |
            MCL_FUTURE) != 0)
    {
        perror("mlockall");
    }


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
            "Cannot request EtherCAT master.\n");

        return EXIT_FAILURE;
    }


    /*
     * Automatic DC reference selection.
     *
     * Has no effect on FreeRun measurement until
     * DC synchronization API calls are used.
     */
    ecrt_master_select_reference_clock(
        master,
        NULL);


    /*
     * Activate master.
     */
    if (ecrt_master_activate(
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
     * Let master settle.
     */
    usleep(
        500000);


    configure_realtime();


    /* ========================================================
     * FreeRun
     * ======================================================== */

    printf(
        "Running FreeRun benchmark...\n\n");


    for (size_t i = 0;
         i < NUM_TEST_FREQS;
         i++)
    {
        if (stop_requested)
            break;


        clear_measurement_buffers();


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

        fflush(stdout);


        if (run_frequency_test(
                test_frequencies[i],
                MODE_FREERUN,
                &freerun_results[
                    freerun_result_count]) != 0)
        {
            printf("STOP\n");

            break;
        }


        freerun_result_count++;


        printf("done\n");
    }


    /*
     * Small pause before DC test.
     */
    if (!stop_requested)
    {
        usleep(
            500000);
    }


    /* ========================================================
     * DC mode
     * ======================================================== */

    if (!stop_requested)
    {
        printf(
            "\nRunning DC benchmark...\n\n");


        for (size_t i = 0;
             i < NUM_TEST_FREQS;
             i++)
        {
            if (stop_requested)
                break;


            clear_measurement_buffers();


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

            fflush(stdout);


            if (run_frequency_test(
                    test_frequencies[i],
                    MODE_DC,
                    &dc_results[
                        dc_result_count]) != 0)
            {
                printf("STOP\n");

                break;
            }


            dc_result_count++;


            printf("done\n");
        }
    }


    /*
     * Return to normal scheduler before printing.
     */
    restore_normal_scheduler();


    /*
     * Final output only.
     */
    if (freerun_result_count > 0)
    {
        print_freerun_table();
    }


    if (dc_result_count > 0)
    {
        print_dc_table();
    }


    printf(
        "\n"
        "Wake = Linux scheduler wake-up delay\n"
        "Send = interval error between ecrt_master_send() calls\n"
        "Exec = receive / DC processing / send execution time\n"
        "DC   = EtherCAT distributed-clock synchronization difference\n"
        "\n"
        "P99.9 Send and Max Send use absolute interval error.\n");


    if (stop_requested)
    {
        printf(
            "\nStopped by user.\n");
    }


    /*
     * Cleanup.
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


    free(wake_samples);
    free(send_interval_samples);
    free(exec_samples);
    free(dc_samples);


    return EXIT_SUCCESS;
}