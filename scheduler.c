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
#include <limits.h>   /* INT_MAX — used as the initial "no best yet" sentinel */

/* ── global definitions ─────────────────────────────────────────────────── */

/*
 * sched_mutex guards task_queue, running_task, cpu_busy, and all TaskNode
 * fields except `preempt` (which is volatile sig_atomic_t and lock-free).
 * sched_ready is signalled whenever the queue state changes so that the
 * scheduler thread can re-evaluate which task to run next.
 */
pthread_mutex_t  sched_mutex  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t   sched_ready  = PTHREAD_COND_INITIALIZER;
TaskNode        *task_queue   = NULL;   /* head of the singly-linked ready list */
TaskNode        *running_task = NULL;   /* pointer to the currently executing task */
bool             cpu_busy     = false;  /* true while a task holds the CPU */

/* ── timeline (Gantt chart) ─────────────────────────────────────────────── */

/*
 * SliceRecord stores one contiguous block of CPU time awarded to one task.
 * Records are appended in chronological order; timeline_print walks the list.
 */
typedef struct SliceRecord {
    int tid;            /* which task ran during this slice */
    int duration;       /* how many seconds (lines) executed in this slice */
    struct SliceRecord *next;
} SliceRecord;

static SliceRecord *tl_head = NULL;   /* oldest slice */
static SliceRecord *tl_tail = NULL;   /* most recent slice; used for O(1) append */

/*
 * last_tid tracks which task was dispatched most recently.
 * Initialised to -1 so the first task is never incorrectly skipped.
 * Updated in sched_select_next each time a task is chosen.
 */
static int last_tid = -1;

/* ── sched_init ──────────────────────────────────────────────────────────── */

void sched_init(void)
{
    /* reset Gantt-chart list and anti-starvation state before any threads start */
    tl_head  = tl_tail = NULL;
    last_tid = -1;
}

/* ── sched_enqueue ───────────────────────────────────────────────────────── */

void sched_enqueue(TaskNode *t)
{
    /*
     * Append to the tail of the list so that arrival order is preserved.
     * When two program tasks have equal remaining_secs, sched_select_next
     * walks from head to tail and picks the first match — naturally giving
     * the tie to the task that arrived earlier (FCFS tiebreak).
     */
    t->next = NULL;
    if (!task_queue) {
        /* first task: becomes both head and tail */
        task_queue = t;
    } else {
        /* find the current tail and link the new task after it */
        TaskNode *cur = task_queue;
        while (cur->next) cur = cur->next;
        cur->next = t;
    }

    /*
     * Preemption check: evaluate whether the newly arrived task should
     * interrupt the currently running task.
     *
     * Conditions that must ALL hold before we can preempt:
     *   1. cpu_busy        — some task is actually executing right now
     *   2. running_task    — we have a valid pointer to it
     *   3. !is_builtin     — shell commands cannot be preempted once started
     *
     * Within those conditions, preempt if either:
     *   (a) the newcomer is a shell command (highest priority, always wins), or
     *   (b) the newcomer is a program with strictly fewer remaining seconds
     *       than the running task (SRJF: a shorter job just arrived).
     *
     * We signal preemption by setting the atomic `preempt` flag on the running
     * task.  The executing thread (run_program_task) checks this flag between
     * output lines and sends SIGSTOP to the child when it sees it.
     */
    if (cpu_busy && running_task && !running_task->is_builtin) {
        bool outranks = t->is_builtin ||
                        (t->remaining_secs < running_task->remaining_secs);
        if (outranks)
            running_task->preempt = 1;  /* atomic write; no lock needed for sig_atomic_t */
    }

    /* wake the scheduler thread so it can re-evaluate the ready queue */
    pthread_cond_signal(&sched_ready);
}

/* ── sched_dequeue ───────────────────────────────────────────────────────── */

void sched_dequeue(TaskNode *t)
{
    if (!task_queue) return;   /* nothing in the list; nothing to do */

    if (task_queue == t) {
        /* task is at the head: advance head to the next node */
        task_queue = t->next;
        t->next = NULL;        /* unlink so the node is no longer in the list */
        return;
    }

    /* search for the node that points to t, then splice t out */
    TaskNode *cur = task_queue;
    while (cur->next && cur->next != t)
        cur = cur->next;

    if (cur->next == t) {
        cur->next = t->next;   /* bypass t */
        t->next   = NULL;      /* clean up t's own pointer */
    }
    /* if t is not found, the list is already consistent — do nothing */
}

/* ── sched_select_next ───────────────────────────────────────────────────── */

