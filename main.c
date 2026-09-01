#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <mach/mach.h> //give us access to Mach APIs.
#include <mach/mach_host.h> //give us access to Mach APIs.
#include <mach/mach_time.h>
#include <unistd.h> //gives us sleep() and usleep()
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <errno.h>
#include <libproc.h>
#include <signal.h>


#define MAX_PROCESSES 4096 //reasonable maximum size for our PID array.
//enough space to store information of abt up to 4096 process
//having more than 4096 simultaneously running processes is unlikely

#define DEFAULT_SAMPLE_INTERVAL 1.0
//default amount of time between monitor updates

#define DEFAULT_TOP_PROCESSES 10
//default number of processes displayed in each process table

//can use mach timebase to convert the process CPU time units into nanoseconds


/*
 * Get system-wide CPU tick information.
 */
void get_cpu_ticks(host_cpu_load_info_data_t *cpu_info)
{
    mach_msg_type_number_t count =
        HOST_CPU_LOAD_INFO_COUNT; //tells the Mach API how much CPU information we're working with.

    kern_return_t result =
        host_statistics64(
            mach_host_self(), //Mach port/handle referring to the current host.
            HOST_CPU_LOAD_INFO,
            (host_info64_t)cpu_info, //The function expects a particular pointer type. our structure's pointer is different, but compatible. so we explicitly convert it to the type the function expects
            &count
        );

    if (result != KERN_SUCCESS)
    {
        fprintf(
            stderr,
            "host_statistics64 failed: %d\n",
            result
        );
    }
}


/*
 * Calculate overall CPU usage.
 */
double calculate_cpu_usage(
    host_cpu_load_info_data_t *first,
    host_cpu_load_info_data_t *second
)
{
    uint64_t user_delta =
        second->cpu_ticks[CPU_STATE_USER] -
        first->cpu_ticks[CPU_STATE_USER];

    uint64_t system_delta =
        second->cpu_ticks[CPU_STATE_SYSTEM] -
        first->cpu_ticks[CPU_STATE_SYSTEM];

    uint64_t idle_delta =
        second->cpu_ticks[CPU_STATE_IDLE] -
        first->cpu_ticks[CPU_STATE_IDLE];

    uint64_t nice_delta =
        second->cpu_ticks[CPU_STATE_NICE] -
        first->cpu_ticks[CPU_STATE_NICE];

    uint64_t total_delta =
        user_delta +
        system_delta +
        idle_delta +
        nice_delta;

    uint64_t busy_delta =
        user_delta +
        system_delta +
        nice_delta;

    if (total_delta == 0)
    {
        return 0.0;
    }

    return
        ((double)busy_delta /
         (double)total_delta) * 100.0;

    //we use double as busy_delta/total_delta are both integers. integer division can produce 0 instead of 0.6
    //double treats it as a floating point number
}


/*
 * Convert bytes to gigabytes.
 */
double bytes_to_gb(uint64_t bytes) //converts bytes to gigabytes - divide by 10^9
{
    return
        (double)bytes /
        (1024.0 * 1024.0 * 1024.0);
}


/*
 * Convert bytes to megabytes.
 */
double bytes_to_mb(uint64_t bytes) //converts bytes to megabytes - divide by 10^6
{
    return
        (double)bytes /
        (1024.0 * 1024.0);
}


/*
 * Memory information.
 */
typedef struct
{
    uint64_t total;
    uint64_t used;
    uint64_t free;
    uint64_t active;
    uint64_t inactive;
    uint64_t wired;

} MemoryInfo;


/*
 * Get current memory statistics.
 */
