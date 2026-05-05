/*
 * scheduler.h - Phase 4 task scheduler for myshell server
 *
 * Implements a combined SRJF (Shortest Remaining Job First) + Round-Robin
 * algorithm.  Shell commands have unconditional top priority (burst = -1).
 * Program tasks (./demo) are preemptable via SIGSTOP / SIGCONT.
 *
 * Quanta: 3 seconds on the first slice, 7 seconds on every subsequent slice.
 * No task may be selected twice consecutively unless it is the only ready task.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>
#include <signal.h>

/* ── task lifecycle ──────────────────────────────────────────────────────── */

typedef enum {
    TASK_QUEUED,   /* waiting in the ready list                               */
    TASK_ACTIVE,   /* currently executing on the CPU                         */
    TASK_DONE      /* finished; output fully delivered to the client          */
} TaskState;

/* ── TaskNode ────────────────────────────────────────────────────────────── */

/*
 * One schedulable unit of work, owned by the client thread that submitted it.
 * The node lives on the client thread's stack; the scheduler holds a pointer
 * to it via the task_queue linked list.
 *
 * Synchronisation model:
 *   - sched_mutex protects task_queue, cpu_busy, running_task, and TaskNode
 *     fields other than `preempt`.
 *   - `preempt` is volatile sig_atomic_t so the scheduler can set it from
 *     outside the executing thread without holding sched_mutex.
 *   - Each TaskNode has its own wake_cond; the scheduler signals it when this
 *     task is chosen to run.
 */
typedef struct TaskNode {
    int   tid;              /* unique client / task ID                        */
    int   sockfd;           /* client socket — output destination             */
    char *cmdline;          /* original command string (owned by client thd)  */
    bool  is_builtin;       /* true = shell cmd, false = ./demo program       */

    /* scheduling parameters (seconds == output lines for program tasks) */
    int  total_secs;        /* N from "demo N", or -1 for shell commands      */
    int  remaining_secs;    /* counts down to 0 as the task executes          */
    int  predicted_secs;    /* initial burst estimate (for SRJF comparisons)  */

    /* child process state */
    pid_t cpid;             /* PID of the forked child                        */
    int   pipe_read_fd;     /* read end of the pipe from child's stdout       */
    bool  launched;         /* true after the first fork()                    */
    int    n_scheduled;     /* how many times this task has been dispatched   */
    size_t bytes_sent;      /* total payload bytes forwarded to the client    */

    TaskState state;

    /* per-task wake-up: scheduler signals wake_cond when it's this task's turn */
    pthread_cond_t        wake_cond;
    bool                  wake_flag;

    /* preemption flag: set atomically by sched_enqueue when a higher-priority
     * task arrives; read by run_program_task between output lines            */
    volatile sig_atomic_t preempt;

    struct TaskNode *next;  /* intrusive singly-linked list                   */
} TaskNode;

/* ── global scheduler state (defined in scheduler.c) ────────────────────── */

extern pthread_mutex_t  sched_mutex;   /* guards shared scheduler state      */
extern pthread_cond_t   sched_ready;   /* wakes the scheduler thread         */
extern TaskNode        *task_queue;    /* head of the ready list             */
extern TaskNode        *running_task;  /* task currently using the CPU       */
extern bool             cpu_busy;      /* true while a task is executing     */

/* ── public API ──────────────────────────────────────────────────────────── */

/* Initialise scheduler data structures; call once before spawning threads. */
void      sched_init(void);

/* Add t to the ready list; may set t->preempt on running_task if warranted. */
void      sched_enqueue(TaskNode *t);

/* Remove t from the ready list (call after TASK_DONE, under sched_mutex). */
void      sched_dequeue(TaskNode *t);

/* Choose the best ready task (SRJF + shell priority + anti-starvation). */
TaskNode *sched_select_next(void);

/* Record one execution slice for the Gantt chart. */
void      timeline_record(int tid, int duration_secs);

/* Print the accumulated Gantt chart and clear internal state. */
void      timeline_print(void);

#endif /* SCHEDULER_H */
