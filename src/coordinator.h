/* Runs checks and installs one app at a time, and keeps the schedule. */
#ifndef MCU_COORDINATOR_H
#define MCU_COORDINATOR_H

#include "settings.h"

/* Callbacks may arrive on the thread that runs the scan. */
typedef struct CoordinatorEvents
{
    void *context;
    void (*result_changed)(void *context, const UpdateResult *result);
    void (*activity)(void *context, const ActivityEntry *entry);
    void (*busy_changed)(void *context, bool busy);
    void (*summary_changed)(void *context, const wchar_t *summary, bool finished);
} CoordinatorEvents;

typedef struct Coordinator
{
    AppSettings *settings;
    SettingsStore *store;
    UpdateProvider provider;
    CoordinatorEvents events;
    volatile LONG busy;
} Coordinator;

/* The monitored apps as they were when a run began, so later edits only affect the next run. */
typedef struct RunSnapshot { ProgramSettings programs[APP_COUNT]; int count; } RunSnapshot;

/* How soon a scheduled run tries again after an update waited because its app was in use. */
#define IN_USE_RETRY_MINUTES 30

typedef enum { SCHEDULE_NOT_DUE, SCHEDULE_DUE, SCHEDULE_SAVE_FAILED } ScheduleCheck;

void coordinator_init(Coordinator *coordinator, AppSettings *settings, SettingsStore *store, UpdateProvider provider);
bool coordinator_is_busy(const Coordinator *coordinator);
bool coordinator_recalculate_schedule(Coordinator *coordinator, wchar_t *error, size_t error_cap);
/* The first time after `now` that the timing comes round. */
DateTimeOffset coordinator_compute_next_run(const ScheduleTiming *timing, DateTimeOffset now);

/* A run in two steps, so the snapshot is taken on the thread that owns the settings.
   coordinator_begin returns false when a run is already under way. */
bool coordinator_begin(Coordinator *coordinator, RunSnapshot *snapshot);
/* Returns how many updates waited because their app was in use. */
int coordinator_execute(Coordinator *coordinator, const RunSnapshot *snapshot, bool install_updates);
int coordinator_run(Coordinator *coordinator, bool install_updates);

/* The schedule in two steps around a run. coordinator_schedule_due moves the next run forward before it returns
   SCHEDULE_DUE, so a restart or a slow installer cannot cause a storm of scans. */
ScheduleCheck coordinator_schedule_due(Coordinator *coordinator, wchar_t *error, size_t error_cap);
void coordinator_schedule_finished(Coordinator *coordinator, int waiting);
void coordinator_evaluate_schedule(Coordinator *coordinator);

#endif