int get_memory_info(MemoryInfo *memory_info)
{
    vm_statistics64_data_t vm_stat;

    mach_msg_type_number_t count =
        HOST_VM_INFO64_COUNT;

    mach_port_t host =
        mach_host_self();

    kern_return_t result =
        host_statistics64(
            host,
            HOST_VM_INFO64,
            (host_info64_t)&vm_stat,
            &count
        );

    if (result != KERN_SUCCESS)
    {
        fprintf(
            stderr,
            "host_statistics64 for memory failed: %d\n",
            result
        );

        return -1;
    }


    /*
     * Get memory page size.
     */
    vm_size_t page_size;

    result =
        host_page_size(
            host,
            &page_size
        );

    if (result != KERN_SUCCESS)
    {
        fprintf(
            stderr,
            "host_page_size failed: %d\n",
            result
        );

        return -1;
    }


    /*
     * Convert pages into bytes.
     */
    uint64_t free_pages =
        (uint64_t)vm_stat.free_count -
        (uint64_t)vm_stat.speculative_count;

    uint64_t active_pages =
        (uint64_t)vm_stat.active_count;

    uint64_t inactive_pages =
        (uint64_t)vm_stat.inactive_count;

    uint64_t wired_pages =
        (uint64_t)vm_stat.wire_count;


    memory_info->free =
        free_pages *
        (uint64_t)page_size;

    memory_info->active =
        active_pages *
        (uint64_t)page_size;

    memory_info->inactive =
        inactive_pages *
        (uint64_t)page_size;

    memory_info->wired =
        wired_pages *
        (uint64_t)page_size;


    /*
     * Calculate total and used memory.
     *
     * Total memory comes from hw.memsize, which is the
     * same value we already retrieved in main().
     *
     * For this project, we treat inactive memory as
     * reclaimable memory, so used memory is:
     *
     * total - free - inactive
     */


    /*
     * Get total physical memory.
     */
    uint64_t total_memory;

    size_t size =
        sizeof(total_memory);

    if (
        sysctlbyname(
            "hw.memsize",
            &total_memory,
            &size,
            NULL,
            0
        ) == -1
    )
    {
        perror("hw.memsize");
        return -1;
    }

    memory_info->total =
        total_memory;


    /*
     * Treat inactive memory as reclaimable.
     */
    memory_info->used =
        memory_info->total -
        memory_info->free -
        memory_info->inactive;

    return 0;
}


/*
 * Disk information.
 *
 * Disk statistics are collected for
 * the root filesystem "/".
 */
typedef struct
{
    uint64_t total;
    uint64_t used;
    uint64_t free;

} DiskInfo;


/*
 * Get current disk statistics.
 *
 * statfs() asks macOS for information about
 * the filesystem containing the specified path.
 *
 * We use "/" so the monitor reports the
 * main filesystem.
 */
int get_disk_info(DiskInfo *disk_info)
{
    struct statfs filesystem_info;

    if (
        statfs(
            "/",
            &filesystem_info
        ) == -1
    )
    {
        perror("statfs");
        return -1;
    }


    /*
     * Calculate total disk space.
     */
    uint64_t block_size =
        (uint64_t)filesystem_info.f_bsize;

    uint64_t total_blocks =
        (uint64_t)filesystem_info.f_blocks;

    uint64_t free_blocks =
        (uint64_t)filesystem_info.f_bavail;


    disk_info->total =
        total_blocks *
        block_size;

    disk_info->free =
        free_blocks *
        block_size;


    /*
     * Used disk space is:
     *
     * total - free
     */
    if (disk_info->free <= disk_info->total)
    {
        disk_info->used =
            disk_info->total -
            disk_info->free;
    }
    else
    {
        disk_info->used = 0;
    }

    return 0;
}


/*
 * Uptime information.
 */
typedef struct
{
    uint64_t seconds;

} UptimeInfo;


/*
 * Get system uptime.
 *
 * macOS exposes the boot time through
 * the kern.boottime sysctl value.
 */
