#ifndef SCHEDULER_H
#define SCHEDULER_H
#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <signal.h>


// Status of a job in the system
typedef enum {
    JOB_WAITING,
    JOB_RUNNING,
    JOB_FINISHED
} JobStatus;

// The Job Structure
typedef struct Job {
    int id;                 // Client ID
    int socket_fd;          // Client socket
    
    char *command;          // Full command string
    bool is_shell_cmd;      // true if ls, pwd, etc. false if ./demo
    
    // Scheduling Times
    int total_time;         // N (for demo), or -1 (shell)
    int remaining_time;     // Decrements as it runs
    int burst_prediction;   // For SRJF comparison
    
    // Execution State
    pid_t pid;              // The child process ID
    int pipe_fd;            // Read end of the pipe
    bool started;           // Has fork() happened?
    int rounds_run;         // How many times it has been scheduled
    
    JobStatus status;
    
    // Synchronization for this specific job
    pthread_cond_t cond;    // Thread sleeps here when not running
    bool my_turn;           // Flag to wake up
    volatile sig_atomic_t preempt_requested;
    struct Job *next;
} Job;

// Global Scheduler State
extern pthread_mutex_t sched_lock;
extern pthread_cond_t sched_cond; // Wakes scheduler thread
extern Job *job_queue;
extern Job *current_job;
extern bool cpu_busy;
// Functions
void scheduler_init();
void add_job(Job *job);
void remove_job(Job *job);
Job* get_next_job(); // The SRJF Algorithm
void append_timeline(int job_id, int duration);
void print_timeline();

#endif