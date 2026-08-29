#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h> //give us access to Mach APIs.
#include <mach/mach_host.h> //give us access to Mach APIs.
#include <unistd.h> //gives us sleep()
#include <stdlib.h>
#include <stdint.h>
#include <libproc.h>
#define MAX_PROCESSES 4096 //reasonable maximum size for our PID array.
//enough space to store information of abt up to 4096 process
//having more than 4096 simultaneously running processes is unlikely


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

    sleep(1);

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


typedef struct
{
    uint64_t total;
    uint64_t used;
    uint64_t free;
    uint64_t active;
    uint64_t inactive;
    uint64_t wired;
} MemoryInfo;


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
     * PROCESS LIST
     */

    Process first_snapshot[MAX_PROCESSES];

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

        printf(
            "%d  %-30s CPU time delta: %llu\n",
            second_snapshot[i].pid,
            second_snapshot[i].name,
            cpu_time_delta
        );
        
    }
    /*
    printf("PROCESSES\n");
    printf("============================================\n");

    for (int i = 0; i < process_count; i++)
{
    printf(
        "%d  %-30s CPU time: %llu  Memory: %.2f MB\n",
        processes[i].pid,
        processes[i].name,
        processes[i].cpu_time,
        bytes_to_mb(processes[i].memory)
    );
} */

    return 0;
}