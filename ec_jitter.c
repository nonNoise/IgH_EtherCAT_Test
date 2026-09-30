/*
 * ec_jitter.c
 *
 * IgH EtherCAT FreeRun Timing Benchmark
 *
 * Purpose:
 *   Compare EtherCAT master timing performance between NIC/driver combinations.
 *
 * Examples:
 *
 *   Intel I210 + ec_igb
 *   Raspberry Pi Ethernet + ec_generic
 *
 * No PDO configuration.
 * No DC synchronization.
 * No external JSON.
 *
 * Every cycle, this benchmark tries to read:
 *
 *   ESC register 0x0130 : AL Status
 *   Size                : 2 bytes
 *
 * Slave position 0 is detected automatically.
 * Vendor ID and Product Code are read automatically from the slave.
 *
 * Frequencies:
 *
 *   1 kHz
 *   5 kHz
 *   10 kHz
 *   15 kHz
 *   20 kHz
 *   25 kHz
 *   50 kHz
 *   75 kHz
 *   100 kHz
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

/*
 * Slave physical ring position.
 *
 * Position 0 = first EtherCAT slave.
 */
#define SLAVE_POSITION          0
#define SLAVE_ALIAS             0


/*
 * ESC register used for benchmark.
 *
 * 0x0130 = AL Status
 *
 * Read only.
 */
#define ESC_REGISTER_ADDRESS    0x0130
#define ESC_REGISTER_SIZE       2


#define RT_PRIORITY             80

#define WARMUP_SECONDS          2
#define TEST_SECONDS            10


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


#define MAX_TEST_HZ             100000

#define MAX_SAMPLES \
    ((size_t)MAX_TEST_HZ * TEST_SECONDS)


/* ============================================================
 * Result
 * ============================================================ */

typedef struct
{
    unsigned int hz;

    uint64_t period_ns;

    size_t cycle_samples;
    size_t send_samples;
    size_t rtt_samples;

    double wake_p999_us;
    double wake_max_us;

    double send_p999_us;
    double send_max_us;

    double exec_p999_us;
    double exec_max_us;

    double rtt_p99_us;
    double rtt_p999_us;
    double rtt_max_us;

    uint64_t late_events;
    uint64_t skipped_cycles;

    uint64_t receive_errors;
    uint64_t send_errors;

    uint64_t reg_success;
    uint64_t reg_errors;
    uint64_t reg_busy_cycles;

} test_result_t;


/* ============================================================
 * Globals
 * ============================================================ */

static volatile sig_atomic_t stop_requested = 0;

static ec_master_t *master = NULL;

static ec_slave_config_t *slave_config = NULL;

static ec_reg_request_t *reg_request = NULL;


/*
 * Measurement arrays.
 */
static int64_t *wake_samples = NULL;
static int64_t *send_samples = NULL;

static uint64_t *exec_samples = NULL;
static uint64_t *rtt_samples = NULL;


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


static int64_t abs_i64_safe(
    int64_t value)
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
            abs_i64_safe(
                data[i]);


        if (value > max_value)
            max_value = value;
    }


    return max_value;
}


/* ============================================================
 * Frequency formatting
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
 * CPU affinity
 * ============================================================ */

static int configure_cpu_affinity(void)
{
    long cpu_count =
        sysconf(
            _SC_NPROCESSORS_ONLN);


    if (cpu_count <= 0)
        return -1;


    /*
     * Use last online CPU.
     *
     * Raspberry Pi 4/5 normally:
     * CPU 3.
     */
    int cpu =
        (int)cpu_count - 1;


    cpu_set_t set;

    CPU_ZERO(&set);

    CPU_SET(
        cpu,
        &set);


    if (sched_setaffinity(
            0,
            sizeof(set),
            &set) != 0)
    {
        return -1;
    }


    return cpu;
}


/* ============================================================
 * Realtime scheduling
 * ============================================================ */

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
        return -1;
    }


    return 0;
}


static void restore_scheduler(void)
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
 * Measurement buffer reset
 * ============================================================ */

static void clear_buffers(void)
{
    memset(
        wake_samples,
        0,
        MAX_SAMPLES *
        sizeof(*wake_samples));


    memset(
        send_samples,
        0,
        MAX_SAMPLES *
        sizeof(*send_samples));


    memset(
        exec_samples,
        0,
        MAX_SAMPLES *
        sizeof(*exec_samples));


    memset(
        rtt_samples,
        0,
        MAX_SAMPLES *
        sizeof(*rtt_samples));
}


