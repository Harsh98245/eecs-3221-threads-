/*
 * alarm_cond.c
 *
 * This is an enhancement to the alarm_mutex.c program, which
 * used only a mutex to synchronize access to the shared alarm
 * list. This version adds a condition variable. The alarm
 * thread waits on this condition variable, with a timeout that
 * corresponds to the earliest timer request. If the main thread
 * enters an earlier timeout, it signals the condition variable
 * so that the alarm thread will wake up and process the earlier
 * timeout first, requeueing the later request.
 */
#include <pthread.h>
#include <time.h>
#include "errors.h"

#define BUFFER_SIZE 4

/*
 * The "alarm" structure now contains the time_t (time since the
 * Epoch, in seconds) for each alarm, so that they can be
 * sorted. Storing the requested number of seconds would not be
 * enough, since the "alarm thread" cannot tell how long it has
 * been on the list.
 */

typedef enum
{
    START_ALARM,
    CHANGE_ALARM,
    CANCEL_ALARM,
    SUSPEND_ALARM,
    REACTIVATE_ALARM,
    VIEW_ALARMS
} Type;

const char *alarmTypeStr[] = {
    "Start_Alarm",
    "Change_Alarm",
    "Cancel_Alarm",
    "Suspend_Alarm",
    "Reactivate_Alarm",
    "View_Alarm"};

typedef struct alarm_tag
{
    struct alarm_tag *link;
    int seconds;
    time_t time; /* seconds from EPOCH */
    char message[128];
    Type type;
    int id;
    int groupId;
    int interval;
    char alarm_type[20];
    time_t timestamp;
} alarm_t;

typedef struct
{
    alarm_t buffer[BUFFER_SIZE];
    int consumerOffset;
    int producerOffset;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t full;
    pthread_cond_t empty;
} CircularBuffer;

void cb_init(CircularBuffer *cb)
{
    cb->consumerOffset = 0;
    cb->producerOffset = 0;
    cb->size = 0;
    pthread_mutex_init(&cb->mutex, NULL);
    pthread_cond_init(&cb->full, NULL);
    pthread_cond_init(&cb->empty, NULL);
}

void cb_push(CircularBuffer *cb, alarm_t *alarm)
{
    pthread_mutex_lock(&cb->mutex);
    while (cb->size == BUFFER_SIZE)
    {
        pthread_cond_wait(&cb->full, &cb->mutex);
    }
    memcpy(&cb->buffer[cb->producerOffset], alarm, sizeof(alarm_t));
    cb->producerOffset = (cb->producerOffset + 1) % BUFFER_SIZE;
    printf("%d\n", cb->producerOffset);
    cb->size = cb->size + 1;
    pthread_cond_signal(&cb->empty);
    pthread_mutex_unlock(&cb->mutex);
}

alarm_t cb_pop(CircularBuffer *cb)
{
    pthread_mutex_lock(&cb->mutex);
    while (cb->size == 0)
    {
        pthread_cond_wait(&cb->empty, &cb->mutex);
    }
    alarm_t alarm = cb->buffer[cb->consumerOffset];
    cb->consumerOffset = (cb->consumerOffset + 1) % BUFFER_SIZE;
    cb->size = cb->size - 1;
    pthread_cond_signal(&cb->full);
    pthread_mutex_unlock(&cb->mutex);
    return alarm;
}

pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
alarm_t *alarm_list = NULL;
time_t current_alarm = 0;
alarm_t *change_alarm_list = NULL;
int most_recent_displayed_alarm_id = 0;


/*
 * Insert alarm entry on list, in order.
 */
void alarm_insert(alarm_t *alarm)
{
    int status;
    alarm_t **last, *next;

    /*
     * LOCKING PROTOCOL:
     *
     * This routine requires that the caller have locked the
     * alarm_mutex!
     */
    if(strcmp(alarm->alarm_type, "Change_Alarm") == 0) {
        last = &change_alarm_list;
    } else {
        last = &alarm_list;
    }
    next = *last;
    while (next != NULL)
    {
        if (next->timestamp >= alarm->timestamp)
        {
            alarm->link = next;
            *last = alarm;
            break;
        }
        last = &next->link;
        next = next->link;
    }
    /*
     * If we reached the end of the list, insert the new alarm
     * there.  ("next" is NULL, and "last" points to the link
     * field of the last item, or to the list header.)
     */
    if (next == NULL)
    {
        *last = alarm;
        alarm->link = NULL;
    }
#ifdef DEBUG
    printf("[list: ");
    for (next = alarm_list; next != NULL; next = next->link)
        printf("%d(%d)[\"%s\"] ", next->time,
               next->time - time(NULL), next->message);
    printf("]\n");
#endif
    /*
     * Wake the alarm thread if it is not busy (that is, if
     * current_alarm is 0, signifying that it's waiting for
     * work), or if the new alarm comes before the one on
     * which the alarm thread is waiting.
     */
    if (current_alarm == 0 || alarm->timestamp < current_alarm)
    {
        current_alarm = alarm->timestamp;
        status = pthread_cond_signal(&alarm_cond);
        if (status != 0)
            err_abort(status, "Signal cond");
    }
}