int get_uptime(UptimeInfo *uptime_info)
{
    struct timeval boot_time;

    size_t size =
        sizeof(boot_time);

    if (
        sysctlbyname(
            "kern.boottime",
            &boot_time,
            &size,
            NULL,
            0
        ) == -1
    )
    {
        perror("kern.boottime");
        return -1;
    }


    /*
     * Get the current wall-clock time.
     */
    struct timeval current_time;

    if (
        gettimeofday(
            &current_time,
            NULL
        ) == -1
    )
    {
        perror("gettimeofday");
        return -1;
    }


    /*
     * Calculate the difference between
     * current time and boot time.
     */
    if (
        current_time.tv_sec >=
        boot_time.tv_sec
    )
    {
        uptime_info->seconds =
            (uint64_t)(
                current_time.tv_sec -
                boot_time.tv_sec
            );
    }
    else
    {
        uptime_info->seconds = 0;
    }

    return 0;
}


/*
 * Print uptime in a human-readable format.
 */
void print_uptime(
    uint64_t seconds
)
{
    uint64_t days =
        seconds / 86400;

    seconds %= 86400;

    uint64_t hours =
        seconds / 3600;

    seconds %= 3600;

    uint64_t minutes =
        seconds / 60;

    seconds %= 60;


    printf(
        "Uptime: %llu days, %llu:%02llu:%02llu\n",
        (unsigned long long)days,
        (unsigned long long)hours,
        (unsigned long long)minutes,
        (unsigned long long)seconds
    );
}


/*
 * Load average information.
 */
typedef struct
{
    double one_minute;
    double five_minutes;
    double fifteen_minutes;

} LoadAverage;


/*
 * Get system load averages.
 *
 * getloadavg() returns the number of
 * runnable processes averaged over
 * 1, 5, and 15 minutes.
 */
int get_load_average(
    LoadAverage *load_average
)
{
    double loads[3];

    int result =
        getloadavg(
            loads,
            3
        );

    if (result != 3)
    {
        fprintf(
            stderr,
            "getloadavg failed\n"
        );

        return -1;
    }


    load_average->one_minute =
        loads[0];

    load_average->five_minutes =
        loads[1];

    load_average->fifteen_minutes =
        loads[2];

    return 0;
}


/*
 * Network information.
 */
typedef struct
{
    uint64_t received_bytes;
    uint64_t transmitted_bytes;

} NetworkInfo;


/*
 * Get network statistics.
 *
 * We use getifaddrs() to examine each
 * network interface.
 *
 * ifi_ibytes = bytes received
 * ifi_obytes = bytes transmitted
 */
int get_network_info(
    NetworkInfo *network_info
)
{
    network_info->received_bytes = 0;
    network_info->transmitted_bytes = 0;


    struct ifaddrs *interfaces = NULL;

    if (
        getifaddrs(
            &interfaces
        ) == -1
    )
    {
        perror("getifaddrs");
        return -1;
    }


    /*
     * Walk through every network interface.
     */
    for (
        struct ifaddrs *interface =
            interfaces;

        interface != NULL;

        interface =
            interface->ifa_next
    )
    {
        if (
            interface->ifa_data == NULL
        )
        {
            continue;
        }


        /*
         * We only want link-layer
         * interface statistics.
         */
        if (
            interface->ifa_addr == NULL
        )
        {
            continue;
        }


        if (
            interface->ifa_addr->sa_family !=
            AF_LINK
        )
        {
            continue;
        }


        struct if_data *interface_data =
            (struct if_data *)
            interface->ifa_data;


        network_info->received_bytes +=
            (uint64_t)interface_data->ifi_ibytes;

        network_info->transmitted_bytes +=
            (uint64_t)interface_data->ifi_obytes;
    }


    freeifaddrs(
        interfaces
    );

    return 0;
}


/*
 * Information about a process.
 */
typedef struct
{
    pid_t pid;

    char name[256];

    uint64_t memory;

    uint64_t cpu_time;

    double cpu_percent;

} Process;


/*
 * Get a snapshot of running processes.
 */