/* ============================================================
 * Wait until slave scan is available
 * ============================================================ */

static int wait_for_slave(
    ec_slave_info_t *slave_info)
{
    /*
     * ecrt_request_master() may return while the bus scan
     * is still completing.
     *
     * Wait up to ~5 seconds.
     */
    for (int i = 0;
         i < 50;
         i++)
    {
        memset(
            slave_info,
            0,
            sizeof(*slave_info));


        if (ecrt_master_get_slave(
                master,
                SLAVE_POSITION,
                slave_info) == 0)
        {
            return 0;
        }


        usleep(
            100000);
    }


    return -1;
}


/* ============================================================
 * Run one frequency
 * ============================================================ */

static int run_test(
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
        1000000000ULL /
        hz;


    const uint64_t period_ns =
        result->period_ns;


    const size_t warmup_cycles =
        (size_t)hz *
        WARMUP_SECONDS;


    const size_t measure_cycles =
        (size_t)hz *
        TEST_SECONDS;


    const size_t total_cycles =
        warmup_cycles +
        measure_cycles;


    size_t cycle_sample_count = 0;
    size_t send_sample_count = 0;
    size_t rtt_sample_count = 0;


    struct timespec target;
    struct timespec wake;
    struct timespec send_time;
    struct timespec previous_send;
    struct timespec exec_end;


    /*
     * Register request start timestamp.
     */
    struct timespec reg_start;


    bool previous_send_valid = false;
    bool reg_start_valid = false;


    uint64_t late_events = 0;
    uint64_t skipped_cycles = 0;

    uint64_t receive_errors = 0;
    uint64_t send_errors = 0;

    uint64_t reg_success = 0;
    uint64_t reg_errors = 0;
    uint64_t reg_busy_cycles = 0;


    /*
     * Start 100 ms in the future.
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
         * ----------------------------------------------------
         * Sleep until absolute deadline.
         * ----------------------------------------------------
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
         * ----------------------------------------------------
         * Wake timestamp.
         * ----------------------------------------------------
         */

        clock_gettime(
            CLOCK_MONOTONIC,
            &wake);


        int64_t wake_lateness_ns =
            timespec_diff_ns(
                &wake,
                &target);


        bool rebase_schedule = false;


        /*
         * If one or more periods have already passed,
         * do NOT try to catch up by executing cycles back-to-back.
         *
         * Instead count skipped periods and restart from
         * a future deadline.
         */
        if (wake_lateness_ns >=
            (int64_t)period_ns)
        {
            late_events++;


            uint64_t skipped =
                (uint64_t)wake_lateness_ns /
                period_ns;


            if (skipped == 0)
                skipped = 1;


            skipped_cycles +=
                skipped;


            rebase_schedule =
                true;
        }


        /*
         * ----------------------------------------------------
         * Receive EtherCAT frames.
         * ----------------------------------------------------
         */

        if (ecrt_master_receive(
                master) != 0)
        {
            receive_errors++;
        }


        /*
         * ----------------------------------------------------
         * Check register request result.
         * ----------------------------------------------------
         */

        ec_request_state_t state =
            ecrt_reg_request_state(
                reg_request);


        if (state == EC_REQUEST_SUCCESS)
        {
            reg_success++;


            if (
                reg_start_valid &&
                cycle >= warmup_cycles &&
                rtt_sample_count < MAX_SAMPLES)
            {
                struct timespec now;


                clock_gettime(
                    CLOCK_MONOTONIC,
                    &now);


                int64_t rtt_ns =
                    timespec_diff_ns(
                        &now,
                        &reg_start);


                if (rtt_ns >= 0)
                {
                    rtt_samples[
                        rtt_sample_count++]
                        =
                        (uint64_t)rtt_ns;
                }
            }


            reg_start_valid =
                false;
        }
        else if (state == EC_REQUEST_ERROR)
        {
            reg_errors++;

            reg_start_valid =
                false;
        }
        else if (state == EC_REQUEST_BUSY)
        {
            reg_busy_cycles++;
        }


        /*
         * ----------------------------------------------------
         * Queue next ESC register read.
         *
         * Never queue a new request while the old request
         * is BUSY.
         * ----------------------------------------------------
         */

        state =
            ecrt_reg_request_state(
                reg_request);


        if (state != EC_REQUEST_BUSY)
        {
            clock_gettime(
                CLOCK_MONOTONIC,
                &reg_start);


            if (ecrt_reg_request_read(
                    reg_request,
                    ESC_REGISTER_ADDRESS,
                    ESC_REGISTER_SIZE) == 0)
            {
                reg_start_valid =
                    true;
            }
            else
            {
                reg_errors++;

                reg_start_valid =
                    false;
            }
        }


        /*
         * ----------------------------------------------------
         * Timestamp immediately before ecrt_master_send().
         *
         * This measures application-side EtherCAT send timing.
         * ----------------------------------------------------
         */

        clock_gettime(
            CLOCK_MONOTONIC,
            &send_time);


        /*
         * Measure actual interval between calls to send().
         */
        if (
            previous_send_valid &&
            cycle >= warmup_cycles &&
            send_sample_count < MAX_SAMPLES)
        {
            int64_t actual_interval_ns =
                timespec_diff_ns(
                    &send_time,
                    &previous_send);


            int64_t interval_error_ns =
                actual_interval_ns -
                (int64_t)period_ns;


            send_samples[
                send_sample_count++]
                =
                interval_error_ns;
        }


        previous_send =
            send_time;


        previous_send_valid =
            true;


        /*
         * ----------------------------------------------------
         * Send actual EtherCAT datagrams.
         * ----------------------------------------------------
         */

        if (ecrt_master_send(
                master) != 0)
        {
            send_errors++;
        }


        /*
         * ----------------------------------------------------
         * Loop execution time.
         * ----------------------------------------------------
         */

        clock_gettime(
            CLOCK_MONOTONIC,
            &exec_end);


        int64_t exec_ns =
            timespec_diff_ns(
                &exec_end,
                &wake);


        if (exec_ns < 0)
            exec_ns = 0;


        /*
         * ----------------------------------------------------
         * Save cycle measurements.
         * ----------------------------------------------------
         */

        if (
            cycle >= warmup_cycles &&
            cycle_sample_count < MAX_SAMPLES)
        {
            wake_samples[
                cycle_sample_count]
                =
                wake_lateness_ns;


            exec_samples[
                cycle_sample_count]
                =
                (uint64_t)exec_ns;


            cycle_sample_count++;
        }


        /*
         * ----------------------------------------------------
         * Next schedule.
         * ----------------------------------------------------
         */

        if (rebase_schedule)
        {
            /*
             * Critical for high-frequency testing:
             *
             * Do NOT run old deadlines back-to-back.
             */
            target =
                wake;


            timespec_add_ns(
                &target,
                period_ns);
        }
        else
        {
            timespec_add_ns(
                &target,
                period_ns);
        }
    }


    /* ========================================================
     * Save counters
     * ======================================================== */

    result->cycle_samples =
        cycle_sample_count;

    result->send_samples =
        send_sample_count;

    result->rtt_samples =
        rtt_sample_count;


    result->late_events =
        late_events;

    result->skipped_cycles =
        skipped_cycles;


    result->receive_errors =
        receive_errors;

    result->send_errors =
        send_errors;


    result->reg_success =
        reg_success;

    result->reg_errors =
        reg_errors;

    result->reg_busy_cycles =
        reg_busy_cycles;


    /* ========================================================
     * Wake jitter
     * ======================================================== */

    if (cycle_sample_count > 0)
    {
        qsort(
            wake_samples,
            cycle_sample_count,
            sizeof(wake_samples[0]),
            cmp_i64);


        result->wake_p999_us =
            wake_samples[
                percentile_index(
                    cycle_sample_count,
                    0.999)]
            /
            1000.0;


        result->wake_max_us =
            wake_samples[
                cycle_sample_count - 1]
            /
            1000.0;
    }


    /* ========================================================
     * Send interval jitter
     *
     * Use ABSOLUTE interval error for percentile comparison.
     * ======================================================== */

    if (send_sample_count > 0)
    {
        result->send_max_us =
            max_abs_i64(
                send_samples,
                send_sample_count)
            /
            1000.0;


        for (size_t i = 0;
             i < send_sample_count;
             i++)
        {
            send_samples[i] =
                abs_i64_safe(
                    send_samples[i]);
        }


        qsort(
            send_samples,
            send_sample_count,
            sizeof(send_samples[0]),
            cmp_i64);


        result->send_p999_us =
            send_samples[
                percentile_index(
                    send_sample_count,
                    0.999)]
            /
            1000.0;
    }


    /* ========================================================
     * Execution time
     * ======================================================== */

    if (cycle_sample_count > 0)
    {
        qsort(
            exec_samples,
            cycle_sample_count,
            sizeof(exec_samples[0]),
            cmp_u64);


        result->exec_p999_us =
            exec_samples[
                percentile_index(
                    cycle_sample_count,
                    0.999)]
            /
            1000.0;


        result->exec_max_us =
            exec_samples[
                cycle_sample_count - 1]
            /
            1000.0;
    }


    /* ========================================================
     * ESC register RTT
     * ======================================================== */

    if (rtt_sample_count > 0)
    {
        qsort(
            rtt_samples,
            rtt_sample_count,
            sizeof(rtt_samples[0]),
            cmp_u64);


        result->rtt_p99_us =
            rtt_samples[
                percentile_index(
                    rtt_sample_count,
                    0.99)]
            /
            1000.0;


        result->rtt_p999_us =
            rtt_samples[
                percentile_index(
                    rtt_sample_count,
                    0.999)]
            /
            1000.0;


        result->rtt_max_us =
            rtt_samples[
                rtt_sample_count - 1]
            /
            1000.0;
    }


    return 0;
}