TaskNode *sched_select_next(void)
{
    if (!task_queue) return NULL;  /* empty queue; nothing to dispatch */

    /* ── Pass 1: shell commands have unconditional priority ── */
    /*
     * Walk the list from head to tail.  The first non-done shell command we
     * find is returned immediately, ahead of all program tasks regardless of
     * their remaining burst time.  Shell commands are also non-preemptable:
     * once started they run until completion.
     */
    for (TaskNode *cur = task_queue; cur; cur = cur->next) {
        if (cur->state != TASK_DONE && cur->is_builtin)
            return cur;
    }

    /*
     * ── Count ready program tasks for the anti-starvation check ──
     *
     * We count ONLY non-done program tasks (not shell commands).  The rule is:
     * "the same program cannot be selected twice in a row if other programs are
     * waiting."  Including shell commands in this count would be incorrect: a
     * queued shell command does not make a program eligible for re-selection
     * since shell commands are handled separately in Pass 1.
     *
     * This differs from the reference implementation (phase4/), which counts
     * ALL queue nodes and can therefore incorrectly suppress re-selection of
     * the only remaining program when a shell command is also in the queue.
     */
    int n_prog = 0;
    for (TaskNode *cur = task_queue; cur; cur = cur->next) {
        if (cur->state != TASK_DONE && !cur->is_builtin)
            n_prog++;
    }

    /* ── Pass 2: SRJF among program tasks ── */
    TaskNode *best  = NULL;
    int       min_r = INT_MAX;  /* sentinel: any real remaining_secs will be smaller */

    for (TaskNode *cur = task_queue; cur; cur = cur->next) {
        if (cur->state == TASK_DONE || cur->is_builtin) continue;

        /*
         * Anti-starvation skip: if there are at least 2 program tasks ready
         * and this task just ran (cur->tid == last_tid), skip it this round
         * so the other task(s) get a turn.  When n_prog == 1, this is the
         * only choice, so we never skip it.
         */
        if (n_prog > 1 && cur->tid == last_tid) continue;

        /*
         * SRJF comparison: prefer the task with the fewest remaining seconds.
         * Because the queue is in FCFS order (earlier arrivals appear first)
         * and we use strict less-than (<), ties naturally resolve in favor of
         * the task that was enqueued first — the FCFS tiebreak.
         */
        if (cur->remaining_secs < min_r) {
            min_r = cur->remaining_secs;
            best  = cur;
        }
    }

    if (best)
        last_tid = best->tid;  /* remember this choice to enforce anti-starvation next time */

    return best;   /* NULL if all program tasks were skipped (rare; scheduler re-waits) */
}

/* ── timeline_record ─────────────────────────────────────────────────────── */

void timeline_record(int tid, int duration_secs)
{
    /* skip zero-duration slices; they would clutter the Gantt chart with nothing useful */
    if (duration_secs <= 0) return;

    SliceRecord *sr = malloc(sizeof(*sr));
    if (!sr) return;   /* allocation failure is non-fatal; one chart entry is dropped */

    sr->tid      = tid;
    sr->duration = duration_secs;
    sr->next     = NULL;

    /* append to tail so records stay in chronological execution order */
    if (!tl_head) {
        tl_head = tl_tail = sr;   /* first record: becomes both head and tail */
    } else {
        tl_tail->next = sr;   /* link after current tail */
        tl_tail       = sr;   /* advance tail pointer */
    }
}

/* ── timeline_print ──────────────────────────────────────────────────────── */

void timeline_print(void)
{
    if (!tl_head) return;  /* no program tasks ran; nothing to print */

    printf("\n=== Scheduling Timeline (Gantt Chart) ===\n");

    /*
     * Single-line Gantt format:
     *   0--[P2]--2--[P3]--5--[P2]--11--[P3]--13
     *
     * `t` is the cumulative wall-clock time; each slice advances it by
     * its duration.  We print the start time, the label, and the end time
     * for every slice in arrival order.
     */
    int t = 0;
    printf("  %d", t);
    for (SliceRecord *sr = tl_head; sr; sr = sr->next) {
        t += sr->duration;                    /* advance clock by this slice's length */
        printf("--[P%d]--%d", sr->tid, t);   /* bracket the slice with time markers */
    }
    printf("\n");

    /*
     * Per-task CPU time summary:
     * Walk the list once per unique tid.  To avoid printing a tid more than
     * once, use an O(n²) seen-check: for each `outer` node, scan all previous
     * nodes to see if we already encountered the same tid.  If yes, skip.
     * If no, do a second scan to sum all slices belonging to that tid.
     *
     * O(n²) is acceptable here because n is bounded by the number of scheduling
     * slices across the entire session, which is typically small (< 50).
     */
    printf("\n  Per-task CPU time:\n");
    for (SliceRecord *outer = tl_head; outer; outer = outer->next) {
        /* check whether this tid has already been printed */
        bool seen = false;
        for (SliceRecord *prev = tl_head; prev != outer; prev = prev->next) {
            if (prev->tid == outer->tid) { seen = true; break; }
        }
        if (seen) continue;   /* already totalled this task; skip duplicate */

        /* accumulate all slices for this tid */
        int total = 0;
        for (SliceRecord *s = tl_head; s; s = s->next) {
            if (s->tid == outer->tid) total += s->duration;
        }
        printf("    P%d: %d s total\n", outer->tid, total);
    }
    printf("\n");
    fflush(stdout);   /* ensure chart appears on the terminal before the next prompt */

    /* free all slice records so subsequent test runs start with a clean list */
    SliceRecord *cur = tl_head;
    while (cur) {
        SliceRecord *nxt = cur->next;
        free(cur);     /* each record was individually malloc'd in timeline_record */
        cur = nxt;
    }
    tl_head = tl_tail = NULL;   /* reset list pointers to empty state */
}
