#include "scheduler.h"
#include "pubsub.h"
#include "btree.h"
#include <stddef.h> /* For NULL */
#include <stdlib.h> /* For malloc and free */
#include <ctype.h>  /* For isprint */
#include <rp6502.h>
#include <stdbool.h>
#include <fcntl.h>
#include <time.h>   /* For system clock timing */

/* Configuration flags - must come before conditional includes */
#define USE_PUBSUB_BTREE_ONLY 1

/* Move declarations to the top of the file */
static unsigned int _count1 = 0;
static unsigned int _count2 = 0;
static unsigned int _count3 = 0;

/* Task to simulate memory usage fluctuation using malloc */
#include "scheduler.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "string_helpers.h"

/* Pub/Sub Manager for multi-topic messaging */
static PubSubManager g_pubsub_mgr;

/* Approximate total RAM available for the OS (matches rp6502.cfg:
    RAM start = $0200, size = $FD00 - __STACKSIZE__ where __STACKSIZE__ is $0800
    So total bytes = 0xFD00 - 0x0800 = 62464
*/
static const unsigned int RAM_TOTAL_BYTES = 62464u;

/* Simple pseudo-random generator (linear congruential method) */
static unsigned int random_seed = 42u;

void print(char *s)
{
    while (*s)
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = *s++;
}

void seed_random(unsigned int val)
{
    random_seed = val ? val : 42u;
}

unsigned int pseudo_random(unsigned int min_val, unsigned int max_val)
{
    /* LCG: next = (a * seed + c) mod m */
    random_seed = (1103515245u * random_seed + 12345u) & 0x7FFFFFFFu;
    if (max_val <= min_val) return min_val;
    return min_val + (random_seed % (max_val - min_val + 1u));
}

/* string_helper functions moved to src/string_helpers.c */

void printValue(unsigned int value, char *description)
{
    char buf[16];        

    itoa_new(value, buf, sizeof(buf));
    puts(description);
    puts(buf);
}

/* forward declare fail_halt (defined after test globals so it can print them) */
static void fail_halt(const char *msg, unsigned int a, unsigned int b);

/* ========== TCP UART Helper Functions ========== */

/* Simple string search function */
static char* my_strstr(const char *haystack, const char *needle)
{
    const char *h, *n;
    
    if (!*needle)
        return (char*)haystack;
    
    while (*haystack)
    {
        h = haystack;
        n = needle;
        
        while (*h && *n && (*h == *n))
        {
            h++;
            n++;
        }
        
        if (!*n)
            return (char*)haystack;
        
        haystack++;
    }
    
    return NULL;
}

/* Simple sprintf for specific format strings */
static void my_sprintf(char *dest, const char *fmt, const char *s1, const char *s2)
{
    while (*fmt)
    {
        if (*fmt == '%' && *(fmt + 1) == 's')
        {
            const char *src = s1;
            while (*src)
                *dest++ = *src++;
            s1 = s2;
            fmt += 2;
        }
        else
        {
            *dest++ = *fmt++;
        }
    }
    *dest = '\0';
}

/* Simple delay function */
static void delay_ms(int ms)
{
    int i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 100; j++)
            ;
}

/* Idle task to keep ticks advancing when all other tasks are sleeping. */
static void idle_task(void *arg)
{
    (void)arg;
    for (;;) {
        scheduler_yield();
    }
}

/* Heuristic: treat value as string if it is non-NULL, null-terminated within a
   small bound, and all chars are printable or whitespace. */
static bool is_likely_string(const void *ptr)
{
    const char *s = (const char *)ptr;
    size_t i;

    if (!s)
        return false;

    for (i = 0; i < 128; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\0')
            return i > 0; /* found terminator within bound */
        if (!(isprint(c) || isspace(c)))
            return false;
    }

    return false; /* no terminator within bound */
}

static void on_rp6502(const char *topic, const PubSubMessage *message, void *user_data)
{
    if (is_likely_string(message->value)) {
        printf("[STATUS_SUBSCRIBER] Received on topic '%s': key=%d, text=%s\n",
               topic, message->key, (const char *)message->value);
    } else {
        unsigned long numeric = (unsigned long)message->value;
        printf("[STATUS_SUBSCRIBER] Received on topic '%s': key=%d, value=%lu\n",
               topic, message->key, numeric);
    }
    (void)user_data;
}

