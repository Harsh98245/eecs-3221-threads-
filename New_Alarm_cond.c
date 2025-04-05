#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <stdbool.h>

#define MAX_ALARMS_PER_THREAD 2
#define CIRCULAR_BUFFER_SIZE 4


//VERYfinal
//alarm structure
//global alarm
typedef struct alarm_tag {
    struct alarm_tag *link;
    int seconds;
    int remaining_sec;
    time_t time;
    char message[128];
    int alarm_id;
    int suspend_status; //ADDED
    time_t timestamp;  //ADDED
    char request_type[20];  //ADDED
    int alarm_request;
    int group_id; // Added group_id
    int interval; // Added interval
    time_t last_printed; // Added last printed time
    int changed_group; // Added changed group flag
    int message_changed; // Add this
    int interval_changed; // Add this
    int cancelled;
    int processed;
    int memory_owner;
    int suspended_printed;
    pthread_t display_thread_id;
} alarm_t;

alarm_t *change_alarm_list = NULL;

typedef struct display_thread {
    pthread_t thread_id;
    int alarm_count;
    alarm_t *alarms[MAX_ALARMS_PER_THREAD];
    struct display_thread *next;
    int group_id; // Added group_id
} display_thread_t;

pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;
alarm_t *alarm_list = NULL;
display_thread_t *display_threads = NULL;

// Circular buffer
alarm_t *circular_buffer[CIRCULAR_BUFFER_SIZE];
int buffer_head = 0;
int buffer_tail = 0;
int buffer_count = 0;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;

int most_recent_displayed_alarm_id = -1; // Shared variable

// Readers-Writers synchronization
sem_t read_count_mutex;
sem_t resource_mutex;
int read_count = 0;

void sort_alarms_by_time(alarm_t *alarms[], int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (alarms[j] != NULL && alarms[j + 1] != NULL && alarms[j]->time > alarms[j + 1]->time) {
                alarm_t *temp = alarms[j];
                alarms[j] = alarms[j + 1];
                alarms[j + 1] = temp;
            }
        }
    }
}