void *consumer_thread(void *args)
{
    printf("Started consumer thread\n");
    CircularBuffer *cb = (CircularBuffer*) args;
    int status;
    alarm_t alarm;
    status = pthread_mutex_lock(&cb->mutex);
    if (status != 0)
        err_abort(status, "Lock mutex");
    while(1) {
        while(cb->producerOffset <= cb->consumerOffset) {
            // printf("waiting for main to put something\n");
            status = pthread_cond_wait(&cb->empty, &cb->mutex);
            if(status != 0) {
                err_abort(status, "Wait on condition");
            }
        }
        // printf("stopped waiting\n");
        int bufferIndex = cb->consumerOffset;
        pthread_mutex_unlock(&cb->mutex);
        alarm = cb_pop(cb);
        printf("Consumer Thread has Retrieved Alarm_Request_Type %s Request"
                "(%d) at %ld: %ld from Circular_Buffer Index:"
                "%d\n", alarm.alarm_type, alarm.id, time(NULL), alarm.timestamp, bufferIndex);
        status = pthread_mutex_lock(&alarm_mutex);
        if(status != 0) {
            err_abort(status, "Lock Mutex");
        }
        if(!strcmp("Start_Alarm", alarm.alarm_type)) {
            alarm_insert(&alarm);
            printf("Start_Alarm(%d) Inserted by Consumer Thread %lu Into Alarm"
                    "List: Group(%d) %ld %d %ld %s\n",
                alarm.id, pthread_self(), alarm.groupId, alarm.timestamp, alarm.interval, alarm.time, alarm.message);
        } else if(!strcmp("Change_Alarm", alarm.alarm_type)) {
            alarm_insert(&alarm);
            printf("Change_Alarm(%d) Inserted by Consumer Thread %lu Into Seperate"
                    "Change Alarm Request List: Group(%d) %ld %d %ld %s\n",
                alarm.id, pthread_self(), alarm.groupId, alarm.timestamp, alarm.interval, alarm.time, alarm.message);
        }
        status = pthread_mutex_unlock(&alarm_mutex);
        if(status != 0) {
            err_abort(status, "unLock Mutex");
        }      
    }
}

void *change_alarm_thread(void *args)
{
    printf("Started change alarm thread\n");
}

void *suspend_reactivate_alarm_thread(void *args)
{
    printf("Started suspend thread\n");
}

void *remove_alarm_thread(void *args)
{
    printf("Started remove thread\n");
}

void *view_alarms_thread(void *args)
{
    printf("Started view alarm thread\n");
}

void *display_alarm_thread(void *args)
{
    printf("Started display alarm thread\n");
}

/*
 * The alarm thread's start routine.
 */
void *alarm_thread(void *arg)
{
    alarm_t *alarm;
    struct timespec cond_time;
    time_t now;
    int status, expired;

    /*
     * Loop forever, processing commands. The alarm thread will
     * be disintegrated when the process exits. Lock the mutex
     * at the start -- it will be unlocked during condition
     * waits, so the main thread can insert alarms.
     */
    status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0)
        err_abort(status, "Lock mutex");
    while (1)
    {
        /*
         * If the alarm list is empty, wait until an alarm is
         * added. Setting current_alarm to 0 informs the insert
         * routine that the thread is not busy.
         */
        current_alarm = 0;
        while (alarm_list == NULL)
        {
            status = pthread_cond_wait(&alarm_cond, &alarm_mutex);
            if (status != 0)
                err_abort(status, "Wait on cond");
        }
        alarm = alarm_list;
        alarm_list = alarm->link;
        now = time(NULL);
        expired = 0;
        if (alarm->time > now)
        {
#ifdef DEBUG
            printf("[waiting: %d(%d)\"%s\"]\n", alarm->time,
                   alarm->time - time(NULL), alarm->message);
#endif
            cond_time.tv_sec = alarm->time;
            cond_time.tv_nsec = 0;
            current_alarm = alarm->time;
            while (current_alarm == alarm->time)
            {
                status = pthread_cond_timedwait(
                    &alarm_cond, &alarm_mutex, &cond_time);
                if (status == ETIMEDOUT)
                {
                    expired = 1;
                    break;
                }
                if (status != 0)
                    err_abort(status, "Cond timedwait");
            }
            if (!expired)
                alarm_insert(alarm);
        }
        else
            expired = 1;
        if (expired)
        {
            printf("(%d) %s\n", alarm->interval, alarm->message);
            // free(alarm);
        }
    }
}

