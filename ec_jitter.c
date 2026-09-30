/*
 * ec_jitter.c
 *
 * IgH EtherCAT FreeRun Timing Benchmark
 *
 * No DC
 * No PDO
 * No external JSON
 *
 * Every cycle:
 *   - receive
 *   - ESC register read request (0x0130 AL Status)
 *   - send
 *
 * Main purpose:
 *   Compare timing limits of NIC / driver combinations such as:
 *
 *     Raspberry Pi onboard Ethernet + ec_generic
 *     Intel I210 + ec_igb
 *
 * Frequencies:
 *   1, 5, 10, 15, 20, 25, 50, 75, 100 kHz
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

#define SLAVE_ALIAS             0
#define SLAVE_POSITION          0

/*
 * ESC AL Status register.
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

#define MAX_TEST_HZ 100000

#define MAX_SAMPLES \
    ((size_t)MAX_TEST_HZ * TEST_SECONDS)


/* ============================================================
 * Result
 * ============================================================ */

typedef struct
{
    unsigned int hz;

    uint64_t period_ns;

    size_t measured_cycles;

    /*
     * Send interval jitter
     */
    double send_p999_us;
    double send_p999_percent;

    double send_max_us;
    double send_max_percent;

    /*
     * Execution time
     */
    double exec_max_us;
    double exec_max_percent;

    /*
     * Deadline miss / skip
     */
    uint64_t late_events;
    uint64_t skipped_cycles;

    double late_rate_ppm;
    double skipped_rate_ppm;

    double late_rate_percent;
    double skipped_rate_percent;

    /*
     * Worst burst
     */
    uint64_t max_consecutive_missed;

    /*
     * Communication errors
     */
    uint64_t receive_errors;
    uint64_t send_errors;
    uint64_t reg_errors;

} test_result_t;


/* ============================================================
 * Globals
 * ============================================================ */

static volatile sig_atomic_t stop_requested = 0;

static ec_master_t *master = NULL;

static ec_slave_config_t *slave_config = NULL;

static ec_reg_request_t *reg_request = NULL;


static int64_t *send_samples = NULL;
static uint64_t *exec_samples = NULL;


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
 * Formatting
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