void *display_alarm_thread(void *arg) {
    display_thread_t *display_thread_data = (display_thread_t *)arg;
    int last_alarm_group_id = display_thread_data->group_id;
    // printf("DEBUG: display_alarm_thread started\n");
    // printf("DEBUG: display_thread_data address: %p\n", (void *)display_thread_data);
    // if (display_thread_data != NULL) {
    //     printf("DEBUG: display_thread_data->alarm_count: %d\n",
    //            display_thread_data->alarm_count);
    //     printf("DEBUG: display_thread_data->group_id: %d\n",
    //            display_thread_data->group_id);
    //     if (display_thread_data->alarm_count > 0) {
    //         printf("DEBUG: display_thread_data->alarms[0]: %p\n",
    //                (void *)display_thread_data->alarms[0]);
    //     }
    // } else {
    //     printf("DEBUG: display_thread_data is NULL!\n");
    //     pthread_exit(NULL);
    // }

    while (1) {
        time_t current_time = time(NULL);
        pthread_mutex_lock(&alarm_mutex); // Protect shared data

        int active_alarms = 0; // To track active alarms for thread exit

        // Iterate through the alarms assigned to this thread
        for (int i = 0; i < display_thread_data->alarm_count; i++) {
            alarm_t *alarm = display_thread_data->alarms[i];
            if (alarm == NULL) continue; // Skip NULL entries

            // 1. Check for Cancellation
            if (alarm->cancelled == 1) {
                printf("Alarm(%d) Cancelled, freeing memory.\n", alarm->alarm_id);
                free(alarm);
                display_thread_data->alarms[i] = NULL;
                continue;
            }

            // 2. Check for Suspension
            if (alarm->suspend_status == 1) {
                printf("Alarm(%d) is suspended. Skipping print.\n", alarm->alarm_id);
                continue;
            }

            if (alarm->last_printed != 0 && current_time < alarm->last_printed + alarm->interval) {
                // Not time to print yet, skip this alarm
                active_alarms++;
                continue;
            }
            // 3. Check for Expiration
            if (alarm->time <= current_time) {
                if (alarm->memory_owner == 1) { // Only free if we own it
                    // printf("DEBUG: Alarm Expired - alarm_id: %d\n", alarm->alarm_id);
                    // printf("DEBUG: Alarm Expired - alarm address: %p\n", (void *)alarm);
                    // printf("DEBUG: Alarm Expired - current time: %ld\n", current_time);
                    // printf("DEBUG: Alarm Expired - alarm time: %ld\n", alarm->time);
                    // printf("Display Alarm Thread %ld Stopped Printing Expired Alarm(%d) at %ld\n",
                    //        pthread_self(), alarm->alarm_id, current_time);

                    free(alarm);
                    //printf("DEBUG: Alarm Freed - alarm address: %p\n", (void *)alarm);
                    display_thread_data->alarms[i] = NULL;
                    alarm = NULL;
                } else {
                    //printf("DEBUG: Alarm Expired - but not freeing because global list owns it.\n");
                    display_thread_data->alarms[i] = NULL; // Just remove from our list
                    alarm = NULL;
                }
                continue;
            }

            // 4. Handle Changes (Group, Message, Interval)
            if (alarm->changed_group == 1) {
                printf("Display Thread %ld Has Stopped Printing Message of Alarm(%d) at %ld: Changed Group(%d)\n",
                       pthread_self(), alarm->alarm_id, current_time, alarm->group_id);
                alarm->time = current_time + alarm->seconds;
                alarm->changed_group = 0;
                alarm->last_printed = current_time;
            }

            if (alarm->message_changed == 1) {
                printf("Display Thread %ld Starts to Print Changed Message Alarm(%d) at %ld: Group(%d) %ld %s\n",
                       pthread_self(), alarm->alarm_id, current_time, display_thread_data->group_id, current_time, alarm->message);
                alarm->message_changed = 0;
                alarm->last_printed = current_time;
            }

            if (alarm->interval_changed == 1) {
                printf("Display Thread %ld Starts to Print Changed Interval Value Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                       pthread_self(), alarm->alarm_id, current_time, display_thread_data->group_id, current_time, alarm->interval, alarm->message);
                alarm->interval_changed = 0;
                alarm->last_printed = current_time;
            }

            // 5. Normal Printing (if interval elapsed)
            if (alarm->last_printed == 0 || current_time - alarm->last_printed >= alarm->interval) {
                printf("Alarm (%d) Printed by Alarm Display Thread %ld at %ld: Group(%d) %s\n",
                       alarm->alarm_id, pthread_self(), current_time, alarm->group_id, alarm->message);
                alarm->last_printed = current_time;
            }

            active_alarms++; // Count if it wasn't cancelled, suspended, or expired
            last_alarm_group_id = alarm->group_id;
        }

        // 6. Compact the array
        int new_count = 0;
        for (int i = 0; i < display_thread_data->alarm_count; i++) {
            if (display_thread_data->alarms[i] != NULL) {
                display_thread_data->alarms[new_count] = display_thread_data->alarms[i];
                new_count++;
            }
        }
        display_thread_data->alarm_count = new_count;

        // 7. Check for thread exit based on active alarms
        if (active_alarms == 0) {
            printf("No more active alarms in Group(%d): Display Thread %ld exiting at %ld\n",
                   last_alarm_group_id, pthread_self(), current_time);
            free(display_thread_data);
            pthread_mutex_unlock(&alarm_mutex);
            pthread_exit(NULL);
        }

        pthread_mutex_unlock(&alarm_mutex);
        sleep(1);
    }
    return NULL;
}


void *start_alarm_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex);
        alarm_t *alarm = alarm_list;
        alarm_t *prev = NULL;

        while (alarm != NULL) {
            if (strcmp(alarm->request_type, "Start_Alarm") == 0 && !alarm->processed) {
                display_thread_t *assigned_thread = NULL;
                display_thread_t *current_thread = display_threads;

                while (current_thread != NULL) {
                    if (pthread_equal(current_thread->thread_id, alarm->display_thread_id)) {
                        assigned_thread = current_thread;
                        break;
                    }
                    current_thread = current_thread->next;
                }

                if (assigned_thread == NULL) {
                    current_thread = display_threads;
                    while (current_thread != NULL) {
                        if (current_thread->group_id == alarm->group_id &&
                            current_thread->alarm_count < MAX_ALARMS_PER_THREAD) {
                            assigned_thread = current_thread;
                            break;
                        }
                        current_thread = current_thread->next;
                    }

                    if (assigned_thread == NULL) {
                        assigned_thread = malloc(sizeof(display_thread_t));
                        if (assigned_thread == NULL) {
                            perror("Failed to allocate memory for display thread");
                            pthread_mutex_unlock(&alarm_mutex);
                            sleep(1);
                            continue;
                        }

                        assigned_thread->group_id = alarm->group_id;
                        assigned_thread->alarm_count = 0;
                        assigned_thread->next = display_threads;
                        display_threads = assigned_thread;

                        if (pthread_create(&assigned_thread->thread_id, NULL,
                                           display_alarm_thread, assigned_thread) != 0) {
                            perror("Failed to create display thread");
                            free(assigned_thread);
                            assigned_thread = NULL;
                            pthread_mutex_unlock(&alarm_mutex);
                            sleep(1);
                            continue;
                        }
                        time_t current_time = time(NULL);
                        //Corrected print statement
                        printf("Start Alarm Thread Created New Display Alarm Thread %ld For Alarm(%d) at %ld: Group(%d)\n",
                               assigned_thread->thread_id, alarm->alarm_id, current_time, alarm->group_id);
                    }
                }
                time_t current_time = time(NULL);
                //Corrected print statement
                printf("Alarm (%d) Assigned to Display Thread (%ld) at %ld: Group(%d)\n",
                       alarm->alarm_id, assigned_thread->thread_id, current_time, alarm->group_id);

                assigned_thread->alarms[assigned_thread->alarm_count++] = alarm;
                alarm->display_thread_id = assigned_thread->thread_id;
                alarm->processed = 1; 
                alarm->memory_owner = 1;
                // if (prev == NULL) {
                //     alarm_list = alarm->link;
                // } else {
                //     prev->link = alarm->link;
                // }
                break;
            }
            prev = alarm;
            alarm = alarm->link;
        }
        pthread_mutex_unlock(&alarm_mutex);
        sleep(1);
    }
    return NULL;
}