int get_process_snapshot(
    Process processes[],
    int max_processes
)
{
    pid_t pids[MAX_PROCESSES];

    int count =
        proc_listallpids(
            pids,
            sizeof(pids)
        );

    //retrieves list of all active pids

    if (count == -1)
    {
        fprintf(
            stderr,
            "proc_listallpids failed\n"
        );

        return -1;
    }

    int process_count = 0;


    for (
        int i = 0;
        i < count &&
        process_count < max_processes;
        i++
    )
    {
        if (pids[i] <= 0)
        {
            continue;
        }


        /*
         * Get process name.
         */
        char name[256];

        int name_length =
            proc_name(
                pids[i],
                name,
                sizeof(name)
            );

        if (name_length <= 0)
        {
            snprintf(
                name,
                sizeof(name),
                "unknown"
            );
        }


        //get information about this process

        /*
         * Get process information.
         */
        struct proc_taskinfo task_info;

        int result =
            proc_pidinfo(
                pids[i],
                PROC_PIDTASKINFO,
                0,
                &task_info,
                sizeof(task_info)
            );

        //the process may have exited between getting the PID list and asking for its information
        //it is also possible that we dont have access to it
        //if we cant get the information, skip the process

        /*
         * Process may have exited or
         * information may be unavailable.
         */
        if (
            result !=
            sizeof(task_info)
        )
        {
            continue;
        }


        Process process;

        process.pid =
            pids[i];


        snprintf(
            process.name,
            sizeof(process.name),
            "%s",
            name
        );


        /*
         * Resident memory.
         */
        process.memory =
            task_info.pti_resident_size;


        /*
         * Cumulative CPU time.
         *
         * These counters are expressed
         * in nanoseconds.
         */
        process.cpu_time =
            task_info.pti_total_user +
            task_info.pti_total_system;

        //this is cummulative cpu time
        //pti_total_user and pti_total_system are CPU time counters
        //but the counters are not expressed directly in seconds
        //units of pti are not the same as CPU seconds


        process.cpu_percent =
            0.0;


        processes[process_count] =
            process;

        process_count++;
    }

    return process_count;
}


/*
 * Find a process by PID.
 */
//this function tells us where is PID X in this process array
int find_process(
    Process processes[],
    int process_count,
    pid_t pid
)
{
    for (
        int i = 0;
        i < process_count;
        i++
    )
    {
        if (
            processes[i].pid ==
            pid
        )
        {
            return i; //return its array position
        }
    }

    return -1; //if the process pid doesnt exist
}


/*
 * Sort processes by CPU usage.
 * Highest first.
 */
int compare_processes_by_cpu(
    const void *a,
    const void *b
)
{
    const Process *process_a =
        (const Process *)a;

    const Process *process_b =
        (const Process *)b;

    if (
        process_a->cpu_percent <
        process_b->cpu_percent
    )
    {
        return 1;
    }

    if (
        process_a->cpu_percent >
        process_b->cpu_percent
    )
    {
        return -1;
    }

    return 0;
}


/*
 * Sort processes by memory usage.
 * Highest first.
 */
int compare_processes_by_memory(
    const void *a,
    const void *b
)
{
    const Process *process_a =
        (const Process *)a;

    const Process *process_b =
        (const Process *)b;

    if (
        process_a->memory <
        process_b->memory
    )
    {
        return 1;
    }

    if (
        process_a->memory >
        process_b->memory
    )
    {
        return -1;
    }

    return 0;
}

//qsort() needs a function that tells it which of the 2 elements should come first
//this comparison sorts processes from highest memory usage to lowest memory usage


/*
 * Print a simple percentage bar.
 *
 * This makes the monitor easier to read
 * than showing percentages alone.
 */
void print_usage_bar(
    double percentage,
    int width
)
{
    if (percentage < 0.0)
    {
        percentage = 0.0;
    }

    if (percentage > 100.0)
    {
        percentage = 100.0;
    }


    int filled =
        (int)(
            (percentage / 100.0) *
            width
        );


    printf("[");

    for (
        int i = 0;
        i < width;
        i++
    )
    {
        if (i < filled)
        {
            printf("#");
        }
        else
        {
            printf(" ");
        }
    }

    printf("]");
}