#if USE_PUBSUB_BTREE_ONLY == 1
/* B-tree instance for storing consumed messages from main topics */
static BTree *g_consumer_btree = NULL;
static unsigned int g_btree_insert_count = 0;

/* Separate B-tree instance for test items */
static BTree *g_test_btree = NULL;

static void on_rp6502_btree(const char *topic, const PubSubMessage *message, void *user_data)
{
    unsigned int key;
    
    /* Ensure btree is initialized */
    if (g_consumer_btree == NULL) {
        g_consumer_btree = btree_create();
        if (g_consumer_btree == NULL) {
            printf("[BTREE_SUBSCRIBER] FAILED to create btree\n");
            return;
        }
    }
    
    /* Use a unique key for each inserted message (use message key and a counter) */
    key = (message->key << 16) | (g_btree_insert_count & 0xFFFF);
    
    /* Insert the message value into the btree */
    btree_insert(g_consumer_btree, key, message->value);
    g_btree_insert_count++;
    
    /* Check if this is a string message (from rp6502_pub_3 with JSON) or numeric (from pub_1/pub_2) */
    if (is_likely_string(message->value)) {
        printf("[BTREE_SUBSCRIBER] Received on topic '%s': key=%d, text=%s, stored_in_btree with key=%u\n",
               topic, message->key, (const char *)message->value, key);
    } else {
        unsigned long numeric = (unsigned long)message->value;
        printf("[BTREE_SUBSCRIBER] Received on topic '%s': key=%d, value=%lu, stored_in_btree with key=%u\n",
               topic, message->key, numeric, key);
    }

    (void)user_data;
}
#endif

/* ========== Test Producer/Validator Tasks for BTree ========== */

#if USE_PUBSUB_BTREE_ONLY == 1
#define TEST_ITEM_COUNT 250
#define NUM_PRODUCERS 4
#define NUM_CONSUMERS 4
#define JSON_ITEM_COUNT 5   /* Number of items with JSON data */
#define MAX_JSON_SIZE 32    /* Max size for JSON strings */

/* Test item data structure - holds both numeric and JSON data */
struct TestItem {
    unsigned int numeric_value;
    char json_data[MAX_JSON_SIZE];
    unsigned char has_json;
};

static struct TestItem test_items[TEST_ITEM_COUNT];
static unsigned int test_items_produced = 0;
static unsigned int test_items_consumed = 0;
static unsigned char test_validation_complete = 0;
/* Shared producer index - only one producer takes an item at a time */
static unsigned int test_producer_index = 0;
/* Per-producer pending items (indexed by producer_id - 1) */
static int producer_pending_items[NUM_PRODUCERS];
static unsigned char producer_started[NUM_PRODUCERS];

/* Timing tracking */
static unsigned int time_test_started = 0;
static unsigned int time_first_produced = 0;
static unsigned int time_all_produced = 0;
static unsigned int time_all_consumed = 0;
static unsigned int time_validation_complete = 0;
static unsigned long cpu_ticks_at_start = 0;
static unsigned long cpu_ticks_at_end = 0;
static unsigned long active_ticks_at_start = 0;
static unsigned long active_ticks_at_end = 0;
static clock_t sys_clock_at_start = 0;
static clock_t sys_clock_at_end = 0;
static unsigned char timing_first_produced_recorded = 0;
static unsigned char timing_all_produced_recorded = 0;
static unsigned char timing_all_consumed_recorded = 0;
static unsigned char timing_validation_recorded = 0;
/* Initialize producer tracking arrays */
static void init_producer_tracking(void) {
    int i;
    for (i = 0; i < NUM_PRODUCERS; i++) {
        producer_pending_items[i] = -1;
        producer_started[i] = 0;
    }
}

/* Helper function: atomically get next item index for production */
static int get_next_test_item_index(void)
{
    unsigned int current;
    
    if (test_producer_index >= TEST_ITEM_COUNT) {
        return -1;  /* All items have been taken */
    }
    
    current = test_producer_index;
    test_producer_index++;
    
    /* Sanity check: ensure we never return invalid index */
    if (current >= TEST_ITEM_COUNT) {
        printf("[ERROR] get_next_test_item_index() returning invalid index %u!\n", current);
        return -1;
    }
    
    return (int)current;
}