void *change_alarm_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex);
        alarm_t *current_change_alarm = change_alarm_list;
        alarm_t *prev_change_alarm = NULL;
        time_t current_time = time(NULL);

        while (current_change_alarm != NULL) {
            alarm_t *next_change_alarm = current_change_alarm->link;
            alarm_t *target_start_alarm = NULL;
            //update global alarm list 
            alarm_t *global_alarm = alarm_list;
            while (global_alarm != NULL) {
                if (global_alarm->alarm_id == current_change_alarm->alarm_id &&
                    strcmp(global_alarm->request_type, "Start_Alarm") == 0 &&
                    global_alarm->timestamp < current_change_alarm->timestamp) {
                    target_start_alarm = global_alarm;
                    break;
                }
                global_alarm = global_alarm->link;
            }
            // Search display threads for the target Start_Alarm
            display_thread_t *current_thread = display_threads;
            while (current_thread != NULL) {
                for (int i = 0; i < current_thread->alarm_count; i++) {
                    alarm_t *alarm = current_thread->alarms[i];
                    if (alarm && alarm->alarm_id == current_change_alarm->alarm_id &&
                        strcmp(alarm->request_type, "Start_Alarm") == 0 &&
                        alarm->timestamp < current_change_alarm->timestamp) {
                        target_start_alarm = alarm;
                        break;
                    }
                }
                if (target_start_alarm != NULL) {
                    break; // Target found, no need to check other threads
                }
                current_thread = current_thread->next;
            }

            if (target_start_alarm != NULL) {
                // Update the Start_Alarm request
                target_start_alarm->time = current_change_alarm->time;
                target_start_alarm->seconds = current_change_alarm->seconds;

                // Update the message safely
                 if (strcmp(target_start_alarm->message, current_change_alarm->message) != 0) {
                    strncpy(target_start_alarm->message, current_change_alarm->message, sizeof(target_start_alarm->message) - 1);
                    target_start_alarm->message[sizeof(target_start_alarm->message) - 1] = '\0';
                    target_start_alarm->message_changed = 1;
                    printf("Change Alarm Thread Has Changed Alarm(%d) Message at %ld: Group(%d) Message(%s)\n",
                           target_start_alarm->alarm_id, current_time, target_start_alarm->group_id, target_start_alarm->message);
                }

                // Check if interval changed
                if (target_start_alarm->interval != current_change_alarm->interval) {
                    target_start_alarm->interval = current_change_alarm->interval;
                    target_start_alarm->interval_changed = 1;
                    printf("Change Alarm Thread Has Changed Alarm(%d) Interval at %ld: New Interval(%d)\n",
                           target_start_alarm->alarm_id, current_time, target_start_alarm->interval);
                }

                target_start_alarm->group_id = current_change_alarm->group_id;
                target_start_alarm->interval = current_change_alarm->interval; // Corrected line
                target_start_alarm->changed_group = 1;

                printf("Change Alarm Thread Has Changed Alarm(%d) at %ld: Group(%d) Message(%s)\n",
                       target_start_alarm->alarm_id, current_time, target_start_alarm->group_id, target_start_alarm->message);
                printf("Updated_Interval: %d\n", target_start_alarm->interval);

            } else {
                printf("Invalid Change Alarm Request(%d) at %ld: Group(%d)\n",
                       current_change_alarm->alarm_id, current_time, current_change_alarm->group_id);
            }

            // Remove the Change_Alarm request from the change_alarm_list
            if (prev_change_alarm == NULL) {
                change_alarm_list = next_change_alarm;
            } else {
                prev_change_alarm->link = next_change_alarm;
            }
            free(current_change_alarm);
            current_change_alarm = next_change_alarm;
        }

        pthread_mutex_unlock(&alarm_mutex);
        sleep(1);
    }
    return NULL;
}

