#include "scheduler.h"
#include "pubsub.h"
#include "btree.h"
#include <stddef.h> /* For NULL */
#include <stdlib.h> /* For malloc and free */
#include <ctype.h>  /* For isprint */
#include <rp6502.h>
#include <stdbool.h>
#include <fcntl.h>

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
#define TEST_ITEM_COUNT 100
#define NUM_PRODUCERS 2
#define NUM_CONSUMERS 4
static unsigned int test_items[TEST_ITEM_COUNT];
static unsigned int test_items_produced = 0;
static unsigned int test_items_consumed = 0;
static unsigned char test_validation_complete = 0;
/* Shared producer index - only one producer takes an item at a time */
static unsigned int test_producer_index = 0;
/* Per-producer pending items (indexed by producer_id - 1) */
static int producer_pending_items[NUM_PRODUCERS];
static unsigned char producer_started[NUM_PRODUCERS];
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
            msg.value = (void *)(unsigned long)test_items[*pending_ptr];
            
            /* Create topic name based on producer_id */
            snprintf(topic_name, sizeof(topic_name), "test_items_%d", producer_id);
            
            if (pubsub_publish(&g_pubsub_mgr, topic_name, &msg)) {
                printf("[TEST_PRODUCER_%d] Published item %d: key=%u, value=%u\n", 
                       producer_id, *pending_ptr, msg.key, test_items[*pending_ptr]);
                test_items_produced++;
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
        printf("[TEST_CONSUMER] WARNING: Duplicate item received: key=%u, value=%lu\n",
               key, (unsigned long)message->value);
    } else {
        /* Insert the message value into the test btree */
        btree_insert(g_test_btree, key, message->value);
        test_items_consumed++;
        
        printf("[TEST_CONSUMER] Consumed item: key=%u, value=%lu\n",
               key, (unsigned long)message->value);
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
                    expected = (unsigned long)test_items[validation_index];
                    retrieved_value = btree_get(g_test_btree, validation_index);
                    
                    if (retrieved_value == (void *)expected) {
                        printf("[TEST_VALIDATOR] PASS: item[%u] = %lu (found in btree)\n", 
                               validation_index, expected);
                        validation_passed++;
                    } else {
                        printf("[TEST_VALIDATOR] FAIL: item[%u] = %lu (got %p from btree)\n", 
                               validation_index, expected, retrieved_value);
                        validation_failed++;
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
            printf("[TEST_VALIDATOR] ========== VALIDATION SUMMARY ==========\n");
            printf("[TEST_VALIDATOR] Total items sent:     %u\n", TEST_ITEM_COUNT);
            printf("[TEST_VALIDATOR] Total items consumed: %u\n", test_items_consumed);
            printf("[TEST_VALIDATOR] Validation passed:    %u/%u\n", validation_passed, TEST_ITEM_COUNT);
            printf("[TEST_VALIDATOR] Validation failed:    %u/%u\n", validation_failed, TEST_ITEM_COUNT);
            
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

        pubsub_process_all(&g_pubsub_mgr);        
        scheduler_sleep(300);
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
        /* Fill test_items with random values */
        printf("\n[MAIN] Generating %u random test items...\n", TEST_ITEM_COUNT);
        for (i = 0; i < TEST_ITEM_COUNT; i++) {
            test_items[i] = pseudo_random(100, 999);
            printf("[MAIN] test_items[%u] = %u\n", i, test_items[i]);
        }
        
        printf("\n[MAIN] Initializing pub/sub system with message storage...\n");
        pubsub_init(&g_pubsub_mgr);
        
        /* Initialize producer tracking */
        init_producer_tracking();
        printf("[MAIN] Creating %u topics and subscribing with %u consumer(s) each...\n", 
               NUM_PRODUCERS, NUM_CONSUMERS);

        /* Dynamically create topics and subscribe consumers */
        for (producer_id = 1; producer_id <= NUM_PRODUCERS; producer_id++) {
            snprintf(topic_name, sizeof(topic_name), "test_items_%d", producer_id);
            pubsub_create_topic(&g_pubsub_mgr, topic_name);
            
            /* Create NUM_CONSUMERS subscriptions for this topic */
            for (i = 0; i < NUM_CONSUMERS; i++) {
                pubsub_subscribe(&g_pubsub_mgr, topic_name, 
                                test_item_consumer, NULL);
            }
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
