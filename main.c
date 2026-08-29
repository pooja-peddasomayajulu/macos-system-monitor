#include <stdio.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h> //give us access to Mach APIs.
#include <mach/mach_host.h> //give us access to Mach APIs.
#include <unistd.h> //gives us sleep()
#include <stdlib.h>
#include <stdint.h>
#include <libproc.h>


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

    pid_t pids[4096];

    int count = proc_listallpids(
        pids,
        sizeof(pids)
    ); //retrieves list of active pids

    if (count == -1)
    {
        fprintf(
            stderr,
            "proc_listallpids failed\n"
        );

        return 1;
    }


    printf("PROCESSES\n");
    printf("============================================\n");


    for (int i = 0; i < count; i++)
    {
        char name[256];

        int name_length = proc_name(
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
         * Get information about this process.
         */

        struct proc_taskinfo task_info;

        int result = proc_pidinfo( //gets task/process information for PID pids[i]
            pids[i],
            PROC_PIDTASKINFO,
            0,
            &task_info,
            sizeof(task_info)
        );


        /*
         * The process may have exited between
         * getting the PID list and asking for its information.
         * It is also possible that we don't have access to it.
         *
         * If we can't get the information, skip this process.
         */

        if (result != sizeof(task_info))
        {
            continue;
        }


        uint64_t cpu_time = task_info.pti_total_user + task_info.pti_total_system; //cummulative cpu time accumulated by process

        uint64_t memory = task_info.pti_resident_size; //how much resident memory the process is currently using


        printf(
            "%d  %-30s CPU time: %llu  Memory: %.2f MB\n",
            pids[i],
            name,
            cpu_time,
            bytes_to_mb(memory)
        );
    }


    return 0;
}