/* Generic producer task that publishes to a topic based on producer_id */
static void test_producer_task(void *arg)
{
    int pool_exhausted_this_round;
    int has_pending;
    int publish_succeeded;
    int consumer_idx;
    PubSubMessage msg;
    int producer_id = (int)(unsigned long)arg;
    int producer_idx = producer_id - 1;  /* Array index (0-based) */
    char topic_name[32];
    int *pending_ptr = &producer_pending_items[producer_idx];
    
    if (!producer_started[producer_idx]) {
        printf("[TEST_PRODUCER_%d] Starting producer, will take items from shared pool\n", producer_id);
        producer_started[producer_idx] = 1;
    }
    
    /* Keep running until validation is complete */
    while (!test_validation_complete) {
        pool_exhausted_this_round = 0;
        has_pending = (*pending_ptr >= 0) ? 1 : 0;
        publish_succeeded = 0;
        
        /* If no pending item, try to get next one from pool */
        if (*pending_ptr < 0) {
            *pending_ptr = get_next_test_item_index();
            if (*pending_ptr < 0) {
                /* Pool exhausted this round */
                pool_exhausted_this_round = 1;
            }
        }
        
        /* Try to publish pending item (if we have one) */
        if (*pending_ptr >= 0 && *pending_ptr < TEST_ITEM_COUNT) {
            msg.key = (unsigned int)(*pending_ptr);
            
            /* Publish JSON data if available, otherwise publish numeric value */
            if (test_items[*pending_ptr].has_json) {
                msg.value = (void *)test_items[*pending_ptr].json_data;
            } else {
                msg.value = (void *)(unsigned long)test_items[*pending_ptr].numeric_value;
            }
            
            /* Distribute items round-robin to consumers by topic */
            consumer_idx = (*pending_ptr) % NUM_CONSUMERS;
            snprintf(topic_name, sizeof(topic_name), "test_items_consumer_%d", consumer_idx);
            
            if (pubsub_publish(&g_pubsub_mgr, topic_name, &msg)) {
                if (test_items[*pending_ptr].has_json) {
                    printf("[TEST_PRODUCER_%d] Published item %d JSON to consumer_%d: %s\n", 
                           producer_id, *pending_ptr, consumer_idx, test_items[*pending_ptr].json_data);
                } else {
                    printf("[TEST_PRODUCER_%d] Published item %d to consumer_%d: key=%u, value=%u\n", 
                           producer_id, *pending_ptr, consumer_idx, msg.key, test_items[*pending_ptr].numeric_value);
                }
                test_items_produced++;
                
                /* Record timing for first production */
                if (!timing_first_produced_recorded) {
                    time_first_produced = scheduler_get_ticks();
                    timing_first_produced_recorded = 1;
                    printf("[TIMING] First item produced at tick %u (elapsed: %u ms)\n", 
                           time_first_produced, time_first_produced - time_test_started);
                }
                
                /* Record timing for all produced */
                if (test_items_produced >= TEST_ITEM_COUNT && !timing_all_produced_recorded) {
                    time_all_produced = scheduler_get_ticks();
                    timing_all_produced_recorded = 1;
                    printf("[TIMING] All %u items produced at tick %u (elapsed: %u ms)\n", 
                           TEST_ITEM_COUNT, time_all_produced, time_all_produced - time_test_started);
                }
                
                *pending_ptr = -1;  /* Mark item as successfully sent */
                publish_succeeded = 1;
            } else {
                if (has_pending) {
                    printf("[TEST_PRODUCER_%d] Retrying pending item %d (queue full)\n", producer_id, *pending_ptr);
                }
            }
        }
        
        /* Yield to allow other tasks to run */
        if (pool_exhausted_this_round && !has_pending) {
            scheduler_sleep(100);  /* Long sleep when pool exhausted and nothing pending */
        } else if (!publish_succeeded && has_pending) {
            scheduler_sleep(50);   /* Medium sleep when retrying pending item */
        } else {
            scheduler_sleep(10);   /* Short sleep after successful publish or getting new item */
        }
    }
}