static void format_rate(
    uint64_t count,
    uint64_t total,
    char *buffer,
    size_t size)
{
    if (total == 0)
    {
        snprintf(
            buffer,
            size,
            "-");

        return;
    }

    double percent =
        100.0 *
        (double)count /
        (double)total;

    double ppm =
        1000000.0 *
        (double)count /
        (double)total;

    /*
     * Use ppm below 0.1%.
     * Otherwise percent is easier to read.
     */
    if (percent < 0.1)
    {
        snprintf(
            buffer,
            size,
            "%.1f ppm",
            ppm);
    }
    else
    {
        snprintf(
            buffer,
            size,
            "%.3f%%",
            percent);
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

    int cpu =
        (int)cpu_count - 1;

    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);

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
 * Buffer reset
 * ============================================================ */

static void clear_buffers(void)
{
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
}


/* ============================================================
 * Wait for slave
 * ============================================================ */

static int wait_for_slave(
    ec_slave_info_t *slave_info)
{
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
 * One frequency test
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


    size_t send_sample_count = 0;
    size_t exec_sample_count = 0;


    uint64_t late_events = 0;
    uint64_t skipped_cycles = 0;

    uint64_t max_consecutive_missed = 0;

    uint64_t receive_errors = 0;
    uint64_t send_errors = 0;
    uint64_t reg_errors = 0;


    struct timespec target;
    struct timespec wake;

    struct timespec send_time;
    struct timespec previous_send_time;

    struct timespec exec_end;


    bool previous_send_valid = false;


    /*
     * Start 100 ms in future.
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


        /* ====================================================
         * Sleep
         * ==================================================== */

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


        /* ====================================================
         * Wake time
         * ==================================================== */

        clock_gettime(
            CLOCK_MONOTONIC,
            &wake);


        int64_t lateness_ns =
            timespec_diff_ns(
                &wake,
                &target);


        bool missed_deadline = false;
        uint64_t missed_this_event = 0;


        /*
         * Count only measurement section.
         */
        if (
            cycle >= warmup_cycles &&
            lateness_ns >=
            (int64_t)period_ns)
        {
            missed_deadline = true;

            late_events++;


            missed_this_event =
                (uint64_t)lateness_ns /
                period_ns;


            if (missed_this_event == 0)
                missed_this_event = 1;


            skipped_cycles +=
                missed_this_event;


            if (missed_this_event >
                max_consecutive_missed)
            {
                max_consecutive_missed =
                    missed_this_event;
            }
        }


        /* ====================================================
         * EtherCAT receive
         * ==================================================== */

        if (ecrt_master_receive(
                master) != 0)
        {
            receive_errors++;
        }


        /* ====================================================
         * ESC register request
         * ==================================================== */

        ec_request_state_t state =
            ecrt_reg_request_state(
                reg_request);


        if (state == EC_REQUEST_ERROR)
        {
            if (cycle >= warmup_cycles)
            {
                reg_errors++;
            }
        }


        /*
         * Queue new AL Status read whenever request is not busy.
         */
        if (state != EC_REQUEST_BUSY)
        {
            if (ecrt_reg_request_read(
                    reg_request,
                    ESC_REGISTER_ADDRESS,
                    ESC_REGISTER_SIZE) != 0)
            {
                if (cycle >= warmup_cycles)
                {
                    reg_errors++;
                }
            }
        }


        /* ====================================================
         * Send timestamp
         * ==================================================== */

        clock_gettime(
            CLOCK_MONOTONIC,
            &send_time);


        if (
            previous_send_valid &&
            cycle >= warmup_cycles &&
            send_sample_count <
            MAX_SAMPLES)
        {
            int64_t actual_interval_ns =
                timespec_diff_ns(
                    &send_time,
                    &previous_send_time);


            int64_t interval_error_ns =
                actual_interval_ns -
                (int64_t)period_ns;


            send_samples[
                send_sample_count++]
                =
                interval_error_ns;
        }


        previous_send_time =
            send_time;

        previous_send_valid =
            true;


        /* ====================================================
         * EtherCAT send
         * ==================================================== */

        if (ecrt_master_send(
                master) != 0)
        {
            receive_errors++;

            if (cycle >= warmup_cycles)
            {
                send_errors++;
            }
        }


        /* ====================================================
         * Execution time
         * ==================================================== */

        clock_gettime(
            CLOCK_MONOTONIC,
            &exec_end);


        int64_t exec_ns =
            timespec_diff_ns(
                &exec_end,
                &wake);


        if (exec_ns < 0)
            exec_ns = 0;


        if (
            cycle >= warmup_cycles &&
            exec_sample_count <
            MAX_SAMPLES)
        {
            exec_samples[
                exec_sample_count++]
                =
                (uint64_t)exec_ns;
        }


        /* ====================================================
         * Schedule next cycle
         * ==================================================== */

        if (missed_deadline)
        {
            /*
             * Important:
             *
             * Do not try to execute all old deadlines
             * back-to-back.
             *
             * Restart from a future deadline.
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
     * Basic counters
     * ======================================================== */

    result->measured_cycles =
        measurement_cycles;


    result->late_events =
        late_events;


    result->skipped_cycles =
        skipped_cycles;


    result->max_consecutive_missed =
        max_consecutive_missed;


    result->receive_errors =
        receive_errors;


    result->send_errors =
        send_errors;


    result->reg_errors =
        reg_errors;


    /* ========================================================
     * Rates
     * ======================================================== */

    if (measurement_cycles > 0)
    {
        result->late_rate_ppm =
            1000000.0 *
            (double)late_events /
            (double)measurement_cycles;


        result->skipped_rate_ppm =
            1000000.0 *
            (double)skipped_cycles /
            (double)measurement_cycles;


        result->late_rate_percent =
            100.0 *
            (double)late_events /
            (double)measurement_cycles;


        result->skipped_rate_percent =
            100.0 *
            (double)skipped_cycles /
            (double)measurement_cycles;
    }


    /* ========================================================
     * Send jitter
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


        double period_us =
            period_ns /
            1000.0;


        result->send_p999_percent =
            100.0 *
            result->send_p999_us /
            period_us;


        result->send_max_percent =
            100.0 *
            result->send_max_us /
            period_us;
    }


    /* ========================================================
     * Execution max
     * ======================================================== */

    if (exec_sample_count > 0)
    {
        qsort(
            exec_samples,
            exec_sample_count,
            sizeof(exec_samples[0]),
            cmp_u64);


        result->exec_max_us =
            exec_samples[
                exec_sample_count - 1]
            /
            1000.0;


        double period_us =
            period_ns /
            1000.0;


        result->exec_max_percent =
            100.0 *
            result->exec_max_us /
            period_us;
    }


    return 0;
}


/* ============================================================
 * Main result table
 * ============================================================ */

static void print_result_table(void)
{
    printf(
        "\n"
        "====================================================================================================================================================\n");

    printf(
        " EtherCAT FreeRun Timing Benchmark\n");

    printf(
        "====================================================================================================================================================\n");


    printf(
        "%-8s "
        "%8s "
        "%9s "
        "%7s "
        "%9s "
        "%7s "
        "%9s "
        "%7s "
        "%9s "
        "%11s "
        "%8s "
        "%11s "
        "%8s\n",

        "Rate",
        "Period",
        "Send999",
        "Jit%",
        "SendMax",
        "Max%",
        "ExecMax",
        "Exec%",
        "Skipped",
        "Miss rate",
        "Late",
        "Late rate",
        "Burst");


    printf(
        "%-8s "
        "%8s "
        "%9s "
        "%7s "
        "%9s "
        "%7s "
        "%9s "
        "%7s "
        "%9s "
        "%11s "
        "%8s "
        "%11s "
        "%8s\n",

        "",
        "(us)",
        "(us)",
        "",
        "(us)",
        "",
        "(us)",
        "",
        "(count)",
        "",
        "(count)",
        "",
        "(max)");


    printf(
        "----------------------------------------------------------------------------------------------------------------------------------------------------\n");


    for (size_t i = 0;
         i < result_count;
         i++)
    {
        const test_result_t *r =
            &results[i];


        char freq[32];
        char miss_rate[32];
        char late_rate[32];


        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        format_rate(
            r->skipped_cycles,
            r->measured_cycles,
            miss_rate,
            sizeof(miss_rate));


        format_rate(
            r->late_events,
            r->measured_cycles,
            late_rate,
            sizeof(late_rate));


        printf(
            "%-8s "
            "%8.2f "
            "%9.2f "
            "%6.1f%% "
            "%9.2f "
            "%6.1f%% "
            "%9.2f "
            "%6.1f%% "
            "%9" PRIu64 " "
            "%11s "
            "%8" PRIu64 " "
            "%11s "
            "%8" PRIu64 "\n",

            freq,

            r->period_ns /
            1000.0,

            r->send_p999_us,
            r->send_p999_percent,

            r->send_max_us,
            r->send_max_percent,

            r->exec_max_us,
            r->exec_max_percent,

            r->skipped_cycles,
            miss_rate,

            r->late_events,
            late_rate,

            r->max_consecutive_missed);
    }


    printf(
        "====================================================================================================================================================\n");


    printf(
        "\n"
        "Send999  = P99.9 absolute interval error between ecrt_master_send() calls\n"
        "Jit%%     = Send999 / target period\n"
        "SendMax  = maximum absolute send interval error\n"
        "Max%%     = SendMax / target period\n"
        "ExecMax  = maximum receive/register/send execution time\n"
        "Exec%%    = ExecMax / target period\n"
        "Skipped  = number of skipped target periods\n"
        "Miss rate= Skipped / measured cycles (ppm or %)\n"
        "Late     = number of deadline-late events\n"
        "Late rate= Late / measured cycles (ppm or %)\n"
        "Burst    = maximum number of periods missed in one late event\n");
}


/* ============================================================
 * Compact limit table
 * ============================================================ */

static void print_limit_table(void)
{
    printf(
        "\n"
        "====================================================================================================\n");

    printf(
        " Timing Limit Overview\n");

    printf(
        "====================================================================================================\n");


    printf(
        "%-8s "
        "%10s "
        "%10s "
        "%12s "
        "%12s "
        "%10s\n",

        "Rate",
        "Jitter%",
        "Exec%",
        "Skipped",
        "Miss rate",
        "Burst");


    printf(
        "----------------------------------------------------------------------------------------------------\n");


    for (size_t i = 0;
         i < result_count;
         i++)
    {
        const test_result_t *r =
            &results[i];


        char freq[32];
        char miss_rate[32];


        format_frequency(
            r->hz,
            freq,
            sizeof(freq));


        format_rate(
            r->skipped_cycles,
            r->measured_cycles,
            miss_rate,
            sizeof(miss_rate));


        printf(
            "%-8s "
            "%9.1f%% "
            "%9.1f%% "
            "%12" PRIu64 " "
            "%12s "
            "%10" PRIu64 "\n",

            freq,
            r->send_p999_percent,
            r->exec_max_percent,
            r->skipped_cycles,
            miss_rate,
            r->max_consecutive_missed);
    }


    printf(
        "====================================================================================================\n");
}


/* ============================================================
 * Error table
 * ============================================================ */

static void print_error_table(void)
{
    printf(
        "\n"
        "EtherCAT errors\n");

    printf(
        "--------------------------------------------------------------------------------\n");

    printf(
        "%-8s "
        "%12s "
        "%12s "
        "%12s\n",

        "Rate",
        "RX Error",
        "TX Error",
        "Reg Error");


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
            "%12" PRIu64 "\n",

            freq,
            r->receive_errors,
            r->send_errors,
            r->reg_errors);
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
     * Allocate
     * ======================================================== */

    send_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*send_samples));


    exec_samples =
        calloc(
            MAX_SAMPLES,
            sizeof(*exec_samples));


    if (
        !send_samples ||
        !exec_samples)
    {
        fprintf(
            stderr,
            "Memory allocation failed.\n");

        return EXIT_FAILURE;
    }


    /* ========================================================
     * Request master
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
     * Detect slave
     * ======================================================== */

    ec_slave_info_t slave_info;


    if (wait_for_slave(
            &slave_info) != 0)
    {
        fprintf(
            stderr,
            "No slave found at position %u.\n",
            SLAVE_POSITION);


        ecrt_release_master(
            master);


        return EXIT_FAILURE;
    }


    /* ========================================================
     * Slave config
     * ======================================================== */

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


    /* ========================================================
     * ESC register request
     * ======================================================== */

    reg_request =
        ecrt_slave_config_create_reg_request(
            slave_config,
            ESC_REGISTER_SIZE);


    if (!reg_request)
    {
        fprintf(
            stderr,
            "Cannot create register request.\n");


        ecrt_release_master(
            master);


        return EXIT_FAILURE;
    }


    /* ========================================================
     * Activate
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


    usleep(
        500000);


    /* ========================================================
     * Realtime preparation
     * ======================================================== */

    if (mlockall(
            MCL_CURRENT |
            MCL_FUTURE) != 0)
    {
        perror(
            "mlockall");
    }


    int cpu =
        configure_cpu_affinity();


    if (configure_realtime() != 0)
    {
        perror(
            "sched_setscheduler");
    }


    /* ========================================================
     * Display
     * ======================================================== */

    printf(
        "Running EtherCAT FreeRun benchmark\n");

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
     * Run tests
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
     * Results
     * ======================================================== */

    restore_scheduler();


    if (result_count > 0)
    {
        print_result_table();

        print_limit_table();

        print_error_table();
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
        send_samples);

    free(
        exec_samples);


    return EXIT_SUCCESS;
}