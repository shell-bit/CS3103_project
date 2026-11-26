#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    INITIAL_ARRAY_CAPACITY = 16,
    ARRAY_GROWTH_FACTOR = 2,
    PRIORITY_MIN = 1, PRIORITY_MAX = 5,
    RR_SHORT = 3, RR_LONG = 6,
    MLFQ_HIGH = 4, MLFQ_MIDDLE = 8, MLFQ_LEVELS = 3,
    RUN_TO_COMPLETION = 0,
    GANTT_SPACING = 3 /* Two spaces and one separator per segment. */
};

static const char IDLE_LABEL[] = "IDLE";
static const char MLFQ_OPTION[] = "--mlfq";

typedef int64_t Time;

typedef struct {
    char *id;
    int arrival, burst, priority;
} Job;

typedef struct {
    Job *jobs;
    size_t count;
} JobQueue;

typedef struct {
    const Job *job;
    Time start, end;
} Segment;

typedef struct {
    Segment *segments;
    size_t count, capacity;
    bool failed;
} GanttChart;

typedef struct {
    Time completion, turnaround, waiting;
} JobMetrics;

typedef struct {
    const JobQueue *queue;
    JobMetrics *rows;
    double average_turnaround, average_waiting, utilisation;
} Metrics;

typedef struct RunJob RunJob;
struct RunJob {
    const Job *job;
    size_t input_position;
    Time remaining;
    Time budget;
    RunJob *next;
};

typedef struct {
    RunJob *head, *tail;
} ReadyQueue;

/* Grow array using its capacity and element size. Return the resized array
 * and update capacity on success. Return NULL on failure. */
static void *grow_array(void *array, size_t *capacity, size_t element_size)
{
    size_t limit = SIZE_MAX / element_size;
    if (*capacity == limit)
        return NULL;

    size_t new_capacity = *capacity > limit / ARRAY_GROWTH_FACTOR
                        ? limit : *capacity * ARRAY_GROWTH_FACTOR;
    if (new_capacity == 0)
        new_capacity = limit < INITIAL_ARRAY_CAPACITY ? limit : INITIAL_ARRAY_CAPACITY;

    void *grown = realloc(array, new_capacity * element_size);
    if (grown != NULL)
        *capacity = new_capacity;
    return grown;
}

/* Release the queue's owned IDs and array, leaving an empty queue. */
static void free_jobs(JobQueue *queue)
{
    for (size_t i = 0; i < queue->count; ++i)
        free(queue->jobs[i].id);
    free(queue->jobs);
    *queue = (JobQueue){0};
}

/* Parse token as a decimal integer from min to max. Store it in value and
 * return true on success. Return false on invalid input. */
static bool parse_int(const char *token, int min, int max, int *value)
{
    char *end;
    errno = 0;
    long number = strtol(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' ||
        number < min || number > max)
        return false;

    *value = (int)number;
    return true;
}

/* Compare the two Job-pointer array elements by ID for qsort. Return a
 * negative value, zero or a positive value in lexicographic order. */
static int compare_ids(const void *left, const void *right)
{
    const Job *const *left_job = left;
    const Job *const *right_job = right;
    return strcmp((*left_job)->id, (*right_job)->id);
}

/* Check queue for duplicate IDs while preserving its order. Use filepath in
 * diagnostics. Return 0 on success or -1 on duplicates or allocation failure. */
static int check_unique_ids(const JobQueue *queue, const char *filepath)
{
    if (queue->count > SIZE_MAX / sizeof(Job *))
        return -1;
    const Job **sorted = malloc(queue->count * sizeof(*sorted));
    if (sorted == NULL) {
        fprintf(stderr, "%s: unable to check job IDs\n", filepath);
        return -1;
    }

    for (size_t i = 0; i < queue->count; ++i)
        sorted[i] = &queue->jobs[i];
    qsort(sorted, queue->count, sizeof(*sorted), compare_ids);

    int status = 0;
    for (size_t i = 1; i < queue->count; ++i) {
        if (strcmp(sorted[i - 1]->id, sorted[i]->id) == 0) {
            fprintf(stderr, "%s: duplicate job ID '%s'\n", filepath, sorted[i]->id);
            status = -1;
            break;
        }
    }
    free(sorted);
    return status;
}

/* Read filepath into an empty queue in file order. Return the job count on
 * success or -1 on error, leaving queue empty on error. */