/* Consumer callback for test items */
static void test_item_consumer(const char *topic, const PubSubMessage *message, void *user_data)
{
    unsigned int key;
    
    /* Ensure test btree is initialized */
    if (g_test_btree == NULL) {
        g_test_btree = btree_create();
    }
    
    if (g_test_btree == NULL) {
        printf("[TEST_CONSUMER] FAILED to create test btree\n");
        return;
    }
    
    /* Use message key as the btree key */
    key = message->key;
    
    /* Check if this is a duplicate (already consumed) */
    if (btree_get(g_test_btree, key) != NULL) {
        printf("[TEST_CONSUMER] WARNING: Duplicate item received: key=%u\n", key);
    } else {
        /* Insert the message value into the test btree */
        btree_insert(g_test_btree, key, message->value);
        test_items_consumed++;
        
        /* Log JSON or numeric based on what it is */
        if (key < JSON_ITEM_COUNT && test_items[key].has_json) {
            printf("[TEST_CONSUMER] Consumed JSON item[%u]: %s\n", key, (const char *)message->value);
        } else {
            printf("[TEST_CONSUMER] Consumed numeric item[%u]: %u\n", key, (unsigned int)(unsigned long)message->value);
        }
        
        /* Record timing for all consumed */
        if (test_items_consumed >= TEST_ITEM_COUNT && !timing_all_consumed_recorded) {
            time_all_consumed = scheduler_get_ticks();
            timing_all_consumed_recorded = 1;
            printf("[TIMING] All %u items consumed at tick %u (elapsed: %u ms)\n", 
                   TEST_ITEM_COUNT, time_all_consumed, time_all_consumed - time_test_started);
        }
    }
    
    (void)topic;
    (void)user_data;
}

