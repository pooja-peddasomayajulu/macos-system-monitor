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
    mach_msg_type_number_t count =
        HOST_CPU_LOAD_INFO_COUNT;

    kern_return_t result =
        host_statistics64(
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
}


/*
 * Convert bytes to gigabytes.
 */
double bytes_to_gb(uint64_t bytes)
{
    return
        (double)bytes /
        (1024.0 * 1024.0 * 1024.0);
}


/*
 * Convert bytes to megabytes.
 */
double bytes_to_mb(uint64_t bytes)
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

        /*
         * Process may have exited or
         * information may be unavailable.
         */
        if (result != sizeof(task_info))
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
            return i;
        }
    }

    return -1;
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


/*
 * Print the monitor screen.
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
     * Clear screen.
     */
    printf("\033[2J");
    printf("\033[H");


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
        "Logical CPUs: %d\n",
        cpu_count
    );

    printf(
        "CPU Usage:    %.2f%%\n\n",
        cpu_usage
    );


    /*
     * Memory information.
     */
    printf(
        "Total memory:    %.2f GB\n",
        bytes_to_gb(
            memory_info->total
        )
    );

    printf(
        "Used memory:     %.2f GB\n",
        bytes_to_gb(
            memory_info->used
        )
    );

    printf(
        "Free memory:     %.2f GB\n",
        bytes_to_gb(
            memory_info->free
        )
    );

    printf(
        "Active memory:   %.2f GB\n",
        bytes_to_gb(
            memory_info->active
        )
    );

    printf(
        "Inactive memory: %.2f GB\n",
        bytes_to_gb(
            memory_info->inactive
        )
    );

    printf(
        "Wired memory:    %.2f GB\n",
        bytes_to_gb(
            memory_info->wired
        )
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
        "Memory Usage:    %.2f%%\n\n",
        memory_usage
    );


    /*
     * TOP 10 BY CPU
     */
    printf(
        "TOP 10 PROCESSES BY CPU\n"
    );

    printf(
        "============================================\n"
    );


    int processes_to_print =
        process_count;

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


    /*
     * Create a separate copy of the
     * process list for memory sorting.
     */
    Process memory_sorted[MAX_PROCESSES];

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


    printf("\n");


    /*
     * TOP 10 BY MEMORY
     */
    printf(
        "TOP 10 PROCESSES BY MEMORY\n"
    );

    printf(
        "============================================\n"
    );


    int memory_processes_to_print =
        process_count;

    if (
        memory_processes_to_print > 10
    )
    {
        memory_processes_to_print = 10;
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

    size_t size =
        sizeof(cpu_count);

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
     * Continuously update monitor.
     */
    while (1)
    {
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

        if (first_count == -1)
        {
            return 1;
        }


        /*
         * Wait one second.
         */
        sleep(
            SAMPLE_INTERVAL
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

        if (second_count == -1)
        {
            return 1;
        }


        /*
         * Calculate CPU percentage
         * for every process.
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
             * Process did not exist
             * in the first snapshot.
             */
            if (first_index == -1)
            {
                continue;
            }


            uint64_t first_cpu_time =
                first_snapshot[
                    first_index
                ].cpu_time;

            uint64_t second_cpu_time =
                second_snapshot[
                    i
                ].cpu_time;


            uint64_t cpu_time_delta =
                second_cpu_time -
                first_cpu_time;


            /*
             * CPU counters are nanoseconds.
             */
            double process_cpu_seconds =
                (double)cpu_time_delta /
                1000000000.0;


            double cpu_percent =
                (process_cpu_seconds /
                 (double)SAMPLE_INTERVAL) *
                100.0;


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
         * Display monitor.
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