int load_jobs(const char *filepath, JobQueue *queue)
{
    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        perror(filepath);
        return -1;
    }

    char *line = NULL;
    size_t line_capacity = 0, job_capacity = 0, line_number = 0;
    const char *error = NULL;
    bool first_row = true;
    ssize_t line_length;

    while ((line_length = getline(&line, &line_capacity, file)) >= 0) {
        ++line_number;
        if (memchr(line, '\0', (size_t)line_length) != NULL) {
            error = "unexpected NUL byte";
            break;
        }

        const char *whitespace = " \t\r\n\v\f";
        char *rest;
        char *id = strtok_r(line, whitespace, &rest);
        if (id == NULL)
            continue;

        char *arrival = strtok_r(NULL, whitespace, &rest);
        char *burst = strtok_r(NULL, whitespace, &rest);
        char *priority = strtok_r(NULL, whitespace, &rest);
        char *extra = strtok_r(NULL, whitespace, &rest);
        if (first_row && arrival && burst && priority && !extra &&
            strcmp(id, "job_id") == 0 && strcmp(arrival, "arrival") == 0 &&
            strcmp(burst, "burst") == 0 && strcmp(priority, "priority") == 0) {
            first_row = false;
            continue;
        }
        first_row = false;

        Job job = {0};
        if (!arrival || !burst || !priority || extra ||
            !parse_int(arrival, 0, INT_MAX, &job.arrival) ||
            !parse_int(burst, 1, INT_MAX, &job.burst) ||
            !parse_int(priority, PRIORITY_MIN, PRIORITY_MAX, &job.priority)) {
            error = "expected job_id, nonnegative arrival, positive burst and priority 1-5";
            break;
        }
        if (queue->count == INT_MAX) {
            error = "too many jobs";
            break;
        }
        if (queue->count == job_capacity) {
            Job *grown = grow_array(queue->jobs, &job_capacity, sizeof(*grown));
            if (grown == NULL) {
                error = "unable to allocate job storage";
                break;
            }
            queue->jobs = grown;
        }
        job.id = strdup(id);
        if (job.id == NULL) {
            error = "unable to copy job ID";
            break;
        }
        queue->jobs[queue->count++] = job;
    }

    if (error == NULL && (!feof(file) || ferror(file)))
        error = "unable to read the complete input";
    if (error == NULL && queue->count == 0)
        error = "no jobs found";

    free(line);
    fclose(file);
    if (error != NULL)
        fprintf(stderr, "%s:%zu: %s\n", filepath, line_number, error);
    if (error != NULL || check_unique_ids(queue, filepath) != 0) {
        free_jobs(queue);
        return -1;
    }
    return (int)queue->count;
}

/* Compare two RunJob records by arrival and file position for qsort.
 * Return a negative value, zero or a positive value in that order. */
static int compare_arrivals(const void *left, const void *right)
{
    const RunJob *left_job = left;
    const RunJob *right_job = right;
    if (left_job->job->arrival != right_job->job->arrival)
        return left_job->job->arrival < right_job->job->arrival ? -1 : 1;
    return (left_job->input_position > right_job->input_position) -
           (left_job->input_position < right_job->input_position);
}

/* Allocate and return fresh, arrival-ordered run state for queue. The caller
 * frees it. Return NULL for an empty queue or allocation failure, marking
 * chart failed on allocation failure. */
static RunJob *new_run(const JobQueue *queue, GanttChart *chart)
{
    if (queue->count == 0)
        return NULL;

    RunJob *run = NULL;
    if (queue->count <= SIZE_MAX / sizeof(*run))
        run = malloc(queue->count * sizeof(*run));
    if (run == NULL) {
        chart->failed = true;
        return NULL;
    }
    for (size_t i = 0; i < queue->count; ++i)
        run[i] = (RunJob){.job = &queue->jobs[i], .input_position = i,
                          .remaining = queue->jobs[i].burst};
    qsort(run, queue->count, sizeof(*run), compare_arrivals);
    return run;
}

/* Record [now, end) for job in chart and set now to end. NULL job means idle.
 * If merge is true, extend a preceding interval for the same job. On
 * allocation failure, mark chart failed and leave now in place. */
static void advance(GanttChart *chart, const Job *job, Time *now,
                    Time end, bool merge)
{
    if (merge && chart->count > 0) {
        Segment *last = &chart->segments[chart->count - 1];
        if (last->job == job) {
            last->end = end;
            *now = end;
            return;
        }
    }

    if (chart->count == chart->capacity) {
        Segment *grown = grow_array(chart->segments, &chart->capacity, sizeof(*grown));
        if (grown == NULL) {
            chart->failed = true;
            return;
        }
        chart->segments = grown;
    }
    chart->segments[chart->count++] = (Segment){job, *now, end};
    *now = end;
}