/* Validator task: checks all sent items are in the btree */
static void test_validator_task(void *arg)
{
    static unsigned int validation_index = 0;
    static unsigned int validation_passed = 0;
    static unsigned int validation_failed = 0;
    static unsigned int validator_phase = 0;  /* 0=waiting, 1=validating, 2=done */
    void *retrieved_value;
    unsigned long expected;
    
    (void)arg;
    
    if (validation_index == 0 && validator_phase == 0) {
        printf("[TEST_VALIDATOR] Starting validator task\n");
        printf("[TEST_VALIDATOR] Waiting for all %u items to be consumed...\n", TEST_ITEM_COUNT);
    }
    
    /* Infinite loop with phases */
    while (1) {
        /* Phase 0: Wait for all items to be consumed */
        if (validator_phase == 0) {
            printf("[TEST_VALIDATOR] Progress: %u/%u consumed\n", 
                   test_items_consumed, TEST_ITEM_COUNT);
            
            if (test_items_consumed >= TEST_ITEM_COUNT) {
                printf("[TEST_VALIDATOR] All items consumed, validating...\n");
                validator_phase = 1;
                validation_index = 0;
            } else {
                scheduler_sleep(200);
            }
        }
        /* Phase 1: Validate items one per invocation */
        else if (validator_phase == 1) {
            if (validation_index < TEST_ITEM_COUNT) {
                if (g_test_btree != NULL) {
                    retrieved_value = btree_get(g_test_btree, validation_index);
                    
                    /* Validate JSON items */
                    if (validation_index < JSON_ITEM_COUNT && test_items[validation_index].has_json) {
                        const char *expected_json = test_items[validation_index].json_data;
                        const char *retrieved_json = (const char *)retrieved_value;
                        
                        if (retrieved_value != NULL && strcmp(retrieved_json, expected_json) == 0) {
                            printf("[TEST_VALIDATOR] PASS: item[%u] JSON valid: %s\n", 
                                   validation_index, expected_json);
                            validation_passed++;
                        } else {
                            printf("[TEST_VALIDATOR] FAIL: item[%u] JSON mismatch. Expected: %s, Got: %s\n", 
                                   validation_index, expected_json, retrieved_json ? retrieved_json : "NULL");
                            validation_failed++;
                        }
                    } 
                    /* Validate numeric items */
                    else {
                        expected = (unsigned long)test_items[validation_index].numeric_value;
                        
                        if (retrieved_value == (void *)expected) {
                            printf("[TEST_VALIDATOR] PASS: item[%u] = %lu (numeric, found in btree)\n", 
                                   validation_index, expected);
                            validation_passed++;
                        } else {
                            printf("[TEST_VALIDATOR] FAIL: item[%u] = %lu (got %p from btree)\n", 
                                   validation_index, expected, retrieved_value);
                            validation_failed++;
                        }
                    }
                } else {
                    printf("[TEST_VALIDATOR] ERROR: test btree is NULL\n");
                    validator_phase = 2;
                }
                validation_index++;
                scheduler_sleep(50);
            } else {
                /* All items validated, move to phase 2 */
                validator_phase = 2;
            }
        }
        /* Phase 2: Print summary and complete */
        else if (validator_phase == 2) {
            unsigned int total_time;
            unsigned int production_time;
            unsigned int consumption_time;
            
            if (!timing_validation_recorded) {
                time_validation_complete = scheduler_get_ticks();
                cpu_ticks_at_end = scheduler_cpu_total_ticks();
                active_ticks_at_end = scheduler_cpu_active_ticks();
                sys_clock_at_end = clock();
                timing_validation_recorded = 1;
            }
            
            /* Total time is from system clock - it's the only reliable wall-clock source.
             * If it shows incorrectly, check that CLOCKS_PER_SEC is correct for your platform. */
            total_time = 0;
            if (sys_clock_at_end > sys_clock_at_start) {
                clock_t sys_elapsed = sys_clock_at_end - sys_clock_at_start;
                clock_t clocks_per_sec = CLOCKS_PER_SEC;
                total_time = (clocks_per_sec > 0) ? (unsigned int)((sys_elapsed * 1000) / clocks_per_sec) : 0;
            }
            
            production_time = time_all_produced - time_first_produced;
            consumption_time = time_all_consumed - time_all_produced;
            
            printf("[TEST_VALIDATOR] ========== VALIDATION SUMMARY ==========\n");
            printf("[TEST_VALIDATOR] Total items sent:     %u\n", TEST_ITEM_COUNT);
            printf("[TEST_VALIDATOR] Total items consumed: %u\n", test_items_consumed);
            printf("[TEST_VALIDATOR] Validation passed:    %u/%u\n", validation_passed, TEST_ITEM_COUNT);
            printf("[TEST_VALIDATOR] Validation failed:    %u/%u\n", validation_failed, TEST_ITEM_COUNT);
            
            printf("[TEST_VALIDATOR] ========== TIMING SUMMARY ==========\n");
            printf("[TEST_VALIDATOR] Producers:           %u\n", NUM_PRODUCERS);
            printf("[TEST_VALIDATOR] Consumers:           %u\n", NUM_CONSUMERS);
            printf("[TEST_VALIDATOR] Total test time:     %u.%03u seconds\n", total_time / 1000, total_time % 1000);
            if (timing_first_produced_recorded) {
                printf("[TEST_VALIDATOR] Time to first item:  %u scheduler ticks\n", time_first_produced - time_test_started);
            }
            if (timing_all_produced_recorded) {
                printf("[TEST_VALIDATOR] Time to produce all: %u scheduler ticks\n", time_all_produced - time_test_started);
                printf("[TEST_VALIDATOR] Production rate:    %u items/sec (estimated)\n", 
                       TEST_ITEM_COUNT * 1000 / (total_time ? total_time : 1));
            }
            if (timing_all_consumed_recorded) {
                printf("[TEST_VALIDATOR] Time to consume all: %u scheduler ticks\n", time_all_consumed - time_test_started);
                printf("[TEST_VALIDATOR] Consumption rate:   %u items/sec (estimated)\n", 
                       TEST_ITEM_COUNT * 1000 / (total_time ? total_time : 1));
            }
            if (timing_validation_recorded) {
                printf("[TEST_VALIDATOR] Time to validate all:%u scheduler ticks\n", time_validation_complete - time_all_consumed);
            }
            
            printf("[TEST_VALIDATOR] ========== SYSTEM CLOCK METRICS ==========\n");
            if (sys_clock_at_end > sys_clock_at_start) {
                clock_t sys_elapsed;
                clock_t clocks_per_sec;
                unsigned int ms_elapsed;
                unsigned int adjusted_ms;
                unsigned long scheduler_tick_count;
                
                sys_elapsed = sys_clock_at_end - sys_clock_at_start;
                clocks_per_sec = CLOCKS_PER_SEC;
                ms_elapsed = 0;
                adjusted_ms = 0;
                scheduler_tick_count = (unsigned long)(time_validation_complete - time_test_started);
                
                printf("[TEST_VALIDATOR] System clock start:   %lu\n", (unsigned long)sys_clock_at_start);
                printf("[TEST_VALIDATOR] System clock end:     %lu\n", (unsigned long)sys_clock_at_end);
                printf("[TEST_VALIDATOR] System clock elapsed:%lu (clock ticks)\n", (unsigned long)sys_elapsed);
                printf("[TEST_VALIDATOR] CLOCKS_PER_SEC (configured): %lu\n", (unsigned long)clocks_per_sec);
                
                if (clocks_per_sec > 0) {
                    ms_elapsed = (unsigned int)((sys_elapsed * 1000) / clocks_per_sec);
                    printf("[TEST_VALIDATOR] Calculated time (from clock()): %u.%03u seconds\n", 
                           ms_elapsed / 1000, ms_elapsed % 1000);
                    
                    /* Use scheduler ticks to detect and correct miscalibration */
                    /* Typical yield/context switch takes ~10-100us, so with ~27000 ticks in 108s,
                       each tick is ~4ms, giving ~250 ticks/second */
                    if (scheduler_tick_count > 1000) {
                        /* Calculate estimated elapsed time from scheduler ticks
                           Assumption: a well-tuned scheduler has 200-500 ticks/second */
                        unsigned long ticks_per_sec_min = 100;
                        unsigned long ticks_per_sec_max = 500;
                        unsigned long implied_ticks_per_sec = (scheduler_tick_count * 1000) / ms_elapsed;
                        
                        if (implied_ticks_per_sec > ticks_per_sec_max || 
                            implied_ticks_per_sec < ticks_per_sec_min) {
                            /* clock() likely miscalibrated - try to detect correction factor */
                            unsigned long expected_ticks_per_sec = 250;  /* typical value */
                            unsigned long corrected_ms = (scheduler_tick_count * 1000) / expected_ticks_per_sec;
                            unsigned long factor_num = (corrected_ms * 100) / ms_elapsed;  /* factor * 100 for integer math */
                            printf("[TEST_VALIDATOR] [AUTO-CALIBRATION] Detected clock() miscalibration\n");
                            printf("[TEST_VALIDATOR]   Implied ticks/sec from clock(): %lu\n", implied_ticks_per_sec);
                            printf("[TEST_VALIDATOR]   Expected ticks/sec: ~%lu\n", expected_ticks_per_sec);
                            printf("[TEST_VALIDATOR]   Correction factor: %lu.%02lu\n", 
                                   factor_num / 100, factor_num % 100);
                            printf("[TEST_VALIDATOR]   Corrected time: %lu.%03lu seconds\n",
                                   corrected_ms / 1000, corrected_ms % 1000);
                            total_time = (unsigned int)corrected_ms;
                        } else {
                            total_time = ms_elapsed;
                        }
                    } else {
                        total_time = ms_elapsed;
                    }
                }
            }
            
            /* Note: If auto-calibration adjusts the time significantly, it means 
             * clock() and CLOCKS_PER_SEC are not properly configured for your environment. */
            
            if (validation_failed == 0) {
                printf("[TEST_VALIDATOR] ========== ALL VALIDATIONS PASSED! ==========\n");
            } else {
                printf("[TEST_VALIDATOR] ========== VALIDATION ERRORS DETECTED ==========\n");
            }
            
            test_validation_complete = 1;
            validator_phase = 3;  /* Mark as fully done */
        }
        /* Phase 3: Keep running but do nothing */
        else {
            scheduler_sleep(100);
            break;  /* Exit the loop but not the function - will be called again */
        }
    }
}