void *cancel_alarm_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex);
        alarm_t *current_alarm = alarm_list;
        alarm_t *prev_alarm = NULL;
        time_t current_time = time(NULL);

        while (current_alarm != NULL) {
            alarm_t *next_alarm = current_alarm->link;

            if (strcmp(current_alarm->request_type, "Cancel_Alarm") == 0) {
                alarm_t *target_start_alarm = NULL;

                // Find the corresponding Start_Alarm with an earlier timestamp
                alarm_t *temp_alarm = alarm_list;
                while (temp_alarm != NULL) {
                    if (temp_alarm->alarm_id == current_alarm->alarm_id &&
                        strcmp(temp_alarm->request_type, "Start_Alarm") == 0 &&
                        temp_alarm->timestamp < current_alarm->timestamp) {
                        target_start_alarm = temp_alarm;
                        break;
                    }
                    temp_alarm = temp_alarm->link;
                }

                if (target_start_alarm != NULL) {
                    // Start_Alarm found with earlier timestamp
                    printf(
                        "Alarm(%d) Cancelled and Removed from Global List at %ld: "
                        "Group(%d) %ld %d %ld %s\n",
                        current_alarm->alarm_id, current_time,
                        target_start_alarm->group_id, target_start_alarm->timestamp,
                        target_start_alarm->interval, target_start_alarm->time,
                        target_start_alarm->message);

                    // Mark the Start_Alarm for cancellation in the display threads
                    display_thread_t *curr_display_thread = display_threads;
                    while (curr_display_thread != NULL) {
                        for (int i = 0; i < curr_display_thread->alarm_count; i++) {
                            if (curr_display_thread->alarms[i] == target_start_alarm) {
                                curr_display_thread->alarms[i]->cancelled = 1;
                                break;
                            }
                        }
                        curr_display_thread = curr_display_thread->next;
                    }

                    // Remove the Start_Alarm from the global list
                    if (prev_alarm == NULL) {
                        alarm_list = target_start_alarm->link;
                    } else {
                        prev_alarm->link = target_start_alarm->link;
                    }

                    // Remove the Cancel_Alarm request
                    alarm_t *temp = alarm_list;
                    alarm_t *prev = NULL;
                    while (temp != NULL) {
                        if (temp == current_alarm) {
                            if (prev == NULL) {
                                alarm_list = temp->link;
                            } else {
                                prev->link = temp->link;
                            }
                            break;
                        }
                        prev = temp;
                        temp = temp->link;
                    }
                    free(current_alarm); // Free the Cancel_Alarm request

                    current_alarm = next_alarm;
                    continue;
                } else {
                    printf("Cancel Alarm Thread: Alarm(%d) not found.\n",
                           current_alarm->alarm_id);
                }

                // Remove the Cancel_Alarm request even if no matching Start_Alarm was found
                if (prev_alarm == NULL) {
                    alarm_list = next_alarm;
                } else {
                    prev_alarm->link = next_alarm;
                }
                free(current_alarm);
                current_alarm = next_alarm;
                continue;
            } else if (strcmp(current_alarm->request_type, "Start_Alarm") == 0 &&
                       current_alarm->time <= current_time) {
                // Start_Alarm expired - Remove from global list
                // **CRITICAL CHANGE:** Do NOT free here. Let display thread handle it.
                printf(
                    "Alarm(%d) Expired and Removed from Global List at %ld (but not freed): "
                    "Group(%d) %ld %d %ld %s\n",
                    current_alarm->alarm_id, current_time, current_alarm->group_id,
                    current_alarm->timestamp, current_alarm->interval,
                    current_alarm->time, current_alarm->message);

                if (prev_alarm == NULL) {
                    alarm_list = next_alarm;
                } else {
                    prev_alarm->link = next_alarm;
                }
                //free(current_alarm); // Do NOT free here!
                current_alarm = next_alarm;
                continue;
            }

            prev_alarm = current_alarm;
            current_alarm = next_alarm;
        next_iteration:;
        }
        pthread_mutex_unlock(&alarm_mutex);
        sleep(1);
    }
    return NULL;
}

void *suspend_reactivate_alarm_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex);
        alarm_t *current_alarm = alarm_list;
        alarm_t *prev_alarm = NULL;
        time_t current_time = time(NULL);

        while (current_alarm != NULL) {
            alarm_t *next_alarm = current_alarm->link;

            if (strcmp(current_alarm->request_type, "Suspend_Alarm") == 0) {
                alarm_t *target_alarm = alarm_list;
                while (target_alarm != NULL) {
    printf("DEBUG: Target Alarm ID: %d, Current Alarm ID: %d\n", target_alarm->alarm_id, current_alarm->alarm_id);
    printf("DEBUG: Target Alarm Request: %s, Current Alarm Request: %s\n", target_alarm->request_type, current_alarm->request_type);
    printf("DEBUG: Target Alarm Timestamp: %ld, Current Alarm Timestamp: %ld\n", target_alarm->timestamp, current_alarm->timestamp);

    if (target_alarm->alarm_id == current_alarm->alarm_id &&
        strcmp(target_alarm->request_type, "Start_Alarm") == 0 &&
        target_alarm->timestamp < current_alarm->timestamp) {

        printf("DEBUG: Match Found - Target Alarm ID: %d\n", target_alarm->alarm_id);

        target_alarm->suspend_status = 1;
        target_alarm->remaining_sec = target_alarm->time - current_time;

        if (target_alarm->suspended_printed == 0) {
            printf("Alarm(%d) Suspended at %ld: Group(%d) %ld %ld %s\n",
                   target_alarm->alarm_id, current_time, target_alarm->group_id,
                   target_alarm->timestamp, target_alarm->time, target_alarm->message);
            target_alarm->suspended_printed = 1;
        }
        break;
    }
    target_alarm = target_alarm->link;
}
                if (target_alarm == NULL) {
                    printf("Suspend Alarm Thread: Alarm(%d) not found.\n", current_alarm->alarm_id);
                }

                if (prev_alarm == NULL) {
                    alarm_list = next_alarm;
                } else {
                    prev_alarm->link = next_alarm;
                }
                free(current_alarm);
                current_alarm = next_alarm;
                continue;
            } else if (strcmp(current_alarm->request_type, "Reactivate_Alarm") == 0) {
                alarm_t *target_alarm = alarm_list;
                while (target_alarm != NULL) {
                    if (target_alarm->alarm_id == current_alarm->alarm_id &&
                        strcmp(target_alarm->request_type, "Start_Alarm") == 0 &&
                        target_alarm->timestamp < current_alarm->timestamp) {
                        target_alarm->suspend_status = 0;
                        target_alarm->time = current_time + target_alarm->remaining_sec;
                        target_alarm->remaining_sec = 0; // Reset remaining time
                        target_alarm->last_printed = current_time - target_alarm->interval;
                        printf("Alarm(%d) Reactivated at %ld: Group(%d) %ld %ld %s\n",
                               target_alarm->alarm_id, current_time, target_alarm->group_id,
                               target_alarm->timestamp, target_alarm->time, target_alarm->message);
                        break;
                    }
                    target_alarm = target_alarm->link;
                }
                if (target_alarm == NULL) {
                    printf("Reactivate Alarm Thread: Alarm(%d) not found.\n", current_alarm->alarm_id);
                }

                if (prev_alarm == NULL) {
                    alarm_list = next_alarm;
                } else {
                    prev_alarm->link = next_alarm;
                }
                free(current_alarm);
                current_alarm = next_alarm;
                continue;
            }
            prev_alarm = current_alarm;
            current_alarm = next_alarm;
        }

        pthread_mutex_unlock(&alarm_mutex);
        sleep(1);
    }
    return NULL;
}

