#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h> //give us access to Mach APIs.
#include <mach/mach_host.h> //give us access to Mach APIs.
#include <mach/mach_time.h>
#include <unistd.h> //gives us sleep()
#include <stdlib.h>
#include <stdint.h>
#include <libproc.h>
#define MAX_PROCESSES 4096 //reasonable maximum size for our PID array.
//enough space to store information of abt up to 4096 process
//having more than 4096 simultaneously running processes is unlikely
#define SAMPLE_INTERVAL 1.0
//can use mach timebase to convert the process CPU time units into nanoseconds

void get_cpu_ticks(host_cpu_load_info_data_t *cpu_info)
{
    mach_msg_type_number_t count =
        HOST_CPU_LOAD_INFO_COUNT; //tells the Mach API how much CPU information we're working with.

    kern_return_t result = host_statistics64(
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

double get_cpu_usage(void)
{
    host_cpu_load_info_data_t first;
    host_cpu_load_info_data_t second;

    get_cpu_ticks(&first);

    sleep(1); //not a good idea to hardcode this
    //may give 1.003 or 1.012 etc, not a sharp and precise 1 second

    get_cpu_ticks(&second);

    unsigned int user_delta =
        second.cpu_ticks[CPU_STATE_USER] -
        first.cpu_ticks[CPU_STATE_USER];

    unsigned int system_delta =
        second.cpu_ticks[CPU_STATE_SYSTEM] -
        first.cpu_ticks[CPU_STATE_SYSTEM];

    unsigned int idle_delta =
        second.cpu_ticks[CPU_STATE_IDLE] -
        first.cpu_ticks[CPU_STATE_IDLE];

    unsigned int nice_delta =
        second.cpu_ticks[CPU_STATE_NICE] -
        first.cpu_ticks[CPU_STATE_NICE];

    unsigned int total_delta =
        user_delta +
        system_delta +
        idle_delta +
        nice_delta;

    unsigned int busy_delta =
        user_delta +
        system_delta +
        nice_delta;

    if (total_delta == 0)
    {
        return 0.0;
    }

    return ((double)busy_delta / total_delta) * 100.0;
    //we use double as busy_delta/total_delta are both integers. integer division can produce 0 instead of 0.6
    //double treats it as a floating point number
}


double bytes_to_gb(uint64_t bytes) //converts bytes to gigabytes - divide by 10^9
{
    return (double)bytes / (1024.0 * 1024.0 * 1024.0);
}

double bytes_to_mb(uint64_t bytes) //converts bytes to megabytes - divide by 10^6
{
    return (double)bytes / (1024.0 * 1024.0);
}

double mach_absolute_time_to_seconds(uint64_t time)
{
    mach_timebase_info_data_t timebase;
    //this asks macOS for the conversion ratio
    //from CPU time to seconds to nanoseconds

    mach_timebase_info(&timebase);

    double nanoseconds =
        (double)time *
        (double)timebase.numer /
        (double)timebase.denom;

    return (double)nanoseconds / 1000000000.0;
    //converts nanoseconds into seconds
}

typedef struct
{
    uint64_t total;
    uint64_t used;
    uint64_t free;
    uint64_t active;
    uint64_t inactive;
    uint64_t wired;
} MemoryInfo;


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

    memory_info->used =
        memory_info->total -
        memory_info->free -
        memory_info->inactive;

    return 0;

}

typedef struct
{
    pid_t pid;
    char name[256];

    uint64_t memory;

    uint64_t cpu_time;

    double cpu_percent;
} Process;

int get_process_snapshot(Process processes[], int max_processes)
{
    pid_t pids[MAX_PROCESSES];

    int count = proc_listallpids(pids, sizeof(pids));
    //retrieves list of all active pids

    if (count == -1)
    {
        fprintf(stderr, "proc_listallpids failed\n");
        return -1;
    }

    int process_count = 0;

    for(int i = 0; i<count && process_count<max_processes;i++)
    {
        char name[256];
        int name_length = proc_name(pids[i], name, sizeof(name));

        if(name_length<=0)
        {
            snprintf(name, sizeof(name), "unknown");
        }

        //get information about this process

        struct proc_taskinfo task_info;

        int result = proc_pidinfo(
            pids[i],
            PROC_PIDTASKINFO,
            0,
            &task_info,
            sizeof(task_info)
        );

        //the process may have exited between getting the PID list and asking for its information
        //it is also possible that we dont have access to it
        //if we cant get the information, skip the process

        if (result != sizeof(task_info))
        {
            continue;
        }

        Process process;

        process.pid = pids[i];

        snprintf(process.name, sizeof(process.name),"%s",name);

        process.memory = task_info.pti_resident_size;

        process.cpu_time = task_info.pti_total_user + task_info.pti_total_system;
        //this is cummulative cpu time
        //pti_total_user and pti_total_system are CPU time counters
        //but the counters are not expressed directly in seconds
        //units of pti are not the same as CPU seconds

        process.cpu_percent = 0.0;

        processes[process_count] = process;

        process_count++;
    }

    return process_count;

}

int find_process(Process processes[], int process_count, pid_t pid)
//this function tells us where is PID X in this process array
{
    for(int i = 0; i < process_count; i++)
    {
        if(processes[i].pid == pid)
        {
            return i; //return its array position
        }
    }
    return -1; //if the process pid doesnt exist
}

int compare_processes_by_cpu(const void *a, const void *b)
{
    const Process *process_a = (const Process *)a;
    const Process *process_b = (const Process *)b;

    if (process_a->cpu_percent < process_b->cpu_percent)
    {
        return 1;
    }

    if (process_a->cpu_percent > process_b->cpu_percent)
    {
        return -1;
    }

    return 0;
}

int compare_processes_by_memory(const void *a, const void *b)
{
    const Process *process_a = (const Process *)a;
    const Process *process_b = (const Process *)b;

    if (process_a->memory < process_b->memory)
    {
        return 1;
    }

    if (process_a->memory > process_b->memory)
    {
        return -1;
    }

    return 0;
}
//qsort() needs a function that tells it which of the 2 elements should come first
//this comparison sorts processes from highest memory usage to lowest memory usage


int main(void)
{
    int cpu_count;
    size_t size = sizeof(cpu_count);

    //sysctlbyname asks macOS for number of logical CPUs

    if (sysctlbyname(
            "hw.logicalcpu",
            &cpu_count,
            &size,
            NULL,
            0
        ) == -1)
    {
        perror("sysctlbyname");
        return 1;
    }

    printf("Logical CPUs: %d\n\n", cpu_count);


    /*
     * CPU MONITOR
     */

    double cpu_usage = get_cpu_usage();

    printf(
        "CPU Usage: %.2f%%\n\n",
        cpu_usage
    );


    /*
     * TOTAL MEMORY
     */

    uint64_t total_memory;

    size = sizeof(total_memory);

    if (sysctlbyname(
            "hw.memsize",
            &total_memory,
            &size,
            NULL,
            0
        ) == -1)
    {
        perror("hw.memsize");
        return 1; //returning 0 means success and 1 means error
    }

    printf(
        "Total memory: %.2f GB\n\n",
        (double)total_memory /
        (1024.0 * 1024.0 * 1024.0)
    );

    double total_gb = bytes_to_gb(total_memory);

    printf(
        "Total memory using bytes_to_gb(): %.2f GB\n\n",
        total_gb
    );

        /*
     * MEMORY STATISTICS
     */

    MemoryInfo memory_info;

    if (get_memory_info(&memory_info) == -1)
    {
        return 1;
    }

    printf(
        "Used memory: %.2f GB\n",
        bytes_to_gb(memory_info.used)
    );

    printf(
        "Free memory: %.2f GB\n",
        bytes_to_gb(memory_info.free)
    );

    printf(
        "Active memory: %.2f GB\n",
        bytes_to_gb(memory_info.active)
    );

    printf(
        "Inactive memory: %.2f GB\n",
        bytes_to_gb(memory_info.inactive)
    );

    printf(
        "Wired memory: %.2f GB\n\n",
        bytes_to_gb(memory_info.wired)
    );

    double memory_usage =
        ((double)memory_info.used /
        (double)memory_info.total) * 100.0;

    printf(
        "Memory Usage: %.2f%%\n\n",
        memory_usage
    );

    /*
     * PROCESS LIST
     */

    Process first_snapshot[MAX_PROCESSES];

    uint64_t start_time = mach_absolute_time();

    int first_count = get_process_snapshot(
        first_snapshot,
        MAX_PROCESSES
    );

    if (first_count == -1)
    {
        return 1;
    }

    sleep(1);

    Process second_snapshot[MAX_PROCESSES];

    int second_count = get_process_snapshot(
        second_snapshot,
        MAX_PROCESSES
    );

    if (second_count==-1)
    {
        return 1;
    }

    uint64_t end_time = mach_absolute_time();

    double elapsed_seconds = mach_absolute_time_to_seconds(end_time - start_time);

    //have to match processes by PID
    //cant calculate a process' CPU usage if we cant find a match (no earlier/later measurement)

    printf("\nPROCESS CPU DELTAS\n");
    printf("======================================\n");

    for(int i = 0; i < second_count; i++)
    //for every process in the 2nd snapshot
    {
        int first_index = find_process(first_snapshot, first_count, second_snapshot[i].pid);
        //we find the same PID in the first snapshot
        if (first_index == -1)
        {
            continue;
        }

        uint64_t first_cpu_time = first_snapshot[first_index].cpu_time;
        //get the CPU time from one second ago

        uint64_t second_cpu_time = second_snapshot[i].cpu_time;
        //gets the current CPU time

        uint64_t cpu_time_delta = second_cpu_time - first_cpu_time;
        //calculates CPU time consumed during the interval

        //calculate the % of total CPU capacity used by this process during the sample interval

        //the machine has multiple logical CPUs, so the total CPU capacity is
        //number of logical CPUs * sample interval

        double process_cpu_seconds = mach_absolute_time_to_seconds(cpu_time_delta);
        //store the calculated CPU % in our process structure
        
        double cpu_percent = (process_cpu_seconds / elapsed_seconds)*100.0;
        //a process can use more than 100% CPU when it has multiple threads running on multiple CPU cores at the same time
        //store the calculated CPU % in our process structure
    

        second_snapshot[i].cpu_percent = cpu_percent;
     
    }

    qsort(
        second_snapshot,
        second_count,
        sizeof(Process),
        compare_processes_by_cpu
    );

    //printing the sorted list
    printf("\nTOP 10 PROCESSES BY CPU\n");
    printf("======================================\n");

    int processes_to_print = second_count;

    if (processes_to_print > 10)
    {
        processes_to_print = 10;
    }

    for (int i = 0; i < processes_to_print; i++)
    {
        printf(
            "%d  %-30s CPU: %.2f%% Memory: %.2f MB\n",
            second_snapshot[i].pid,
            second_snapshot[i].name,
            second_snapshot[i].cpu_percent,
            bytes_to_mb(second_snapshot[i].memory)
        );
    }

    return 0;
}