/* Cleanup task: waits for validation to complete and halts the system */
static void test_cleanup_task(void *arg)
{
    (void)arg;
    
    printf("[CLEANUP] Waiting for validation to complete...\n");
    
    while (!test_validation_complete) {
        scheduler_sleep(100);
    }
    
    printf("[CLEANUP] Validation complete! All tests finished.\n");
    printf("[CLEANUP] Halting system...\n");
    scheduler_sleep(500);
    
    /* Halt the system by disabling interrupts and entering infinite loop */
#if defined(__CC65__)
    __asm__("sei");
#endif
    
    for (;;) { /* halt */ }
}
#endif

/* Bridge task that polls MQTT and publishes to pubsub */
static void mqtt_to_pubsub_bridge(void *arg)
{
    static unsigned int msg_len;
    static unsigned int payload_len;
    PubSubMessage pub_msg;
    
    (void)arg;
    
    printf("[BRIDGE] Starting MQTT to PubSub bridge task\n");
    
    for (;;) {
        /* Poll for MQTT messages */
        RIA.op = 0x35;  /* mq_poll */
        while (RIA.busy) { }
        
        msg_len = RIA.a | (RIA.x << 8);
        
        if (msg_len > 0) {
            printf("[BRIDGE] MQTT message received (%u bytes)\n", msg_len);
            
            /* Read the actual message to consume it from the MQTT queue */
            /* Set up buffer at 0x0600 for message payload */
            RIA.xstack = 0x0600 >> 8;    /* payload addr high */
            RIA.xstack = 0x0600 & 0xFF;  /* payload addr low */
            RIA.xstack = 255 >> 8;       /* buffer size high */
            RIA.xstack = 255 & 0xFF;     /* buffer size low */
            
            RIA.op = 0x36;  /* mq_read_message - THIS CONSUMES THE MESSAGE */
            while (RIA.busy) { }
            
            payload_len = RIA.a | (RIA.x << 8);
            printf("[BRIDGE] Read message: %u bytes from MQTT\n", payload_len);
            
            /* Create key/value message: key=0, value=payload length cast to pointer */
            pub_msg.key = 0;
            pub_msg.value = (void *)(unsigned long)payload_len;
            
            /* Publish to pubsub system */
            if (pubsub_publish(&g_pubsub_mgr, "rp6502_sub", &pub_msg)) {
                printf("[BRIDGE] Published key=0, value=%u to 'rp6502_sub' topic\n", payload_len);
            } else {
                printf("[BRIDGE] FAILED to publish to pubsub (queue full?)\n");
            }
            
            /* Keep polling to see if there are more messages queued */
        } else {
            /* No message, sleep to avoid busy-polling */
            scheduler_sleep(100);
        }
    }
}