//start_alarm_request processing
void start_alarm(char *line) {
    int status;
    alarm_t *alarm, **last, *next;

    // Memory allocation
    alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (alarm == NULL) {
        perror("Allocate alarm");
        return;
    }

    //Modified scanf to include interval
    if (sscanf(line, "Start_Alarm(%d): %d %d %d %64[^\n]", &alarm->alarm_id, &alarm->group_id, &alarm->seconds, &alarm->interval, alarm->message) < 5) {
        fprintf(stderr, "Bad command\n");
        free(alarm);
        return;
    } else {
        // Mutex lock before insertion
        alarm->timestamp = time(NULL);
        alarm->suspend_status = 0;
        alarm->suspended_printed = 0; // Initialize suspended_printed
        strcpy(alarm->request_type, "Start_Alarm");
        alarm->time = time(NULL) + alarm->seconds; // Total duration
        alarm->last_printed = 0;
        alarm->changed_group = 0;
        alarm->message_changed = 0;
        alarm->interval_changed = 0;
        alarm->cancelled = 0;

        status = pthread_mutex_lock(&alarm_mutex);
        if (status != 0) {
            perror("Lock mutex");
            free(alarm);
            return;
        }

        // Checking for uniqueness of alarm_id
        next = alarm_list;
        while (next != NULL) {
            if (next->alarm_id == alarm->alarm_id) {
                printf("Error: Alarm ID %d is already in use.\n", alarm->alarm_id);
                free(alarm);
                pthread_mutex_unlock(&alarm_mutex);
                return;
            }
            next = next->link;
        }

        // Insertion process
        last = &alarm_list;
        next = *last;
        while (next != NULL && next->time < alarm->time) {
            last = &next->link;
            next = next->link;
        }

        alarm->link = *last;
        *last = alarm;
        printf("Start_Alarm: alarm_list address after adding: %p\n", (void *)alarm_list);

        // Printing confirmation
        printf("Start_Alarm(%d) Request Inserted Into Alarm List: %d %d %s\n", alarm->alarm_id, alarm->time, alarm->interval, alarm->message);

        // DEBUG
#ifdef DEBUG
        printf("[list: ");
        for (next = alarm_list; next != NULL; next = next->link) {
            time_t now = time(NULL);
            printf("(%d sec) [\"%s\"] ", (int)(next->time - now), next->message);
        }
        printf("]\n");
#endif

        // Unlock mutex after successful insertion
        status = pthread_mutex_unlock(&alarm_mutex);
        if (status != 0) {
            perror("Unlock mutex");
            free(alarm);
            return;
        }
    }
}

void remove_processed_alarms() {
    pthread_mutex_lock(&alarm_mutex);
    alarm_t *alarm = alarm_list;
    alarm_t *prev = NULL;

    while (alarm != NULL) {
        if (alarm->processed) {
            // Remove processed alarm from the list
            if (prev == NULL) {
                alarm_list = alarm->link;  // Removing head of the list
            } else {
                prev->link = alarm->link;  // Removing from the middle or end
            }
            free(alarm);  // Free the alarm memory
            alarm = (prev == NULL) ? alarm_list : prev->link;
        } else {
            prev = alarm;
            alarm = alarm->link;
        }
    }
    pthread_mutex_unlock(&alarm_mutex);
}

void change_alarm(char *line) {
    int status;
    alarm_t *new_alarm;

    new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (new_alarm == NULL) {
        perror("Allocate alarm");
        return;
    }

    // Modified scanf to include interval
    if (sscanf(line, "Change_Alarm(%d): %d %d %d %64[^\n]", &new_alarm->alarm_id, &new_alarm->group_id, &new_alarm->seconds, &new_alarm->interval, new_alarm->message) < 5) {
        fprintf(stderr, "Bad command\n");
        free(new_alarm);
        return;
    }

    new_alarm->timestamp = time(NULL);
    new_alarm->suspend_status = 0;
    strcpy(new_alarm->request_type, "Change_Alarm");
    new_alarm->time = time(NULL) + new_alarm->seconds;
    new_alarm->last_printed = 0;
    new_alarm->changed_group = 0;
    new_alarm->message_changed = 0;
    new_alarm->interval_changed = 0;
    new_alarm->cancelled = 0;

    status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0) {
        perror("Lock mutex");
        free(new_alarm);
        return;
    }

    new_alarm->link = change_alarm_list;
    change_alarm_list = new_alarm;

    printf("Change_Alarm(%d) Request Inserted Into Change Alarm List: %d %d %s\n", new_alarm->alarm_id, new_alarm->time, new_alarm->interval, new_alarm->message);

    printf("[change list: ");
    alarm_t *next;
    for (next = change_alarm_list; next != NULL; next = next->link) {
        time_t now = time(NULL);
        printf("(%d sec) [\"%s\"] ", (int)(next->time - now), next->message);
    }
    printf("]\n");




    status = pthread_mutex_unlock(&alarm_mutex);
    if (status != 0) {
        perror("Unlock mutex");
        free(new_alarm);
        return;
    }
}

