#include "scheduler.h"
#include "pubsub.h"
#include <stddef.h> /* For NULL */
#include <stdlib.h> /* For malloc and free */
#include <ctype.h>  /* For isprint */
#include <rp6502.h>
#include <stdbool.h>
#include <fcntl.h>

/* Configuration flags - must come before conditional includes */
#define USE_PUBSUB_BTREE_ONLY 1
#define TEST_MSG_LENGTH 1
#define TCP_TEST_MSG_COUNT 1
#define TCP_TEST_MSG_LENGTH 2
#define TCP_BATCH_SIZE 10
#define RESPONSE_BUFFER_SIZE 256
#define COMMAND_TIMEOUT 10000

const unsigned int BATCH_SIZE = 500;    
const unsigned int MAX_ITEM_COUNT = 2000;    
volatile unsigned char mq_lock = 0;

/* Move declarations to the top of the file */
static unsigned char mem_fluctuate_active = 0;
static unsigned char mem_fluctuate_buffer[64];
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
#include "ringq.h"

volatile unsigned int sleep_ms = 250;
volatile unsigned int sleep_ms_short = 10;
volatile unsigned int sleep_ms_producer = 50;
volatile unsigned int sleep_ms_consumer = 200;
volatile unsigned int consumed_item_1 = 0;
volatile unsigned int consumed_item_2 = 0;
volatile unsigned int produced_item = 0;
volatile uint16_t task_a_counter = 1;
volatile uint16_t task_b_counter = 2;

/* Pub/Sub Manager for multi-topic messaging */
static PubSubManager g_pubsub_mgr;

#if USE_PUBSUB_BTREE_ONLY == 1
/* Simple message storage for USE_PUBSUB_BTREE_ONLY - stores message values */
#define MESSAGE_STORAGE_SIZE 32
static void *message_storage[MESSAGE_STORAGE_SIZE];
static unsigned int message_storage_count = 0;
#endif

/* Approximate total RAM available for the OS (matches rp6502.cfg:
    RAM start = $0200, size = $FD00 - __STACKSIZE__ where __STACKSIZE__ is $0800
    So total bytes = 0xFD00 - 0x0800 = 62464
*/
static const unsigned int RAM_TOTAL_BYTES = 62464u;

/* forward from scheduler.c */
unsigned int scheduler_memory_usage(void);

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

void xram_strcpy(unsigned int addr, const char* str) {
    int i;
    RIA.step0 = 1;  // Enable auto-increment
    RIA.addr0 = addr;
    for (i = 0; str[i]; i++) {
        RIA.rw0 = str[i];
    }
    RIA.rw0 = 0;
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

/* Simple print function */
static void tcp_print(char *s)
{
    while (*s)
        if (RIA.ready & RIA_READY_TX_BIT)
            RIA.tx = *s++;
}

/* Simple delay function */
static void delay_ms(int ms)
{
    int i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 100; j++)
            ;
}