static void pubsub_monitor(void *arg)
{
    static unsigned int empty_count = 0;
    unsigned int queue_size;
    unsigned int i;
    
    (void)arg;
    
    printf("[MONITOR] Starting pubsub monitor task\n");
    
    for (;;) {
        /* Process queued messages FIRST to ensure minimal latency */
        pubsub_process_all(&g_pubsub_mgr);
        
        /* Check if validation is complete */
        if (test_validation_complete) {
            printf("[MONITOR] Validation complete, exiting monitor task\n");
            break;
        }
        
        printf("[MONITOR] Queue sizes:");

        if (g_pubsub_mgr.topic_count == 0) {
            printf(" none\n");
        } else {
            for (i = 0; i < g_pubsub_mgr.topic_count; i++) {
                queue_size = pubsub_queue_size(&g_pubsub_mgr, g_pubsub_mgr.topics[i].name);
                printf(" %s=%u", g_pubsub_mgr.topics[i].name, queue_size);
            }
            printf("\n");
        }

        /* Reduced sleep to ensure frequent message processing */
        scheduler_sleep(50);
    }
}

static void pubsub_mqtt_monitor(void *arg)
{
    static unsigned int empty_count = 0;
    static const unsigned int EMPTY_THRESHOLD = 10;
    unsigned int queue_size;
    
    (void)arg;
    
    printf("[MONITOR] Starting pubsub monitor task\n");
    
    for (;;) {
        pubsub_process_all(&g_pubsub_mgr);
        
        queue_size = pubsub_queue_size(&g_pubsub_mgr, "rp6502_sub");
        printf("[MONITOR] Queue sizes: rp6502_sub=%u\n", queue_size);
        
        /* Track empty cycles */
        if (queue_size == 0) {
            empty_count++;
            
            if (empty_count >= EMPTY_THRESHOLD) {
                printf("[MONITOR] Queue empty for %u cycles. Exiting monitor task.\n", empty_count);
                break;
            }
        } else {
            empty_count = 0;  /* Reset counter if queue has messages */
        }
        
        scheduler_sleep(300);
    }
    
    printf("[MONITOR] Monitor task completed\n");
}

