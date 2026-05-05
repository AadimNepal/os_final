#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

pthread_mutex_t sched_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sched_cond = PTHREAD_COND_INITIALIZER;
Job *job_queue = NULL;
Job *current_job;
bool cpu_busy;

typedef struct TimelineEntry {
    int job_id;    // e.g., 1 => P1
    int duration;  // how many "time units" this job ran in that slice
    struct TimelineEntry *next;
} TimelineEntry;

static TimelineEntry *timeline_head = NULL;
static TimelineEntry *timeline_tail = NULL;

// For the summary string at the bottom
static char timeline_buffer[4096];

// Track last scheduled job to prevent immediate re-selection (unless only 1 left)
static int last_job_id = -1; 

void scheduler_init() {
    timeline_buffer[0] = '\0';
}

void add_job(Job *j) {
    j->next = NULL;
    if (!job_queue) {
        job_queue = j;
    } else {
        Job *curr = job_queue;
        while (curr->next) curr = curr->next;
        curr->next = j;
    }
    // --- Preemption logic ---
    // Only preempt if CPU is currently running a *program*.
    if (cpu_busy && current_job && !current_job->is_shell_cmd) {

        bool new_has_priority = false;

        // Shell commands always preempt running program
        if (j->is_shell_cmd) {
            new_has_priority = true;
        }
        // Otherwise, SRJF: shorter remaining time wins
        else if (!j->is_shell_cmd &&
                 j->remaining_time < current_job->remaining_time) {
            new_has_priority = true;
        }

        if (new_has_priority) {
            current_job->preempt_requested = 1;
        }
    }

    // Let scheduler know something changed (new job and/or preemption)
    pthread_cond_signal(&sched_cond);
}

void remove_job(Job *j) {
    if (!job_queue) return;
    if (job_queue == j) {
        job_queue = j->next;
        return;
    }
    Job *curr = job_queue;
    while (curr->next && curr->next != j) {
        curr = curr->next;
    }
    if (curr->next == j) {
        curr->next = j->next;
    }
}

// THE ALGORITHM: Combined SRJF + RR
Job* get_next_job() {
    if (!job_queue) return NULL;

    Job *best = NULL;
    Job *curr = job_queue;
    int count = 0;

    // Check for Shell Commands (-1) - HIGHEST PRIORITY
    // They are non-preemptive, run immediately.
    while (curr) {
        if (curr->status != JOB_FINISHED && curr->is_shell_cmd) {
            return curr;
        }
        count++;
        curr = curr->next;
    }

    // Filter for SRJF (Programs)
    curr = job_queue;
    int min_remaining = 999999;

    while (curr) {
        if (curr->status != JOB_FINISHED) {
            // Constraint: Same process can't be selected 2x consecutive times
            // UNLESS it is the only process left.
            bool skip = (count > 1 && curr->id == last_job_id);

            if (!skip) {
                if (curr->remaining_time < min_remaining) {
                    min_remaining = curr->remaining_time;
                    best = curr;
                }
            }
        }
        curr = curr->next;
    }

    if (best) {
        last_job_id = best->id;
    }
    return best;
}

void append_timeline(int job_id, int duration) {
    // Ignore bogus / non-positive slices
    if (duration <= 0) return;

    TimelineEntry *e = malloc(sizeof(*e));
    if (!e) return;  // ignore on OOM, not worth crashing the server

    e->job_id = job_id;
    e->duration = duration;
    e->next = NULL;

    if (!timeline_head) {
        timeline_head = timeline_tail = e;
    } else {
        timeline_tail->next = e;
        timeline_tail = e;
    }
}

void print_timeline(void) {
    if (!timeline_head) {
        // No demo jobs ran -> no Gantt diagram
        return;
    }

    int current_time = 0;
    TimelineEntry *cur = timeline_head;

    // Print initial "0"
    printf("%d", current_time);

    while (cur) {
        current_time += cur->duration;  // cumulative time
        printf(")-P%d-(%d", cur->job_id, current_time);
        cur = cur->next;
    }

    printf("\n");
    fflush(stdout);

    // Clear the timeline after printing so the next run starts fresh
    cur = timeline_head;
    while (cur) {
        TimelineEntry *next = cur->next;
        free(cur);
        cur = next;
    }
    timeline_head = timeline_tail = NULL;
}