/* Schedule the jobs in queue to completion in arrival order, breaking ties
 * by file position. Record execution and idle intervals in chart. */
void schedule_FCFS(JobQueue *queue, GanttChart *chart)
{
    RunJob *run = new_run(queue, chart);
    if (run == NULL)
        return;

    Time now = 0;
    for (size_t i = 0; i < queue->count && !chart->failed; ++i) {
        const Job *job = run[i].job;
        if (job->arrival > now)
            advance(chart, NULL, &now, job->arrival, false);
        if (!chart->failed)
            advance(chart, job, &now, now + job->burst, false);
    }
    free(run);
}

/* Write a schedule for queue into chart. If preemptive is true, rank by
 * priority and reconsider at arrivals. Otherwise choose the shortest burst
 * at each dispatch and run it to completion. */
static void schedule_ranked(const JobQueue *queue, GanttChart *chart,
                            bool preemptive)
{
    RunJob *run = new_run(queue, chart);
    if (run == NULL)
        return;

    Time now = 0;
    size_t arrived = 0, completed = 0;
    while (completed < queue->count && !chart->failed) {
        while (arrived < queue->count && run[arrived].job->arrival <= now)
            ++arrived;

        RunJob *best = NULL;
        int best_rank = INT_MAX;
        for (size_t i = 0; i < arrived; ++i) {
            if (run[i].remaining == 0)
                continue;
            int rank = preemptive ? run[i].job->priority : run[i].job->burst;
            if (best == NULL || rank < best_rank) {
                best = &run[i];
                best_rank = rank;
            }
        }
        if (best == NULL) {
            advance(chart, NULL, &now, run[arrived].job->arrival, false);
            continue;
        }

        Time end = now + best->remaining;
        if (preemptive && arrived < queue->count && run[arrived].job->arrival < end)
            end = run[arrived].job->arrival;
        best->remaining -= end - now;
        advance(chart, best->job, &now, end, preemptive);
        if (best->remaining == 0)
            ++completed;
    }
    free(run);
}

/* Write the non-preemptive SJF schedule for queue into chart. Break equal
 * bursts by arrival and file order. */
void schedule_SJF(JobQueue *queue, GanttChart *chart)
{
    schedule_ranked(queue, chart, false);
}

/* Write the preemptive priority schedule for queue into chart. Break equal
 * priorities by arrival and file order. */
void schedule_priority_preemptive(JobQueue *queue, GanttChart *chart)
{
    schedule_ranked(queue, chart, true);
}

/* Append a job to the tail of a ready queue. */
static void enqueue(ReadyQueue *ready, RunJob *job)
{
    job->next = NULL;
    if (ready->tail != NULL)
        ready->tail->next = job;
    else
        ready->head = job;
    ready->tail = job;
}

/* Remove and return the current head of a nonempty ready queue. */
static RunJob *dequeue(ReadyQueue *ready)
{
    RunJob *job = ready->head;
    ready->head = job->next;
    if (ready->head == NULL)
        ready->tail = NULL;
    job->next = NULL;
    return job;
}

/* Append jobs arriving by now from the sorted run of count jobs to ready.
 * Advance next past the jobs admitted. */
static void admit_arrivals(RunJob *run, size_t count, size_t *next,
                           Time now, ReadyQueue *ready)
{
    while (*next < count && run[*next].job->arrival <= now) {
        enqueue(ready, &run[*next]);
        ++*next;
    }
}

/* Schedule queue into chart using level_count ready queues and their quanta.
 * A single level implements RR. An MLFQ final level with RUN_TO_COMPLETION
 * uses non-preemptive FCFS. */