static void pubsub_publish_task(void *arg)
{
    static char json_buffer[128];
    PubSubMessage msg;
    
    (void)arg;
    
    printf("[MONITOR] Starting pubsub publish task\n");
    
    for (;;) {
        /* Check if validation is complete */
        if (test_validation_complete) {
            printf("[PUBLISH_TASK] Validation complete, exiting publish task\n");
            break;
        }
        
        /* Publish to rp6502_pub_1 */
        msg = pubsub_make_message(1, (void *)(unsigned long)_count1);
        pubsub_publish(&g_pubsub_mgr, "rp6502_pub_1", &msg);
        scheduler_sleep(100);
        
        /* Publish to rp6502_pub_2 */
        msg = pubsub_make_message(2, (void *)(unsigned long)_count2);
        pubsub_publish(&g_pubsub_mgr, "rp6502_pub_2", &msg);
        scheduler_sleep(200);
        
        /* Publish JSON string to rp6502_pub_3 */
        snprintf(json_buffer, sizeof(json_buffer), 
                 "{\"count\":%u,\"status\":\"active\"}", _count3);
        msg = pubsub_make_message(3, json_buffer);
        pubsub_publish(&g_pubsub_mgr, "rp6502_pub_3", &msg);

        _count1++;
        _count2++;
        _count3++;
        scheduler_sleep(300);
    }
}

void main()
{
    unsigned int i;
    unsigned int producer_id;
    char topic_name[32];
    
    scheduler_init();

    /* Seed RNG with tick count so different runs have different sequences */
    seed_random(scheduler_get_ticks());
    
    /* Warm up the RNG with a few iterations to diverge from fixed seed */
    for (i = 0; i < 10u; ++i) {
        pseudo_random(0u, 1u);
    }

    #if USE_PUBSUB_BTREE_ONLY == 1
        /* Record test start time and CPU metrics */
        time_test_started = scheduler_get_ticks();
        cpu_ticks_at_start = scheduler_cpu_total_ticks();
        active_ticks_at_start = scheduler_cpu_active_ticks();
        sys_clock_at_start = clock();
        printf("[TIMING] Test started at scheduler tick %u, system clock %lu\n", 
               time_test_started, (unsigned long)sys_clock_at_start);
        
        /* Generate test data (numeric and JSON) */
        printf("\n[MAIN] Generating %u test items (%u with JSON data)...\n", TEST_ITEM_COUNT, JSON_ITEM_COUNT);
        for (i = 0; i < TEST_ITEM_COUNT; i++) {
            test_items[i].numeric_value = pseudo_random(100, 999);
            test_items[i].has_json = 0;
            test_items[i].json_data[0] = '\0';
            
            /* Add JSON data to first JSON_ITEM_COUNT items */
            if (i < JSON_ITEM_COUNT) {
                unsigned int id = i + 1000;
                unsigned int value = pseudo_random(10, 100);
                unsigned int timestamp = i * 10;
                snprintf(test_items[i].json_data, MAX_JSON_SIZE,
                        "{\"id\":%u,\"val\":%u,\"ts\":%u}",
                        id, value, timestamp);
                test_items[i].has_json = 1;
                printf("[MAIN] test_items[%u] JSON: %s\n", i, test_items[i].json_data);
            } else if (i % 50 == 0) {
                printf("[MAIN] test_items[%u] = %u (numeric only)\n", i, test_items[i].numeric_value);
            }
        }
        
        printf("\n[MAIN] Initializing pub/sub system with message storage...\n");
        pubsub_init(&g_pubsub_mgr);
        
        /* Initialize producer tracking */
        init_producer_tracking();
        printf("[MAIN] Creating %u consumer topics for work-queue distribution...\n", 
               NUM_CONSUMERS);

        /* Create one topic per consumer for work-queue distribution */
        for (i = 0; i < NUM_CONSUMERS; i++) {
            snprintf(topic_name, sizeof(topic_name), "test_items_consumer_%d", i);
            pubsub_create_topic(&g_pubsub_mgr, topic_name);
            pubsub_subscribe(&g_pubsub_mgr, topic_name, 
                            test_item_consumer, NULL);
        }

        scheduler_add(pubsub_monitor, NULL);
        
        /* Dynamically add producer tasks */
        for (producer_id = 1; producer_id <= NUM_PRODUCERS; producer_id++) {
            scheduler_add(test_producer_task, (void *)(unsigned long)producer_id);
        }
        
        scheduler_add(test_validator_task, NULL);
        scheduler_add(test_cleanup_task, NULL);
        scheduler_add(idle_task, NULL);
    #endif

    scheduler_run();
}
