/* ROM-free schedule regression for the controller-poll handshake.
 *
 * The Python runner extracts the production joy.c functions and the port's
 * production message-queue functions into joy_poll_production.inc and
 * port_queue_production.inc. This harness supplies only synthetic queue state
 * and inert controller/save adapters; it contains no game data.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef signed char s8;
typedef int s32;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef void *OSMesg;

typedef struct OSMesgQueue_s {
    void *mtqueue;
    void *fullqueue;
    s32 validCount;
    s32 first;
    s32 msgCount;
    OSMesg *msg;
} OSMesgQueue;

typedef struct {
    u16 button;
    s8 stick_x;
    s8 stick_y;
    u8 errno;
} OSContPad;

#define MAXCONTROLLERS 4
#define CONTSAMPLE_LEN 20
#define OS_MESG_NOBLOCK 0
#define OS_MESG_BLOCK 1

struct contsample {
    OSContPad pads[MAXCONTROLLERS];
};

struct contdata {
    struct contsample samples[CONTSAMPLE_LEN];
    s32 curlast;
    s32 curstart;
    s32 nextlast;
    s32 nextsecondlast;
    u16 buttonspressed[MAXCONTROLLERS];
    s32 playbackcontcount;
};

#define CONT_INPUT_BUFFER_LEN 10
#define CONT_DISABLE_POLL_SEND_BUFFER_LEN 1
#define CONT_DISABLE_POLL_RECEIVE_BUFFER_LEN 1
#define CONT_ENABLE_POLL_SEND_BUFFER_LEN 1
#define CONT_ENABLE_POLL_RECEIVE_BUFFER_LEN 1

OSMesg g_ContInputMessageBuffer[CONT_INPUT_BUFFER_LEN];
OSMesgQueue g_ContInputMessageQueue;
OSMesg g_ContDisablePollSendMessageBuffer[CONT_DISABLE_POLL_SEND_BUFFER_LEN];
OSMesgQueue g_ContDisablePollSendMessageQueue;
OSMesg g_ContDisablePollReceiveMessageBuffer[CONT_DISABLE_POLL_RECEIVE_BUFFER_LEN];
OSMesgQueue g_ContDisablePollReceiveMessageQueue;
OSMesg g_ContEnablePollSendMessageBuffer[CONT_ENABLE_POLL_SEND_BUFFER_LEN];
OSMesgQueue g_ContEnablePollSendMessageQueue;
OSMesg g_ContEnablePollReceiveMessageBuffer[CONT_ENABLE_POLL_RECEIVE_BUFFER_LEN];
OSMesgQueue g_ContEnablePollReceiveMessageQueue;

struct contdata g_ContData[2];
s32 g_ContBusy;
s32 g_ContPollDisableCount;
s32 g_ContQueuesCreated;
s32 g_ContInitDone;
s32 g_ContCheckStatusTimer60;
u32 g_ContBadReadsStickX[MAXCONTROLLERS];
u32 g_ContBadReadsStickY[MAXCONTROLLERS];
u32 g_ContBadReadsButtons[MAXCONTROLLERS];
u32 g_ContBadReadsButtonsPressed[MAXCONTROLLERS];

static OSMesgQueue *ge_retrace_q;
static struct {
    short type;
    char misc[30];
} ge_retrace_msg = {1, {0}};

#include "port_queue_production.inc"

static int disable_send_attempts;
static int disable_send_failures;
static int enable_send_attempts;
static int enable_send_failures;
static int controller_reads;
static int status_checks;
static int save_writes;

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)
{
    s32 result = productionOsSendMesg(mq, msg, flag);

    if (mq == &g_ContDisablePollSendMessageQueue) {
        disable_send_attempts++;
        disable_send_failures += result != 0;
    } else if (mq == &g_ContEnablePollSendMessageQueue) {
        enable_send_attempts++;
        enable_send_failures += result != 0;
    }

    return result;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    return productionOsRecvMesg(mq, msg, flag);
}

s32 osContStartReadData(OSMesgQueue *mq)
{
    return osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
}

void osContGetReadData(OSContPad *pads)
{
    controller_reads++;
    memset(pads, 0, sizeof(*pads) * MAXCONTROLLERS);
}

void joyCheckStatus(void)
{
    status_checks++;
}

void joyRumblePakTick(void)
{
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes)
{
    (void)mq;
    (void)address;
    (void)buffer;
    (void)nbytes;
    save_writes++;
    return 0;
}

#include "joy_poll_production.inc"

static int checks;
static int failures;

static void check(int condition, const char *description)
{
    checks++;
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", description);
    failures += !condition;
}

static void reset_state(void)
{
    memset(g_ContInputMessageBuffer, 0, sizeof(g_ContInputMessageBuffer));
    memset(g_ContDisablePollSendMessageBuffer, 0, sizeof(g_ContDisablePollSendMessageBuffer));
    memset(g_ContDisablePollReceiveMessageBuffer, 0, sizeof(g_ContDisablePollReceiveMessageBuffer));
    memset(g_ContEnablePollSendMessageBuffer, 0, sizeof(g_ContEnablePollSendMessageBuffer));
    memset(g_ContEnablePollReceiveMessageBuffer, 0, sizeof(g_ContEnablePollReceiveMessageBuffer));
    memset(g_ContData, 0, sizeof(g_ContData));
    memset(g_ContBadReadsStickX, 0, sizeof(g_ContBadReadsStickX));
    memset(g_ContBadReadsStickY, 0, sizeof(g_ContBadReadsStickY));
    memset(g_ContBadReadsButtons, 0, sizeof(g_ContBadReadsButtons));
    memset(g_ContBadReadsButtonsPressed, 0, sizeof(g_ContBadReadsButtonsPressed));

    osCreateMesgQueue(&g_ContInputMessageQueue, g_ContInputMessageBuffer, CONT_INPUT_BUFFER_LEN);
    osCreateMesgQueue(&g_ContDisablePollSendMessageQueue, g_ContDisablePollSendMessageBuffer, CONT_DISABLE_POLL_SEND_BUFFER_LEN);
    osCreateMesgQueue(&g_ContDisablePollReceiveMessageQueue, g_ContDisablePollReceiveMessageBuffer, CONT_DISABLE_POLL_RECEIVE_BUFFER_LEN);
    osCreateMesgQueue(&g_ContEnablePollSendMessageQueue, g_ContEnablePollSendMessageBuffer, CONT_ENABLE_POLL_SEND_BUFFER_LEN);
    osCreateMesgQueue(&g_ContEnablePollReceiveMessageQueue, g_ContEnablePollReceiveMessageBuffer, CONT_ENABLE_POLL_RECEIVE_BUFFER_LEN);

    g_ContBusy = 0;
    g_ContPollDisableCount = 0;
    g_ContQueuesCreated = 1;
    g_ContInitDone = 1;
    g_ContCheckStatusTimer60 = 0;
    disable_send_attempts = 0;
    disable_send_failures = 0;
    enable_send_attempts = 0;
    enable_send_failures = 0;
    controller_reads = 0;
    status_checks = 0;
    save_writes = 0;

    productionOsSendMesg(&g_ContInputMessageQueue, NULL, OS_MESG_NOBLOCK);
}

static void submit_mission_save(void)
{
    u8 synthetic_save_byte = 0;
    check(joyGamePakLongWrite(0, &synthetic_save_byte, 1) == 0,
          "synthetic mission save reaches the production wrapper");
    check(save_writes == 1, "synthetic EEPROM adapter records one write");
}

static void run_legacy_zero_gap(void)
{
    printf("\nlegacy balanced zero-gap schedule\n");
    reset_state();
    submit_mission_save();
    joyCheckStatusThreadSafe();

    check(disable_send_attempts == 2 && disable_send_failures == 1,
          "status disable collides with the queued save disable");
    check(enable_send_attempts == 2 && enable_send_failures == 1,
          "status enable collides symmetrically with the queued save enable");

    joyPoll(); joyPoll(); joyPoll();

    check(g_ContPollDisableCount == 0, "zero-gap disable and enable requests return depth to zero");
    check(g_ContDisablePollSendMessageQueue.validCount == 0 && g_ContEnablePollSendMessageQueue.validCount == 0,
          "zero-gap request queues drain");
    check(controller_reads == 1, "zero-gap schedule permits a fresh controller read");
    check(status_checks == 1, "status check executes once");
}

static void run_legacy_one_poll_gap(void)
{
    int reads_before_disabled_polls;
    int i;

    printf("\nlegacy dangerous one-poll-gap schedule\n");
    reset_state();
    submit_mission_save();
    joyPoll();
    check(g_ContPollDisableCount == 1, "single eligible poll consumes save disable first");
    check(g_ContDisablePollSendMessageQueue.validCount == 0 && g_ContEnablePollSendMessageQueue.validCount == 1,
          "single poll leaves only the save enable queued");

    joyCheckStatusThreadSafe();
    check(disable_send_attempts == 2 && disable_send_failures == 0,
          "status disable now fits in the emptied one-slot queue");
    check(enable_send_attempts == 2 && enable_send_failures == 1,
          "status enable is dropped against the pending save enable");

    joyPoll(); joyPoll();
    check(g_ContPollDisableCount == 1, "asymmetric drop leaves residual disable depth one");
    check(g_ContDisablePollSendMessageQueue.validCount == 0 && g_ContEnablePollSendMessageQueue.validCount == 0,
          "dangerous schedule leaves both request queues empty");

    reads_before_disabled_polls = controller_reads;
    for (i = 0; i < 8; i++) joyPoll();
    check(controller_reads == reads_before_disabled_polls && controller_reads == 0,
          "subsequent eligible polls suppress every fresh controller read");
    check(status_checks == 1, "status check still executes despite the dropped enable");
}

static void run_native_safe_schedule(int gap_polls)
{
    int reads_at_status;
    int i;

    printf("\nnative repaired %s schedule\n", gap_polls ? "one-poll-gap" : "zero-gap");
    reset_state();
    submit_mission_save();
    for (i = 0; i < gap_polls; i++) joyPoll();
    reads_at_status = controller_reads;
    joyCheckStatusThreadSafe();

    check(disable_send_attempts == 0 && enable_send_attempts == 0,
          "native save/status path submits no poll-control requests");
    check(g_ContPollDisableCount == 0, "native polling depth remains zero");
    check(g_ContDisablePollSendMessageQueue.validCount == 0 && g_ContEnablePollSendMessageQueue.validCount == 0,
          "native request queues remain empty");
    check(status_checks == 1, "native status check executes directly");

    for (i = 0; i < 4; i++) joyPoll();
    check(controller_reads == reads_at_status + 4, "fresh controller reads continue after status");
}

int main(void)
{
#if EXPECT_NATIVE_SAFE
    run_native_safe_schedule(0);
    run_native_safe_schedule(1);
#else
    run_legacy_zero_gap();
    run_legacy_one_poll_gap();
#endif

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