/* ============================================================
 * Result table
 * ============================================================ */

static void print_result_table(void)
{
    printf(
        "\n"
        "=================================================================================================================================\n");

    printf(
        " EtherCAT FreeRun ESC Register Benchmark - READ 0x%04X\n",
        ESC_REGISTER_ADDRESS);

    printf(
        "=================================================================================================================================\n");


    printf(
        "%-8s "
        "%8s "
        "%9s "
        "%9s "
        "%10s "
        "%9s "
        "%9s "
        "%10s "
        "%9s "
        "%8s "
        "%8s "
        "%8s\n",

        "Rate",
        "Period",
        "Wake999",
        "WakeMax",
        "Send999",
        "SendMax",
        "ExecMax",
        "RTT999",
        "RTTMax",
        "Late",
        "Skipped",
        "RegErr");


    printf(
        "%-8s "
        "%8s "
        "%9s "
        "%9s "
        "%10s "
        "%9s "
        "%9s "
        "%10s "
        "%9s "
        "%8s "
        "%8s "
        "%8s\n",

        "",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "(us)",
        "",
        "",
        "");


    printf(
        "---------------------------------------------------------------------------------------------------------------------------------\n");


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
            "%-8s "
            "%8.2f "
            "%9.2f "
            "%9.2f "
            "%10.2f "
            "%9.2f "
            "%9.2f ",

            freq,

            r->period_ns /
            1000.0,

            r->wake_p999_us,

            r->wake_max_us,

            r->send_p999_us,

            r->send_max_us,

            r->exec_max_us);


        if (r->rtt_samples > 0)
        {
            printf(
                "%10.2f "
                "%9.2f ",

                r->rtt_p999_us,
                r->rtt_max_us);
        }
        else
        {
            printf(
                "%10s "
                "%9s ",

                "-",
                "-");
        }


        printf(
            "%8" PRIu64 " "
            "%8" PRIu64 " "
            "%8" PRIu64 "\n",

            r->late_events,
            r->skipped_cycles,
            r->reg_errors);
    }


    printf(
        "=================================================================================================================================\n");


    printf(
        "\n"
        "Wake999 = P99.9 Linux wake-up lateness\n"
        "WakeMax = maximum Linux wake-up lateness\n"
        "Send999 = P99.9 absolute ecrt_master_send() interval error\n"
        "SendMax = maximum absolute ecrt_master_send() interval error\n"
        "ExecMax = maximum receive/register/send loop execution time\n"
        "RTT999  = P99.9 ESC register request round-trip time\n"
        "RTTMax  = maximum ESC register request round-trip time\n"
        "Late    = number of wake-ups >= one complete target period late\n"
        "Skipped = number of skipped schedule periods after large lateness\n"
        "RegErr  = ESC register request errors\n");
}