void cancel_alarm(char *line) {
    int status;
    alarm_t *new_alarm, **last, *next;

    new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (new_alarm == NULL) {
        perror("Allocate alarm");
        return;
    }

    if (sscanf(line, "Cancel_Alarm(%d)", &new_alarm->alarm_id) < 1) {
        fprintf(stderr, "Bad command\n");
        free(new_alarm);
        return;
    }

    new_alarm->timestamp = time(NULL);
    strcpy(new_alarm->request_type, "Cancel_Alarm");
    new_alarm->cancelled = 0;

    status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0) {
        perror("Lock mutex");
        free(new_alarm);
        return;
    }

    last = &alarm_list;
    next = *last;
    while (next != NULL && next->time < new_alarm->time) {
        last = &next->link;
        next = next->link;
    }

    new_alarm->link = *last;
    *last = new_alarm;

    printf("Cancel_Alarm(%d) Request Inserted Into Alarm List\n", new_alarm->alarm_id);

#ifdef DEBUG
    printf("[list: ");
    for (next = alarm_list; next != NULL; next = next->link) {
        time_t now = time(NULL);
        printf("(%d sec) [\"%s\"] ", (int)(next->time - now), next->message);
    }
    printf("]\n");
#endif

    status = pthread_mutex_unlock(&alarm_mutex);
    if (status != 0) {
        perror("Unlock mutex");
        free(new_alarm);
        return;
    }
}

void suspend_alarm(char *line) {
    int status;
    alarm_t *new_alarm, **last, *next;

    new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (new_alarm == NULL) {
        perror("Allocate alarm");
        return;
    }

    if (sscanf(line, "Suspend_Alarm(%d)", &new_alarm->alarm_id) < 1) {
        fprintf(stderr, "Bad command\n");
        free(new_alarm);
        return;
    }

    new_alarm->timestamp = time(NULL);
    strcpy(new_alarm->request_type, "Suspend_Alarm");
    new_alarm->cancelled = 0;

    status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0) {
        perror("Lock mutex");
        free(new_alarm);
        return;
    }

    last = &alarm_list;
    next = *last;
    while (next != NULL && next->time < new_alarm->time) {
        last = &next->link;
        next = next->link;
    }

    new_alarm->link = *last;
    *last = new_alarm;

    printf("Suspend_Alarm(%d) Request Inserted Into Alarm List\n", new_alarm->alarm_id);

#ifdef DEBUG
    printf("[list: ");
    for (next = alarm_list; next != NULL; next = next->link) {
        time_t now = time(NULL);
        printf("(%d sec) [\"%s\"] ", (int)(next->time - now), next->message);
    }
    printf("]\n");
#endif

    status = pthread_mutex_unlock(&alarm_mutex);
    if (status != 0) {
        perror("Unlock mutex");
        free(new_alarm);
        return;
    }
}

void reactivate_alarm(char *line) {
    int status;
    alarm_t *new_alarm, **last, *next;

    new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (new_alarm == NULL) {
        perror("Allocate alarm");
        return;
    }

    if (sscanf(line, "Reactivate_Alarm(%d)", &new_alarm->alarm_id) < 1) {
        fprintf(stderr, "Bad command\n");
        free(new_alarm);
        return;
    }

    new_alarm->timestamp = time(NULL);
    strcpy(new_alarm->request_type, "Reactivate_Alarm");
    new_alarm->cancelled = 0;

    status = pthread_mutex_lock(&alarm_mutex);
    if (status != 0) {
        perror("Lock mutex");
        free(new_alarm);
        return;
    }

    last = &alarm_list;
    next = *last;
    while (next != NULL && next->time < new_alarm->time) {
        last = &next->link;
        next = next->link;
    }

    new_alarm->link = *last;
    *last = new_alarm;

    printf("Reactivate_Alarm(%d) Request Inserted Into Alarm List\n", new_alarm->alarm_id);

#ifdef DEBUG
    printf("[list: ");
    for (next = alarm_list; next != NULL; next = next->link) {
        time_t now = time(NULL);
        printf("(%d sec) [\"%s\"] ", (int)(next->time - now), next->message);
    }
    printf("]\n");
#endif

    status = pthread_mutex_unlock(&alarm_mutex);
    if (status != 0) {
        perror("Unlock mutex");
        free(new_alarm);
        return;
    }
}

void view_alarms(char *line) {
    alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
    new_alarm->time = time(NULL);
    new_alarm->alarm_request = 1;
    strcpy(new_alarm->message, "View Alarms Request");
    strcpy(new_alarm->request_type, "View_Alarms");
    new_alarm->cancelled = 0;
    pthread_mutex_lock(&alarm_mutex);
    new_alarm->link = alarm_list;
    alarm_list = new_alarm;
    pthread_mutex_unlock(&alarm_mutex);

    printf("View_Alarms Request Inserted Into Alarm List\n");
}