/*
 * Print the monitor screen.
 *
 * Instead of repeatedly printing pieces of the
 * monitor directly to the terminal, we first build
 * the complete screen in memory.
 *
 * This reduces visible flickering because the terminal
 * receives the monitor output as one completed frame.
 */
void print_monitor(
    int cpu_count,
    double cpu_usage,
    MemoryInfo *memory_info,
    DiskInfo *disk_info,
    UptimeInfo *uptime_info,
    LoadAverage *load_average,
    NetworkInfo *network_info,
    Process processes[],
    int process_count,
    int top_processes,
    double sample_interval
)
{
    /*
     * Move cursor to the top of the terminal.
     *
     * \033[H = move cursor to home position.
     */
    printf("\033[H");

    /*
     * Clear everything below the cursor.
     *
     * This removes the previous monitor frame.
     */
    printf("\033[J");

    /*
     * Start building the complete monitor frame.
     *
     * The monitor is still printed section by section,
     * but the terminal output is flushed only after the
     * complete frame has been generated.
     */

    printf(
        "============================================\n"
    );

    printf(
        "              SYSTEM MONITOR                \n"
    );

    printf(
        "============================================\n\n"
    );


    /*
     * CPU information.
     */
    printf(
        "CPU\n"
    );

    printf(
        "--------------------------------------------\n"
    );

    printf(
        "Logical CPUs: %d\n",
        cpu_count
    );

    printf(
        "CPU Usage:    %6.2f%% ",
        cpu_usage
    );

    print_usage_bar(
        cpu_usage,
        25
    );

    printf(
        "\n\n"
    );


    /*
     * Memory information.
     */
    printf(
        "MEMORY\n"
    );

    printf(
        "--------------------------------------------\n"
    );

    printf(
        "Total memory:    %8.2f GB\n",
        bytes_to_gb(
            memory_info->total
        )
    );

    printf(
        "Used memory:     %8.2f GB\n",
        bytes_to_gb(
            memory_info->used
        )
    );

    printf(
        "Free memory:     %8.2f GB\n",
        bytes_to_gb(
            memory_info->free
        )
    );

    printf(
        "Active memory:   %8.2f GB\n",
        bytes_to_gb(
            memory_info->active
        )
    );

    printf(
        "Inactive memory: %8.2f GB\n",
        bytes_to_gb(
            memory_info->inactive
        )
    );

    printf(
        "Wired memory:    %8.2f GB\n",
        bytes_to_gb(
            memory_info->wired
        )
    );


    /*
     * Memory percentage.
     */
    double memory_usage =
        0.0;

    if (
        memory_info->total > 0
    )
    {
        memory_usage =
            (
                (double)memory_info->used /
                (double)memory_info->total
            ) *
            100.0;
    }

    printf(
        "Memory Usage:    %6.2f%% ",
        memory_usage
    );

    print_usage_bar(
        memory_usage,
        25
    );

    printf(
        "\n\n"
    );


    /*
     * Disk information.
     */
    printf(
        "DISK\n"
    );

    printf(
        "--------------------------------------------\n"
    );

    printf(
        "Total disk:      %8.2f GB\n",
        bytes_to_gb(
            disk_info->total
        )
    );

    printf(
        "Used disk:       %8.2f GB\n",
        bytes_to_gb(
            disk_info->used
        )
    );

    printf(
        "Free disk:       %8.2f GB\n",
        bytes_to_gb(
            disk_info->free
        )
    );


    double disk_usage =
        0.0;

    if (
        disk_info->total > 0
    )
    {
        disk_usage =
            (
                (double)disk_info->used /
                (double)disk_info->total
            ) *
            100.0;
    }

    printf(
        "Disk Usage:      %6.2f%% ",
        disk_usage
    );

    print_usage_bar(
        disk_usage,
        25
    );

    printf(
        "\n\n"
    );


    /*
     * System information.
     */
    printf(
        "SYSTEM\n"
    );

    printf(
        "--------------------------------------------\n"
    );

    print_uptime(
        uptime_info->seconds
    );

    printf(
        "Load average:    %.2f  %.2f  %.2f\n\n",
        load_average->one_minute,
        load_average->five_minutes,
        load_average->fifteen_minutes
    );


    /*
     * Network information.
     */
    printf(
        "NETWORK\n"
    );

    printf(
        "--------------------------------------------\n"
    );

    printf(
        "Received:        %10.2f MB\n",
        bytes_to_mb(
            network_info->received_bytes
        )
    );

    printf(
        "Transmitted:     %10.2f MB\n\n",
        bytes_to_mb(
            network_info->transmitted_bytes
        )
    );


    /*
     * TOP processes by CPU.
     */
    printf(
        "TOP %d PROCESSES BY CPU\n",
        top_processes
    );

    printf(
        "============================================\n"
    );


    int processes_to_print =
        process_count;

    if (
        processes_to_print >
        top_processes
    )
    {
        processes_to_print =
            top_processes;
    }


    for (
        int i = 0;
        i < processes_to_print;
        i++
    )
    {
        printf(
            "%-6d %-30s CPU: %6.2f%% "
            "Memory: %8.2f MB\n",

            processes[i].pid,

            processes[i].name,

            processes[i].cpu_percent,

            bytes_to_mb(
                processes[i].memory
            )
        );
    }


    /*
     * Create a separate copy of the
     * process list for memory sorting.
     */
    Process memory_sorted[
        MAX_PROCESSES
    ];


    for (
        int i = 0;
        i < process_count;
        i++
    )
    {
        memory_sorted[i] =
            processes[i];
    }


    /*
     * Sort the copy by memory usage.
     */
    qsort(
        memory_sorted,
        process_count,
        sizeof(Process),
        compare_processes_by_memory
    );


    printf(
        "\n"
    );


    /*
     * TOP processes by memory.
     */
    printf(
        "TOP %d PROCESSES BY MEMORY\n",
        top_processes
    );

    printf(
        "============================================\n"
    );


    int memory_processes_to_print =
        process_count;

    if (
        memory_processes_to_print >
        top_processes
    )
    {
        memory_processes_to_print =
            top_processes;
    }


    for (
        int i = 0;
        i < memory_processes_to_print;
        i++
    )
    {
        printf(
            "%-6d %-30s Memory: %8.2f MB "
            "CPU: %6.2f%%\n",

            memory_sorted[i].pid,

            memory_sorted[i].name,

            bytes_to_mb(
                memory_sorted[i].memory
            ),

            memory_sorted[i].cpu_percent
        );
    }


    printf(
        "\n"
    );


    /*
     * Display refresh configuration.
     */
    printf(
        "Refresh interval: %.2f seconds\n",
        sample_interval
    );

    printf(
        "Press Ctrl+C to stop.\n"
    );


    /*
     * Make sure everything currently buffered
     * is sent to the terminal immediately.
     */
    fflush(stdout);
}