/* Simple number parser */
static int parse_number(char *str)
{
    int result = 0;
    while (*str >= '0' && *str <= '9')
    {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
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
static void on_rp6502_btree(const char *topic, const PubSubMessage *message, void *user_data)
{
    unsigned int storage_index;
    
    if (message_storage_count >= MESSAGE_STORAGE_SIZE)
        return;  /* Storage full */
    
    /* Store the message value in the simple storage array */
    storage_index = message_storage_count++;
    message_storage[storage_index] = message->value;
    
    printf("[BTREE_SUBSCRIBER] Received on topic '%s': key=%d, stored_index=%u, value=%p\n",
           topic, message->key, storage_index, message->value);
    
    (void)user_data;
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

/* Queue test: producer/consumer pair that use their own RingQ instance to
   send a random number of items (1000-10000 per run) and verify that all
   items were received. */
static RingQ test_q_2;
volatile unsigned int test_sent_count = 0;
volatile unsigned int test_recv_count = 0;
volatile unsigned int test_producer_done = 0;
volatile unsigned int test_total_item_count = 0;
volatile unsigned int test_start_ticks = 0;
volatile unsigned int test_consumer_ready = 0;
volatile unsigned int test_end_ticks = 0;
static unsigned int test_time_elapsed_ticks = 0;
volatile unsigned long test_sent_sum = 0UL;
volatile unsigned long test_recv_sum = 0UL;
volatile unsigned int test_run_count = 0;

/* Instrumentation totals for ringq operations (incremented in ringq.c). */
volatile unsigned long ringq_total_pushed = 0UL;
volatile unsigned long ringq_total_popped = 0UL;

/* Debug hook invoked from ringq.c on invariant failures. */
void ringq_debug_fail(const char *msg, unsigned int a, unsigned int b)
{
    /* Reuse fail_halt to print extended state and halt */
    fail_halt(msg, a, b);
}

/* Sequence instrumentation: monotonic sequence written by producer and
   recorded by consumer to help detect extra/duplicate pops. */
volatile unsigned long test_seq = 0UL;
volatile unsigned int recv_log[64];
volatile unsigned int recv_log_pos = 0u;

/* Full fail_halt implementation prints extended runtime state then halts. */
static void fail_halt(const char *msg, unsigned int a, unsigned int b)
{
    char numbuf[32];
    unsigned int tail_idx;
    unsigned int val_tail;
    unsigned int prev_idx;

    puts("*** FATAL: "); puts(msg); puts("\r\n");

    puts("args: ");
    itoa_new(a, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    itoa_new(b, numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    puts("run:"); itoa_new(test_run_count, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("ticks:"); itoa_new(scheduler_get_ticks(), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    puts("total_items:"); itoa_new(test_total_item_count, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("sent_count:"); itoa_new(test_sent_count, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("recv_count:"); itoa_new(test_recv_count, numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    puts("sent_sum_lo:"); itoa_new((unsigned int)(test_sent_sum & 0xFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("recv_sum_lo:"); itoa_new((unsigned int)(test_recv_sum & 0xFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    /* Test queue state */
    puts("test_q_2 head:"); itoa_new(test_q_2.head, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("tail:"); itoa_new(test_q_2.tail, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("count:"); itoa_new(q_count(&test_q_2), numbuf, sizeof(numbuf)); puts(numbuf); puts(" free:"); itoa_new(q_space_free(&test_q_2), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

    if (!q_is_empty(&test_q_2)) {
        tail_idx = test_q_2.tail;
        val_tail = test_q_2.buf[tail_idx];
        itoa_new(val_tail, numbuf, sizeof(numbuf)); puts("q2.buf[tail]:"); puts(numbuf); puts(" ");
        prev_idx = (test_q_2.head - 1) & (Q_CAP - 1);
        itoa_new(test_q_2.buf[prev_idx], numbuf, sizeof(numbuf)); puts("q2.buf[head-1]:"); puts(numbuf); puts("\r\n");
    }

    /* Print a small window of consecutive queue entries starting at tail. */
    {
        unsigned int idx = test_q_2.tail;
        unsigned int i;
        puts("q2.buf[window 8]:");
        for (i = 0; i < 8u; ++i) {
            unsigned int v = test_q_2.buf[idx];
            itoa_new(v, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
            idx = (idx + 1) & (Q_CAP - 1);
        }
        puts("\r\n");
    }

    /* Print recent received values from consumer log */
    {
        unsigned int rp = recv_log_pos;
        unsigned int i;
        puts("recent_recv[8]:");
        for (i = 0; i < 8u; ++i) {
            unsigned int idx = (unsigned int)((rp - 8u + i) & 63u);
            unsigned int v = recv_log[idx];
            itoa_new(v, numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
        }
        puts("\r\n");
    }

    /* Print global instrumentation totals */
    puts("ringq_total_pushed:"); itoa_new((unsigned int)(ringq_total_pushed & 0xFFFFFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts(" ");
    puts("ringq_total_popped:"); itoa_new((unsigned int)(ringq_total_popped & 0xFFFFFFFFu), numbuf, sizeof(numbuf)); puts(numbuf); puts("\r\n");

#if defined(__CC65__)
    /* disable interrupts to keep system halted */
    __asm__("sei");
#endif

    for (;;) { /* halt */ }
}




/* ========== End TCP UART Tasks ========== */

void main()
{
    unsigned int i;
    test_run_count = 0;
    scheduler_init();

    /* Seed RNG with tick count so different runs have different sequences */
    seed_random(scheduler_get_ticks());
    
    /* Warm up the RNG with a few iterations to diverge from fixed seed */
    for (i = 0; i < 10u; ++i) {
        pseudo_random(0u, 1u);
    }

    #if USE_PUBSUB_BTREE_ONLY == 1
        printf("\n[MAIN] Initializing pub/sub system with message storage...\n");
        pubsub_init(&g_pubsub_mgr);
        
        pubsub_create_topic(&g_pubsub_mgr, "rp6502_pub_1");
        pubsub_create_topic(&g_pubsub_mgr, "rp6502_pub_2");        
        pubsub_create_topic(&g_pubsub_mgr, "rp6502_pub_3");                
        
        printf("[MAIN] Subscribing to topics with message storage...\n");

        pubsub_subscribe(&g_pubsub_mgr, "rp6502_pub_1", 
                        on_rp6502_btree, NULL);
        pubsub_subscribe(&g_pubsub_mgr, "rp6502_pub_2", 
                        on_rp6502_btree, NULL);
        pubsub_subscribe(&g_pubsub_mgr, "rp6502_pub_3", 
                        on_rp6502_btree, NULL);            

        scheduler_add(pubsub_publish_task, NULL);
        scheduler_add(pubsub_monitor, NULL);
        scheduler_add(idle_task, NULL);
    #endif

    scheduler_run();
}