void *view_alarms_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&alarm_mutex);
        alarm_t *current_alarm = alarm_list;
        alarm_t *prev_alarm = NULL;
        time_t view_time = time(NULL);

        while (current_alarm != NULL) {
            alarm_t *next_alarm = current_alarm->link;

            if (strcmp(current_alarm->request_type, "View_Alarms") == 0) {
                printf("View Alarms at View Time %ld:\n", view_time);
                int count = 1;

                alarm_t *temp_alarm = alarm_list;
                while (temp_alarm != NULL) {
                    if (strcmp(temp_alarm->request_type, "Start_Alarm") == 0) {
                        display_thread_t *display_thread = display_threads;
                        pthread_t assigned_thread_id = 0; // Initialize to 0
                        bool thread_found = false;

                        while (display_thread != NULL) {
                            for (int i = 0; i < display_thread->alarm_count; i++) {
                                if (display_thread->alarms[i] != NULL && display_thread->alarms[i]->alarm_id == temp_alarm->alarm_id) {
                                    assigned_thread_id = display_thread->thread_id;
                                    thread_found = true;
                                    break;
                                }
                            }
                            if (thread_found) break;
                            display_thread = display_thread->next;
                        }

                        if (thread_found) {
                            printf("%d. Alarm(%d): Group(%d) Status %d Assigned Display Thread %lu\n",
                                   count++, temp_alarm->alarm_id, temp_alarm->group_id, temp_alarm->suspend_status, assigned_thread_id);
                        } else {
                            printf("%d. Alarm(%d): Group(%d) Status %d Assigned Display Thread (Not Found)\n",
                                   count++, temp_alarm->alarm_id, temp_alarm->group_id, temp_alarm->suspend_status);
                        }
                    }
                    temp_alarm = temp_alarm->link;
                }

                printf("View Alarms request %ld Alarm Requests Viewed at View Time %ld printed by View Alarms Thread %lu\n",
                       current_alarm->timestamp, view_time, pthread_self());

                if (prev_alarm == NULL) {
                    alarm_list = next_alarm;
                } else {
                    prev_alarm->link = next_alarm;
                }
                free(current_alarm);
                current_alarm = next_alarm;
                break;
            }
            prev_alarm = current_alarm;
            current_alarm = next_alarm;
        }

        pthread_mutex_unlock(&alarm_mutex);
        sleep(1);
    }
    return NULL;
}
// Circular buffer functions
void insert_into_buffer(alarm_t *alarm) {
    pthread_mutex_lock(&buffer_mutex);
    while (buffer_count == CIRCULAR_BUFFER_SIZE) {
        pthread_cond_wait(&buffer_not_full, &buffer_mutex);
    }
    circular_buffer[buffer_tail] = alarm;
    buffer_tail = (buffer_tail + 1) % CIRCULAR_BUFFER_SIZE;
    buffer_count++;
    pthread_cond_signal(&buffer_not_empty);
    printf("Alarm Thread has Inserted %s Request(%d) at %ld into Circular_Buffer Index: %d\n",
           alarm->request_type, alarm->alarm_id, alarm->timestamp, (buffer_tail - 1 + CIRCULAR_BUFFER_SIZE) % CIRCULAR_BUFFER_SIZE);
    pthread_mutex_unlock(&buffer_mutex);
}

alarm_t *retrieve_from_buffer() {
    pthread_mutex_lock(&buffer_mutex);
    while (buffer_count == 0) {
        pthread_cond_wait(&buffer_not_empty, &buffer_mutex);
    }
    alarm_t *alarm = circular_buffer[buffer_head];
    buffer_head = (buffer_head + 1) % CIRCULAR_BUFFER_SIZE;
    buffer_count--;
    pthread_cond_signal(&buffer_not_full);
    printf("Consumer Thread has Retrieved %s Request(%d) at %ld from Circular_Buffer Index: %d\n",
           alarm->request_type, alarm->alarm_id, alarm->timestamp, (buffer_head - 1 + CIRCULAR_BUFFER_SIZE) % CIRCULAR_BUFFER_SIZE);
    pthread_mutex_unlock(&buffer_mutex);
    return alarm;
}