/*
 * Print program usage instructions.
 */
void print_usage(
    const char *program_name
)
{
    printf(
        "Usage: %s [options]\n\n",
        program_name
    );

    printf(
        "Options:\n"
    );

    printf(
        "  -i, --interval SECONDS   Set refresh interval\n"
    );

    printf(
        "  -t, --top NUMBER         Number of processes to show\n"
    );

    printf(
        "  -h, --help               Show this help message\n"
    );

    printf(
        "\nExamples:\n"
    );

    printf(
        "  %s\n",
        program_name
    );

    printf(
        "  %s --interval 2\n",
        program_name
    );

    printf(
        "  %s --top 20\n",
        program_name
    );

    printf(
        "  %s --interval 2 --top 20\n",
        program_name
    );
}

/*
 * Restore the terminal before exiting.
 *
 * We leave the alternate screen,
 * show the cursor again,
 * and return the terminal to its
 * normal state.
 */
void restore_terminal(void)
{
    /*
     * \033[?25h = show cursor
     * \033[?1049l = leave alternate screen
     */
    printf(
        "\033[?25h"
        "\033[?1049l"
    );

    fflush(stdout);
}


/*
 * Handle Ctrl+C.
 *
 * SIGINT is generated when the user
 * presses Ctrl+C in the terminal.
 */