int main(int argc, char *argv[])
{
    int status;
    char line[128];
    alarm_t *alarm;
    pthread_t thread[6];

    CircularBuffer cb;
    cb_init(&cb);

    status = pthread_create(
        &thread[0], NULL, alarm_thread, NULL);
    if (status != 0)
        err_abort(status, "Create alarm thread");

    status = pthread_create(
        &thread[1], NULL, consumer_thread, &cb);
    if (status != 0)
        err_abort(status, "Create consumer thread");

    status = pthread_create(
        &thread[2], NULL, change_alarm_thread, NULL);
    if (status != 0)
        err_abort(status, "Create change alarm thread");

    status = pthread_create(
        &thread[3], NULL, suspend_reactivate_alarm_thread, NULL);
    if (status != 0)
        err_abort(status, "Create suspend reactivate alarm thread");

    status = pthread_create(
        &thread[4], NULL, remove_alarm_thread, NULL);
    if (status != 0)
        err_abort(status, "Create remove alarm thread");

    status = pthread_create(
        &thread[5], NULL, view_alarms_thread, NULL);
    if (status != 0)
        err_abort(status, "Create view alarm thread");

    while (1)
    {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin) == NULL)
            exit(0);
        if (strlen(line) <= 1)
            continue;
        alarm = (alarm_t *)malloc(sizeof(alarm_t));
        if (alarm == NULL)
            errno_abort("Allocate alarm");

        /*
         * Parse input line into seconds (%d) and a message
         * (%64[^\n]), consisting of up to 64 characters
         * separated from the seconds by whitespace.
         */
        if (sscanf(line, "%[^ (](%d): Group(%d) %d %128[^\n]",
                   alarm->alarm_type, &alarm->id, &alarm->groupId, &alarm->interval, alarm->message) == 5)
        {
            alarm->time = time(NULL) + alarm->interval;
            alarm->timestamp = time(NULL);
            int bufferIndex = cb.producerOffset;
            cb_push(&cb, alarm);
            printf("0 Alarm Thread has Inserted Alarm_Request_Type %s Request("
                   "%d) at %ld: %ld into Circular_Buffer Index: %d\n.",
                   alarm->alarm_type, alarm->id, alarm->timestamp, alarm->timestamp, bufferIndex);
        }
        else if (sscanf(line, "%[^ (](%d)",
                        alarm->alarm_type, &alarm->id) == 2)
        {
            int bufferIndex = cb.producerOffset;
            alarm->timestamp = time(NULL);
            cb_push(&cb, alarm);
            printf("1 Alarm Thread has Inserted Alarm_Request_Type %s Request("
                   "%d) at %ld: %ld into Circular_Buffer Index: %d\n.",
                   alarm->alarm_type, alarm->id, alarm->timestamp, alarm->timestamp, bufferIndex);
        }
        else if (sscanf(line, "View Alarms") == 0)
        {
            int bufferIndex = cb.producerOffset;
            alarm->timestamp = time(NULL);
            cb_push(&cb, alarm);
            printf("2 Alarm Thread has Inserted Alarm_Request_Type %s at %ld: %ld into Circular_Buffer Index: %d\n.", alarm->alarm_type, alarm->timestamp, alarm->timestamp, bufferIndex);
        }
        else
        {
            fprintf(stderr, "Bad command\n");
            free(alarm);
        }
        // if (sscanf (line, "%d %64[^\n]",
        //     &alarm->seconds, alarm->message) < 2) {
        //     fprintf (stderr, "Bad command\n");
        //     free (alarm);
        // }
        // else {
        //     status = pthread_mutex_lock (&alarm_mutex);
        //     if (status != 0)
        //         err_abort (status, "Lock mutex");
        //     alarm->time = time (NULL) + alarm->seconds;
        //     /*
        //      * Insert the new alarm into the list of alarms,
        //      * sorted by expiration time.
        //      */
        //     alarm_insert (alarm);
        //     status = pthread_mutex_unlock (&alarm_mutex);
        //     if (status != 0)
        //         err_abort (status, "Unlock mutex");
        // }
    }
}