static void schedule_levels(const JobQueue *queue, const int quanta[],
                            size_t level_count, GanttChart *chart)
{
    RunJob *run = new_run(queue, chart);
    if (run == NULL)
        return;

    ReadyQueue ready[MLFQ_LEVELS] = {0};
    Time now = 0;
    size_t next = 0, completed = 0;
    while (completed < queue->count && !chart->failed) {
        admit_arrivals(run, queue->count, &next, now, &ready[0]);

        size_t level = 0;
        while (level < level_count && ready[level].head == NULL)
            ++level;
        if (level == level_count) {
            advance(chart, NULL, &now, run[next].job->arrival, false);
            continue;
        }

        RunJob *job = ready[level].head;
        int quantum = quanta[level];
        if (job->budget == 0)
            job->budget = quantum == RUN_TO_COMPLETION ? job->remaining : quantum;
        Time duration = job->remaining < job->budget ? job->remaining : job->budget;
        /* A higher-level arrival may interrupt a middle-level RR slice. */
        if (level > 0 && quantum > 0 && next < queue->count &&
            run[next].job->arrival - now < duration)
            duration = run[next].job->arrival - now;

        advance(chart, job->job, &now, now + duration, false);
        if (chart->failed)
            break;
        job->remaining -= duration;
        job->budget -= duration;

        /* Arrivals at a slice boundary enter before the expired job rejoins. */
        admit_arrivals(run, queue->count, &next, now, &ready[0]);
        if (job->remaining == 0) {
            dequeue(&ready[level]);
            ++completed;
        } else if (job->budget == 0) {
            size_t destination = level + 1 < level_count ? level + 1 : level;
            enqueue(&ready[destination], dequeue(&ready[level]));
        }
    }
    free(run);
}

/* Write the RR schedule for queue into chart using time_quantum. Requeue
 * unfinished jobs at the rear when their slices expire. Mark chart failed
 * if time_quantum is invalid. */
void schedule_RR(JobQueue *queue, int time_quantum, GanttChart *chart)
{
    if (time_quantum <= 0) {
        chart->failed = true;
        return;
    }
    schedule_levels(queue, &time_quantum, 1, chart);
}

/* Write the MLFQ schedule for queue into chart using RR at levels 1 and 2
 * and non-preemptive FCFS at level 3. */
void schedule_MLFQ(JobQueue *queue, GanttChart *chart)
{
    const int quanta[MLFQ_LEVELS] = {MLFQ_HIGH, MLFQ_MIDDLE, RUN_TO_COMPLETION};
    schedule_levels(queue, quanta, MLFQ_LEVELS, chart);
}

/* Derive per-job metrics and summaries from chart and queue. Store them in
 * result in file order. The caller frees result->rows, which is NULL for an
 * empty queue or allocation failure. */
void compute_metrics(const GanttChart *chart, const JobQueue *queue,
                     Metrics *result)
{
    *result = (Metrics){.queue = queue};
    if (queue->count == 0)
        return;
    result->rows = calloc(queue->count, sizeof(*result->rows));
    if (result->rows == NULL)
        return;

    for (size_t i = 0; i < chart->count; ++i) {
        const Segment *segment = &chart->segments[i];
        if (segment->job != NULL)
            result->rows[segment->job - queue->jobs].completion = segment->end;
    }

    Time busy = 0;
    double total_turnaround = 0, total_waiting = 0;
    for (size_t i = 0; i < queue->count; ++i) {
        const Job *job = &queue->jobs[i];
        JobMetrics *row = &result->rows[i];
        row->turnaround = row->completion - job->arrival;
        row->waiting = row->turnaround - job->burst;
        total_turnaround += (double)row->turnaround;
        total_waiting += (double)row->waiting;
        busy += job->burst;
    }

    Time elapsed = chart->count > 0 ? chart->segments[chart->count - 1].end : 0;
    result->average_turnaround = total_turnaround / (double)queue->count;
    result->average_waiting = total_waiting / (double)queue->count;
    if (elapsed > 0)
        result->utilisation = 100.0 * (double)busy / (double)elapsed;
}

/* Return true if id needs quotes to distinguish it from chart syntax. */
static bool quote_id(const char *id)
{
    return strcmp(id, IDLE_LABEL) == 0 || strpbrk(id, "|\\\"") != NULL;
}

/* Return the width of the escaped label for job, or IDLE for NULL. Write
 * that label to stdout if print is true. */
static size_t gantt_label(const Job *job, bool print)
{
    const char *label = job == NULL ? IDLE_LABEL : job->id;
    if (job == NULL || !quote_id(label)) {
        if (print)
            fputs(label, stdout);
        return strlen(label);
    }

    size_t width = 2;
    if (print)
        putchar('"');
    for (const char *c = label; *c; ++c) {
        if (*c == '\\' || *c == '"' || *c == '|') {
            if (print)
                putchar('\\');
            ++width;
        }
        if (print)
            putchar(*c);
        ++width;
    }
    if (print)
        putchar('"');
    return width;
}

/* Return the larger of label_width and the digit count of segment's start. */
static size_t gantt_width(const Segment *segment, size_t label_width)
{
    size_t time_width = (size_t)snprintf(NULL, 0, "%" PRId64, segment->start);
    return label_width > time_width ? label_width : time_width;
}