void handle_signal(int signal_number)
{
    (void)signal_number;

    restore_terminal();

    exit(0);
}


/*
 * Main program.
 */
int main(
    int argc,
    char *argv[]
)
{
    /*
     * Configuration values.
     */
    double sample_interval =
        DEFAULT_SAMPLE_INTERVAL;

    int top_processes =
        DEFAULT_TOP_PROCESSES;

    /*
    * Register Ctrl+C handler.
    *
    * This allows us to restore the terminal
    * before the program exits.
    */
    signal(
        SIGINT,
        handle_signal
    );

    /*
     * Command-line options.
     */
    static struct option long_options[] =
    {
        {
            "interval",
            required_argument,
            NULL,
            'i'
        },

        {
            "top",
            required_argument,
            NULL,
            't'
        },

        {
            "help",
            no_argument,
            NULL,
            'h'
        },

        {
            NULL,
            0,
            NULL,
            0
        }
    };


    int option;

    while (
        (
            option =
                getopt_long(
                    argc,
                    argv,
                    "i:t:h",
                    long_options,
                    NULL
                )
        ) != -1
    )
    {
        switch (option)
        {
            case 'i':
            {
                sample_interval =
                    atof(optarg);

                if (
                    sample_interval <= 0.0
                )
                {
                    fprintf(
                        stderr,
                        "Interval must be greater than 0.\n"
                    );

                    return 1;
                }

                break;
            }


            case 't':
            {
                top_processes =
                    atoi(optarg);

                if (
                    top_processes <= 0
                )
                {
                    fprintf(
                        stderr,
                        "Top process count must be greater than 0.\n"
                    );

                    return 1;
                }


                if (
                    top_processes >
                    MAX_PROCESSES
                )
                {
                    top_processes =
                        MAX_PROCESSES;
                }

                break;
            }


            case 'h':
            {
                print_usage(
                    argv[0]
                );

                return 0;
            }


            default:
            {
                print_usage(
                    argv[0]
                );

                return 1;
            }
        }
    }


    /*
     * Get number of logical CPUs.
     */
    int cpu_count;

    size_t size =
        sizeof(cpu_count);

    //sysctlbyname asks macOS for number of logical CPUs

    if (
        sysctlbyname(
            "hw.logicalcpu",
            &cpu_count,
            &size,
            NULL,
            0
        ) == -1
    )
    {
        perror("sysctlbyname");
        return 1;
    }


    /*
    * Start the monitor in the terminal's
    * alternate screen.
    *
    * This gives the monitor its own screen
    * instead of overwriting the user's normal
    * terminal history.
    *
    * \033[?1049h = enter alternate screen
    * \033[H     = move cursor to home position
    * \033[?25l  = hide cursor
    */
    printf(
        "\033[?1049h"
    );

    printf(
        "\033[H"
    );

    printf(
        "\033[?25l"
    );

    fflush(stdout);


    /*
     * Continuously update monitor.
     */
    while (1)
    {
        /*
         * CPU MONITOR
         */

        /*
         * First CPU snapshot.
         */
        host_cpu_load_info_data_t first_cpu;

        get_cpu_ticks(
            &first_cpu
        );


        /*
         * First process snapshot.
         */
        Process first_snapshot[
            MAX_PROCESSES
        ];

        int first_count =
            get_process_snapshot(
                first_snapshot,
                MAX_PROCESSES
            );

        if (
            first_count == -1
        )
        {
            return 1;
        }


        /*
         * Wait for the configured
         * sampling interval.
         */
        usleep(
            (useconds_t)(
                sample_interval *
                1000000.0
            )
        );


        /*
         * Second CPU snapshot.
         */
        host_cpu_load_info_data_t second_cpu;

        get_cpu_ticks(
            &second_cpu
        );


        /*
         * Calculate overall CPU usage.
         */
        double cpu_usage =
            calculate_cpu_usage(
                &first_cpu,
                &second_cpu
            );


        /*
         * Second process snapshot.
         */
        Process second_snapshot[
            MAX_PROCESSES
        ];

        int second_count =
            get_process_snapshot(
                second_snapshot,
                MAX_PROCESSES
            );

        if (
            second_count == -1
        )
        {
            return 1;
        }


        /*
         * Calculate CPU percentage
         * for every process.
         */

        //have to match processes by PID
        //cant calculate a process' CPU usage if we cant find a match (no earlier/later measurement)

        for (
            int i = 0;
            i < second_count;
            i++
        )
        {
            //for every process in the 2nd snapshot

            int first_index =
                find_process(
                    first_snapshot,
                    first_count,
                    second_snapshot[i].pid
                );

            //we find the same PID in the first snapshot


            /*
             * Process did not exist
             * in the first snapshot.
             */
            if (
                first_index == -1
            )
            {
                continue;
            }


            uint64_t first_cpu_time =
                first_snapshot[
                    first_index
                ].cpu_time;

            //get the CPU time from one second ago


            uint64_t second_cpu_time =
                second_snapshot[
                    i
                ].cpu_time;

            //gets the current CPU time


            uint64_t cpu_time_delta =
                second_cpu_time -
                first_cpu_time;

            //calculates CPU time consumed during the interval


            //calculate the % of total CPU capacity used by this process during the sample interval

            //the machine has multiple logical CPUs, so the total CPU capacity is
            //number of logical CPUs * sample interval


            /*
             * CPU counters are nanoseconds.
             */
            double process_cpu_seconds =
                (double)cpu_time_delta /
                1000000000.0;

            //store the calculated CPU % in our process structure


            double cpu_percent =
                (
                    process_cpu_seconds /
                    sample_interval
                ) *
                100.0;

            //a process can use more than 100% CPU when it has multiple threads running on multiple CPU cores at the same time
            //store the calculated CPU % in our process structure


            second_snapshot[i].cpu_percent =
                cpu_percent;
        }


        /*
         * Sort main process array
         * by CPU.
         */
        qsort(
            second_snapshot,
            second_count,
            sizeof(Process),
            compare_processes_by_cpu
        );


        /*
         * Get current memory information.
         */
        MemoryInfo memory_info;

        if (
            get_memory_info(
                &memory_info
            ) == -1
        )
        {
            return 1;
        }


        /*
         * Get current disk information.
         */
        DiskInfo disk_info;

        if (
            get_disk_info(
                &disk_info
            ) == -1
        )
        {
            return 1;
        }


        /*
         * Get current uptime.
         */
        UptimeInfo uptime_info;

        if (
            get_uptime(
                &uptime_info
            ) == -1
        )
        {
            return 1;
        }


        /*
         * Get current load average.
         */
        LoadAverage load_average;

        if (
            get_load_average(
                &load_average
            ) == -1
        )
        {
            return 1;
        }


        /*
         * Get current network statistics.
         */
        NetworkInfo network_info;

        if (
            get_network_info(
                &network_info
            ) == -1
        )
        {
            return 1;
        }


        /*
         * Display monitor.
         */
        print_monitor(
            cpu_count,
            cpu_usage,
            &memory_info,
            &disk_info,
            &uptime_info,
            &load_average,
            &network_info,
            second_snapshot,
            second_count,
            top_processes,
            sample_interval
        );
    }


    return 0;
}