/* ============================================================
 * Register statistics
 * ============================================================ */

static void print_register_statistics(void)
{
    printf(
        "\n"
        "ESC register access statistics\n"
        "--------------------------------------------------------------------------------\n");


    printf(
        "%-8s "
        "%12s "
        "%12s "
        "%12s "
        "%12s\n",

        "Rate",
        "Reg OK",
        "Reg Error",
        "Busy cycles",
        "RTT samples");


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


        printf(
            "%-8s "
            "%12" PRIu64 " "
            "%12" PRIu64 " "
            "%12" PRIu64 " "
            "%12zu\n",

            freq,

            r->reg_success,
            r->reg_errors,
            r->reg_busy_cycles,
            r->rtt_samples);
    }


    printf(
        "--------------------------------------------------------------------------------\n");
}


/* ============================================================
 * Main
 * ============================================================ */

int main(void)
{
    signal(
        SIGINT,
        signal_handler);


    signal(
        SIGTERM,
        signal_handler);


    /* ========================================================
     * Allocate measurement buffers
     * ======================================================== */

    wake_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*wake_samples));


    send_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*send_samples));


    exec_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*exec_samples));


    rtt_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*rtt_samples));


    if (
        !wake_samples ||
        !send_samples ||
        !exec_samples ||
        !rtt_samples)
    {
        fprintf(
            stderr,
            "Memory allocation failed.\n");

        return EXIT_FAILURE;
    }


    /* ========================================================
     * Request EtherCAT master
     * ======================================================== */

    master =
        ecrt_request_master(
            MASTER_INDEX);


    if (!master)
    {
        fprintf(
            stderr,
            "Cannot request EtherCAT master %d.\n",
            MASTER_INDEX);

        return EXIT_FAILURE;
    }


    /* ========================================================
     * Detect slave 0 automatically
     * ======================================================== */

    ec_slave_info_t slave_info;


    if (wait_for_slave(
            &slave_info) != 0)
    {
        fprintf(
            stderr,
            "No EtherCAT slave found at position %u.\n",
            SLAVE_POSITION);


        ecrt_release_master(
            master);


        return EXIT_FAILURE;
    }


    /*
     * Create slave configuration using IDs read from the
     * actual slave.
     */
    slave_config =
        ecrt_master_slave_config(
            master,
            SLAVE_ALIAS,
            SLAVE_POSITION,
            slave_info.vendor_id,
            slave_info.product_code);


    if (!slave_config)
    {
        fprintf(
            stderr,
            "Cannot create slave configuration.\n");


        ecrt_release_master(
            master);


        return EXIT_FAILURE;
    }


    /*
     * Allocate a realtime register request.
     *
     * We only need 2 bytes for AL Status.
     */
    reg_request =
        ecrt_slave_config_create_reg_request(
            slave_config,
            ESC_REGISTER_SIZE);


    if (!reg_request)
    {
        fprintf(
            stderr,
            "Cannot create ESC register request.\n");


        ecrt_release_master(
            master);


        return EXIT_FAILURE;
    }


    /* ========================================================
     * Activate EtherCAT master
     * ======================================================== */

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
     * Give master a short stabilization interval.
     */
    usleep(
        500000);


    /* ========================================================
     * Lock memory
     * ======================================================== */

    if (mlockall(
            MCL_CURRENT |
            MCL_FUTURE) != 0)
    {
        perror(
            "mlockall");
    }


    /* ========================================================
     * CPU affinity
     * ======================================================== */

    int cpu =
        configure_cpu_affinity();


    /* ========================================================
     * Realtime scheduler
     * ======================================================== */

    if (configure_realtime() != 0)
    {
        perror(
            "sched_setscheduler");
    }


    /*
     * Minimal startup display.
     */
    printf(
        "Running EtherCAT FreeRun ESC benchmark\n");

    printf(
        "ESC 0x%04X / Slave %u / Vendor 0x%08X / Product 0x%08X",
        ESC_REGISTER_ADDRESS,
        SLAVE_POSITION,
        slave_info.vendor_id,
        slave_info.product_code);


    if (cpu >= 0)
    {
        printf(
            " / CPU %d",
            cpu);
    }


    printf(
        "\n\n");


    /* ========================================================
     * Frequency tests
     * ======================================================== */

    for (size_t i = 0;
         i < NUM_TEST_FREQS;
         i++)
    {
        if (stop_requested)
            break;


        clear_buffers();


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


        if (run_test(
                test_frequencies[i],
                &results[result_count]) != 0)
        {
            printf(
                "STOP\n");

            break;
        }


        result_count++;


        printf(
            "done\n");
    }


    /* ========================================================
     * Stop realtime scheduler before result formatting
     * ======================================================== */

    restore_scheduler();


    /* ========================================================
     * Results
     * ======================================================== */

    if (result_count > 0)
    {
        print_result_table();

        print_register_statistics();
    }


    if (stop_requested)
    {
        printf(
            "\nStopped by user.\n");
    }


    /* ========================================================
     * Cleanup
     * ======================================================== */

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
        wake_samples);

    free(
        send_samples);

    free(
        exec_samples);

    free(
        rtt_samples);


    return EXIT_SUCCESS;
}