/* Write n spaces to standard output. */
static void print_spaces(size_t n)
{
    for (size_t i = 0; i < n; ++i)
        putchar(' ');
}

/* Write algo_name, the interval chart and the computed result rows and
 * summaries to stdout. */
void print_schedule(const char *algo_name, const GanttChart *chart,
                    const Metrics *result)
{
    printf("=== %s ===\nGantt: |", algo_name);
    for (size_t i = 0; i < chart->count; ++i) {
        const Segment *segment = &chart->segments[i];
        putchar(' ');
        size_t label_width = gantt_label(segment->job, true);
        print_spaces(gantt_width(segment, label_width) - label_width);
        fputs(" |", stdout);
    }

    fputs("\nTime:  ", stdout);
    Time end = 0;
    for (size_t i = 0; i < chart->count; ++i) {
        const Segment *segment = &chart->segments[i];
        size_t digits = (size_t)printf("%" PRId64, segment->start);
        size_t cell_width = gantt_width(segment, gantt_label(segment->job, false));
        print_spaces(cell_width + GANTT_SPACING - digits);
        end = segment->end;
    }
    printf("%" PRId64 "\n\n", end);

    printf("%-12s %10s %10s %12s %12s %12s\n", "Job", "AT", "BT", "CT", "TAT", "WT");
    for (size_t i = 0; i < result->queue->count; ++i) {
        const Job *job = &result->queue->jobs[i];
        const JobMetrics *row = &result->rows[i];
        printf("%-12s %10d %10d %12" PRId64 " %12" PRId64 " %12" PRId64 "\n",
               job->id, job->arrival, job->burst, row->completion,
               row->turnaround, row->waiting);
    }
    printf("Avg TAT: %.2f   Avg WT: %.2f   CPU Util: %.2f%%\n",
           result->average_turnaround, result->average_waiting, result->utilisation);
}

/* Run the required schedules on queue and append MLFQ if include_mlfq is
 * true. Print each result, free per-run allocations and return true on
 * success or false on scheduling or metrics failure. */
static bool run_schedules(JobQueue *queue, bool include_mlfq)
{
    const struct {
        const char *name;
        void (*schedule)(JobQueue *, GanttChart *);
        int quantum;
    } configurations[] = {
        {.name = "FCFS", .schedule = schedule_FCFS},
        {.name = "SJF", .schedule = schedule_SJF},
        {.name = "Preemptive Priority", .schedule = schedule_priority_preemptive},
        {.name = "Round Robin (q=3)", .quantum = RR_SHORT},
        {.name = "Round Robin (q=6)", .quantum = RR_LONG},
        {.name = "MLFQ", .schedule = schedule_MLFQ}
    };
    size_t count = sizeof(configurations) / sizeof(configurations[0]);
    if (!include_mlfq)
        --count;

    for (size_t i = 0; i < count; ++i) {
        GanttChart chart = {0};
        Metrics metrics = {0};
        if (configurations[i].schedule != NULL)
            configurations[i].schedule(queue, &chart);
        else
            schedule_RR(queue, configurations[i].quantum, &chart);
        if (!chart.failed)
            compute_metrics(&chart, queue, &metrics);

        bool success = metrics.rows != NULL;
        if (success) {
            if (i != 0)
                putchar('\n');
            print_schedule(configurations[i].name, &chart, &metrics);
        } else {
            fprintf(stderr, "%s: unable to allocate schedule or metrics\n",
                    configurations[i].name);
        }
        free(metrics.rows);
        free(chart.segments);
        if (!success)
            return false;
    }
    return true;
}

/* Read the input path and optional MLFQ flag from argc and argv. Load jobs,
 * run the selected configurations and free resources. Return EXIT_SUCCESS
 * on success or EXIT_FAILURE on invalid arguments or a processing error. */
int main(int argc, char *argv[])
{
    bool include_mlfq = argc == 3 && strcmp(argv[2], MLFQ_OPTION) == 0;
    if (argc != 2 && !include_mlfq) {
        fprintf(stderr, "Usage: %s <orders_file> [%s]\n", argv[0], MLFQ_OPTION);
        return EXIT_FAILURE;
    }

    JobQueue queue = {0};
    if (load_jobs(argv[1], &queue) < 0)
        return EXIT_FAILURE;
    bool success = run_schedules(&queue, include_mlfq);
    free_jobs(&queue);
    if (fflush(stdout) != 0 || ferror(stdout))
        success = false;
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
