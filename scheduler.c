/*
 * scheduler.c - scheduling algorithm and Gantt-chart timeline for Phase 4
 *
 * Selection algorithm (sched_select_next):
 *   1. Shell commands (is_builtin, remaining_secs == -1) always win.
 *   2. Among program tasks, choose the one with the smallest remaining_secs
 *      (Shortest Remaining Job First).  Tie-break: queue order (FCFS).
 *   3. Anti-starvation: the same task cannot be selected twice in a row
 *      unless it is the only ready program task.
 *
 * Preemption (sched_enqueue):
 *   When a new task arrives while a *program* is running, it may preempt if:
 *   - the new task is a shell command, OR
 *   - the new task has a shorter remaining_secs than the running task.
 *   Preemption is requested by setting running_task->preempt = 1; the
 *   executing thread checks this flag between output lines.
 */

#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>   /* INT_MAX */

/* ── global definitions ─────────────────────────────────────────────────── */

pthread_mutex_t  sched_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   sched_ready  = PTHREAD_COND_INITIALIZER;
TaskNode        *task_queue   = NULL;
TaskNode        *running_task = NULL;
bool             cpu_busy     = false;

/* ── timeline (Gantt chart) ─────────────────────────────────────────────── */

/*
 * SliceRecord - one contiguous block of CPU time awarded to one task.
 * We keep a singly-linked list in arrival order; timeline_print walks it.
 */
typedef struct SliceRecord {
    int tid;
    int duration;           /* seconds actually executed in this slice */
    struct SliceRecord *next;
} SliceRecord;

static SliceRecord *tl_head = NULL;
static SliceRecord *tl_tail = NULL;

/* ID of the most recently dispatched task (enforces anti-starvation rule). */
static int last_tid = -1;

/* ── sched_init ──────────────────────────────────────────────────────────── */

void sched_init(void)
{
    tl_head = tl_tail = NULL;
    last_tid = -1;
}

/* ── sched_enqueue ───────────────────────────────────────────────────────── */

void sched_enqueue(TaskNode *t)
{
    /* Append to the tail so queue order reflects arrival order (FCFS tiebreak). */
    t->next = NULL;
    if (!task_queue) {
        task_queue = t;
    } else {
        TaskNode *cur = task_queue;
        while (cur->next) cur = cur->next;
        cur->next = t;
    }

    /*
     * Preemption check: only possible when the CPU is occupied by a program.
     * Shell commands cannot be preempted once started.
     *
     * Two cases warrant a preemption signal:
     *   (a) the newcomer is a shell command — those always outrank programs, or
     *   (b) the newcomer is a program with strictly fewer remaining seconds
     *       (SRJF rule: shorter burst takes precedence immediately).
     *
     * In both cases we set the atomic flag; the running thread checks it
     * between output lines and suspends the child via SIGSTOP when it sees it.
     */
    if (cpu_busy && running_task && !running_task->is_builtin) {
        bool outranks = t->is_builtin ||
                        (t->remaining_secs < running_task->remaining_secs);
        if (outranks)
            running_task->preempt = 1;
    }

    /* wake the scheduler thread so it can re-evaluate the queue */
    pthread_cond_signal(&sched_ready);
}

/* ── sched_dequeue ───────────────────────────────────────────────────────── */

void sched_dequeue(TaskNode *t)
{
    if (!task_queue) return;

    if (task_queue == t) {
        task_queue = t->next;
        t->next = NULL;
        return;
    }

    TaskNode *cur = task_queue;
    while (cur->next && cur->next != t)
        cur = cur->next;
    if (cur->next == t) {
        cur->next = t->next;
        t->next   = NULL;
    }
}

/* ── sched_select_next ───────────────────────────────────────────────────── */

TaskNode *sched_select_next(void)
{
    if (!task_queue) return NULL;

    /* Pass 1 — any non-done shell command takes absolute priority. */
    for (TaskNode *cur = task_queue; cur; cur = cur->next) {
        if (cur->state != TASK_DONE && cur->is_builtin)
            return cur;
    }

    /*
     * Count only non-done *program* tasks.  We deliberately exclude shell
     * commands from this count: the anti-starvation rule ("no consecutive
     * repeat") applies to the scheduling of programs among themselves and
     * should not be affected by how many shell commands are waiting.
     */
    int n_prog = 0;
    for (TaskNode *cur = task_queue; cur; cur = cur->next) {
        if (cur->state != TASK_DONE && !cur->is_builtin)
            n_prog++;
    }

    /* Pass 2 — SRJF among program tasks, respecting anti-starvation rule. */
    TaskNode *best  = NULL;
    int       min_r = INT_MAX;

    for (TaskNode *cur = task_queue; cur; cur = cur->next) {
        if (cur->state == TASK_DONE || cur->is_builtin) continue;

        /* Skip this task if it just ran and there are other options. */
        if (n_prog > 1 && cur->tid == last_tid) continue;

        if (cur->remaining_secs < min_r) {
            min_r = cur->remaining_secs;
            best  = cur;
        }
    }

    if (best)
        last_tid = best->tid;

    return best;
}

/* ── timeline_record ─────────────────────────────────────────────────────── */

void timeline_record(int tid, int duration_secs)
{
    if (duration_secs <= 0) return;

    SliceRecord *sr = malloc(sizeof(*sr));
    if (!sr) return;      /* non-fatal; chart entry is silently dropped */

    sr->tid      = tid;
    sr->duration = duration_secs;
    sr->next     = NULL;

    if (!tl_head) {
        tl_head = tl_tail = sr;
    } else {
        tl_tail->next = sr;
        tl_tail       = sr;
    }
}

/* ── timeline_print ──────────────────────────────────────────────────────── */

void timeline_print(void)
{
    if (!tl_head) return;  /* no program tasks ran; nothing to show */

    printf("\n=== Scheduling Timeline (Gantt Chart) ===\n");

    /* Single-line Gantt: time markers bracket each CPU slice */
    int t = 0;
    printf("  %d", t);
    for (SliceRecord *sr = tl_head; sr; sr = sr->next) {
        t += sr->duration;
        printf("--[P%d]--%d", sr->tid, t);
    }
    printf("\n");

    /* Per-task totals: iterate unique tids, sum all slices for each */
    printf("\n  Per-task CPU time:\n");
    for (SliceRecord *outer = tl_head; outer; outer = outer->next) {
        /* skip if we already printed this tid */
        bool seen = false;
        for (SliceRecord *prev = tl_head; prev != outer; prev = prev->next) {
            if (prev->tid == outer->tid) { seen = true; break; }
        }
        if (seen) continue;

        int total = 0;
        for (SliceRecord *s = tl_head; s; s = s->next) {
            if (s->tid == outer->tid) total += s->duration;
        }
        printf("    P%d: %d s total\n", outer->tid, total);
    }
    printf("\n");
    fflush(stdout);

    /* free list so subsequent runs start with a clean slate */
    SliceRecord *cur = tl_head;
    while (cur) {
        SliceRecord *nxt = cur->next;
        free(cur);
        cur = nxt;
    }
    tl_head = tl_tail = NULL;
}