void *consumer_thread(void *arg) {
    while (1) {
        alarm_t *alarm = retrieve_from_buffer();
        if (alarm == NULL) {
            sleep(1);
            continue;
        }
        char line[256]; // Assuming a maximum line length of 256
        snprintf(line, sizeof(line), "%s(%d): %d %d %s",
                 alarm->request_type, alarm->alarm_id, alarm->group_id,
                 alarm->seconds, alarm->message);

        if (strcmp(alarm->request_type, "Start_Alarm") == 0) {
            start_alarm(line);
        } else if (strcmp(alarm->request_type, "Change_Alarm") == 0) {
            change_alarm(line);
        } else if (strcmp(alarm->request_type, "Cancel_Alarm") == 0) {
            cancel_alarm(line);
        } else if (strcmp(alarm->request_type, "Suspend_Alarm") == 0) {
            suspend_alarm(line);
        } else if (strcmp(alarm->request_type, "Reactivate_Alarm") == 0) {
            reactivate_alarm(line);
        } else if (strcmp(alarm->request_type, "View_Alarms") == 0) {
            view_alarms(line);
        }
        free(alarm);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int status;
    char line[128];
    pthread_t display_thread;
    pthread_t view_thread;
    pthread_t consumer_thread_id;
    pthread_t start_alarm_tid, change_alarm_tid, cancel_alarm_tid, suspend_reactivate_tid;
    display_thread_t *display_thread_data = (display_thread_t *)malloc(sizeof(display_thread_t));
    if (display_thread_data == NULL) {
        perror("Allocate display thread");
        return 1;
    }

    display_thread_data->alarm_count = 0;
    memset(display_thread_data->alarms, 0, sizeof(display_thread_data->alarms));
    display_thread_data->next = NULL;

    status = pthread_create(&display_thread, NULL, display_alarm_thread, display_thread_data);
    if (status != 0) {
        perror("Create display alarm thread");
        return 1;
    }

    printf("Display Alarm Thread %lu Created at %ld\n", display_thread, time(NULL));
    pthread_create(&consumer_thread_id, NULL, consumer_thread, NULL);
    printf("Consumer Thread %lu Created at %ld\n", consumer_thread_id, time(NULL));
    pthread_create(&view_thread, NULL, view_alarms_thread, NULL);
    printf("View Alarms Thread %lu Created at %ld\n", view_thread, time(NULL));
    pthread_create(&start_alarm_tid, NULL, start_alarm_thread, NULL);
    pthread_create(&change_alarm_tid, NULL, change_alarm_thread, NULL);
    pthread_create(&cancel_alarm_tid, NULL, cancel_alarm_thread, NULL);
    pthread_create(&suspend_reactivate_tid, NULL, suspend_reactivate_alarm_thread, NULL);

    sem_init(&read_count_mutex, 0, 1);
    sem_init(&resource_mutex, 0, 1);

    while (1) {
        printf("alarm> ");

        if (fgets(line, sizeof(line), stdin) == NULL) exit(0);

        if (strlen(line) <= 1) continue;

        if (strncmp(line, "Start_Alarm", 11) == 0) {
            alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
            if (new_alarm == NULL) {
                perror("Allocate alarm");
                continue;
            }
            if (sscanf(line, "Start_Alarm(%d): %d %d %64[^\n]", &new_alarm->alarm_id, &new_alarm->group_id, &new_alarm->seconds, new_alarm->message) < 4) {
                fprintf(stderr, "Bad command\n");
                free(new_alarm);
                continue;
            }

            new_alarm->timestamp = time(NULL);
            new_alarm->suspend_status = 0;
            strcpy(new_alarm->request_type, "Start_Alarm");
            new_alarm->time = time(NULL) + new_alarm->seconds;
            new_alarm->interval = new_alarm->seconds; // Initialize interval
            new_alarm->last_printed = 0;
            new_alarm->changed_group = 0;

            insert_into_buffer(new_alarm);
        } else if (strncmp(line, "Change_Alarm", 12) == 0) {
            alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
            if (new_alarm == NULL) {
                perror("Allocate alarm");
                continue;
            }
            if (sscanf(line, "Change_Alarm(%d): %d %d %64[^\n]", &new_alarm->alarm_id, &new_alarm->group_id, &new_alarm->seconds, new_alarm->message) < 4) {
                fprintf(stderr, "Bad command\n");
                free(new_alarm);
                continue;
            }

            new_alarm->timestamp = time(NULL);
            new_alarm->suspend_status = 0;
            strcpy(new_alarm->request_type, "Change_Alarm");
            new_alarm->time = time(NULL) + new_alarm->seconds;
            new_alarm->interval = new_alarm->seconds; // Initialize interval
            new_alarm->last_printed = 0;
            new_alarm->changed_group = 0;

            insert_into_buffer(new_alarm);
        } else if (strncmp(line, "Cancel_Alarm", 12) == 0) {
            alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
            if (new_alarm == NULL) {
                perror("Allocate alarm");
                continue;
            }
            if (sscanf(line, "Cancel_Alarm(%d)", &new_alarm->alarm_id) < 1) {
                fprintf(stderr, "Bad command\n");
                free(new_alarm);
                continue;
            }

            new_alarm->timestamp = time(NULL);
            strcpy(new_alarm->request_type, "Cancel_Alarm");

            insert_into_buffer(new_alarm);
        } else if (strncmp(line, "Suspend_Alarm", 13) == 0
    ) {
            alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
            if (new_alarm == NULL) {
                perror("Allocate alarm");
                continue;
            }
            if (sscanf(line, "Suspend_Alarm(%d)", &new_alarm->alarm_id) < 1) {
                fprintf(stderr, "Bad command\n");
                free(new_alarm);
                continue;
            }

            new_alarm->timestamp = time(NULL);
            strcpy(new_alarm->request_type, "Suspend_Alarm");

            insert_into_buffer(new_alarm);
        } else if (strncmp(line, "Reactivate_Alarm", 16) == 0) {
            alarm_t *new_alarm = (alarm_t *)malloc(sizeof(alarm_t));
            if (new_alarm == NULL) {
                perror("Allocate alarm");
                continue;
            }
            if (sscanf(line, "Reactivate_Alarm(%d)", &new_alarm->alarm_id) < 1) {
                fprintf(stderr, "Bad command\n");
                free(new_alarm);
                continue;
            }

            new_alarm->timestamp = time(NULL);
            strcpy(new_alarm->request_type, "Reactivate_Alarm");

            insert_into_buffer(new_alarm);
        } else if (strncmp(line, "View_Alarms", 11) == 0) {
            view_alarms(line);
        } else {
            fprintf(stderr, "Bad command\n");
        }
    }

    return 0;
}
