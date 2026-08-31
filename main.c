#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <libproc.h>

#define MAX_PROCESSES 4096
#define SAMPLE_INTERVAL 1

/*
 * Get system-wide CPU tick information.
 */
void get_cpu_ticks(host_cpu_load_info_data_t *cpu_info)
{
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    kern_return_t result = host_statistics64(
        mach_host_self(),
        HOST_CPU_LOAD_INFO,
        (host_info64_t)cpu_info,
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
 * Calculate CPU usage using two CPU snapshots.
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

    return ((double)busy_delta / (double)total_delta) * 100.0;
}


/*
 * Convert bytes to gigabytes.
 */
double bytes_to_gb(uint64_t bytes)
{
    return (double)bytes /
           (1024.0 * 1024.0 * 1024.0);
}


/*
 * Convert bytes to megabytes.
 */
double bytes_to_mb(uint64_t bytes)
{
    return (double)bytes /
           (1024.0 * 1024.0);
}


/*
 * Convert Mach absolute time to seconds.
 */
double mach_absolute_time_to_seconds(uint64_t time)
{
    mach_timebase_info_data_t timebase;

    mach_timebase_info(&timebase);

    double nanoseconds =
        (double)time *
        (double)timebase.numer /
        (double)timebase.denom;

    return nanoseconds / 1000000000.0;
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
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

    mach_port_t host = mach_host_self();

    kern_return_t result = host_statistics64(
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

    vm_size_t page_size;

    result = host_page_size(
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
        free_pages * (uint64_t)page_size;

    memory_info->active =
        active_pages * (uint64_t)page_size;

    memory_info->inactive =
        inactive_pages * (uint64_t)page_size;

    memory_info->wired =
        wired_pages * (uint64_t)page_size;


    /*
     * Get total physical memory.
     */
    uint64_t total_memory;

    size_t size = sizeof(total_memory);

    if (sysctlbyname(
            "hw.memsize",
            &total_memory,
            &size,
            NULL,
            0
        ) == -1)
    {
        perror("hw.memsize");
        return -1;
    }

    memory_info->total = total_memory;


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
 * Information about one process.
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
 * Get a snapshot of all running processes.
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
        i < count && process_count < max_processes;
        i++
    )
    {
        if (pids[i] <= 0)
        {
            continue;
        }

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


        /*
         * Get detailed process information.
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

        /*
         * Process may have exited or
         * information may be unavailable.
         */
        if (result != sizeof(task_info))
        {
            continue;
        }


        Process process;

        process.pid = pids[i];

        snprintf(
            process.name,
            sizeof(process.name),
            "%s",
            name
        );

        process.memory =
            task_info.pti_resident_size;


        /*
         * These CPU counters are expressed
         * in nanoseconds.
         */
        process.cpu_time =
            task_info.pti_total_user +
            task_info.pti_total_system;

        process.cpu_percent = 0.0;

        processes[process_count] = process;

        process_count++;
    }

    return process_count;
}


/*
 * Find a process in a snapshot by PID.
 */
int find_process(
    Process processes[],
    int process_count,
    pid_t pid
)
{
    for (int i = 0; i < process_count; i++)
    {
        if (processes[i].pid == pid)
        {
            return i;
        }
    }

    return -1;
}


/*
 * Sort processes by CPU usage.
 * Highest CPU usage first.
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
 * Print the complete monitor screen.
 */
void print_monitor(
    int cpu_count,
    double cpu_usage,
    MemoryInfo *memory_info,
    Process processes[],
    int process_count
)
{
    /*
     * Clear the terminal and move cursor
     * to the top-left corner.
     */
    printf("\033[2J");
    printf("\033[H");

    printf("============================================\n");
    printf("              SYSTEM MONITOR                \n");
    printf("============================================\n\n");


    /*
     * CPU
     */
    printf(
        "Logical CPUs: %d\n",
        cpu_count
    );

    printf(
        "CPU Usage:    %.2f%%\n\n",
        cpu_usage
    );


    /*
     * Memory
     */
    printf(
        "Total memory: %.2f GB\n",
        bytes_to_gb(memory_info->total)
    );

    printf(
        "Used memory:  %.2f GB\n",
        bytes_to_gb(memory_info->used)
    );

    printf(
        "Free memory:  %.2f GB\n",
        bytes_to_gb(memory_info->free)
    );

    printf(
        "Active memory: %.2f GB\n",
        bytes_to_gb(memory_info->active)
    );

    printf(
        "Inactive memory: %.2f GB\n",
        bytes_to_gb(memory_info->inactive)
    );

    printf(
        "Wired memory: %.2f GB\n",
        bytes_to_gb(memory_info->wired)
    );


    /*
     * Memory percentage.
     */
    double memory_usage = 0.0;

    if (memory_info->total > 0)
    {
        memory_usage =
            ((double)memory_info->used /
             (double)memory_info->total) *
            100.0;
    }

    printf(
        "Memory Usage: %.2f%%\n\n",
        memory_usage
    );


    /*
     * Process list.
     */
    printf(
        "TOP 10 PROCESSES BY CPU\n"
    );

    printf(
        "============================================\n"
    );

    int processes_to_print = process_count;

    if (processes_to_print > 10)
    {
        processes_to_print = 10;
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

    printf("\n");
    printf("Press Ctrl+C to stop.\n");

    fflush(stdout);
}


/*
 * Main program.
 */
int main(void)
{
    /*
     * Get number of logical CPUs.
     */
    int cpu_count;

    size_t size = sizeof(cpu_count);

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
     * Continuously update the monitor.
     */
    while (1)
    {
        /*
         * Take first CPU snapshot.
         */
        host_cpu_load_info_data_t first_cpu;

        get_cpu_ticks(&first_cpu);


        /*
         * Take first process snapshot.
         */
        Process first_snapshot[MAX_PROCESSES];

        int first_count =
            get_process_snapshot(
                first_snapshot,
                MAX_PROCESSES
            );

        if (first_count == -1)
        {
            return 1;
        }


        /*
         * Wait for the sampling interval.
         */
        sleep(SAMPLE_INTERVAL);


        /*
         * Take second CPU snapshot.
         */
        host_cpu_load_info_data_t second_cpu;

        get_cpu_ticks(&second_cpu);


        /*
         * Calculate total CPU usage.
         */
        double cpu_usage =
            calculate_cpu_usage(
                &first_cpu,
                &second_cpu
            );


        /*
         * Take second process snapshot.
         */
        Process second_snapshot[MAX_PROCESSES];

        int second_count =
            get_process_snapshot(
                second_snapshot,
                MAX_PROCESSES
            );

        if (second_count == -1)
        {
            return 1;
        }


        /*
         * Calculate process CPU percentages.
         */
        for (
            int i = 0;
            i < second_count;
            i++
        )
        {
            int first_index =
                find_process(
                    first_snapshot,
                    first_count,
                    second_snapshot[i].pid
                );

            /*
             * New process or process that
             * disappeared from the first snapshot.
             */
            if (first_index == -1)
            {
                continue;
            }


            /*
             * CPU time counters are nanoseconds.
             */
            uint64_t first_cpu_time =
                first_snapshot[first_index].cpu_time;

            uint64_t second_cpu_time =
                second_snapshot[i].cpu_time;


            /*
             * Calculate CPU time consumed.
             */
            uint64_t cpu_time_delta =
                second_cpu_time -
                first_cpu_time;


            /*
             * Convert nanoseconds to seconds.
             */
            double process_cpu_seconds =
                (double)cpu_time_delta /
                1000000000.0;


            /*
             * Calculate percentage.
             *
             * 100% = one completely utilized CPU core.
             *
             * A multithreaded process can therefore
             * exceed 100%.
             */
            double cpu_percent =
                (process_cpu_seconds /
                 (double)SAMPLE_INTERVAL) *
                100.0;

            second_snapshot[i].cpu_percent =
                cpu_percent;
        }


        /*
         * Sort by CPU usage.
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
         * Display everything.
         */
        print_monitor(
            cpu_count,
            cpu_usage,
            &memory_info,
            second_snapshot,
            second_count
        );
    }

    return 0;
}