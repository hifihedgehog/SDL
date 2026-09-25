/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Shared harness for the serial joystick replay tests of hifihedgehog/SDL#33
   Part 5. It runs a module inside the real engine behind a scripted fake
   port: opens can fail, writes can stay pending, and every port call is
   recorded with the clock value at which it happened. Presence changes and
   published snapshots are recorded in order. Time is in milliseconds.

   Battery A, on one packet vector at a time, starting from a present device:
   (A1) each strict prefix fed alone changes nothing, (A2) the packet split at
   every boundary, and fed one byte at a time, decodes once, identical to one
   feed, (A3) bytes of 00 and of FF past the count change nothing, (A4) a
   reset after a partial packet discards it, (A5) the same packet twice
   produces one change, (A6) a malformed packet changes nothing and the next
   valid packet decodes. */

#ifndef testserialharness_h_
#define testserialharness_h_

#include "../src/joystick/serial/SDL_serial_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(condition)                                                      \
    do {                                                                      \
        ++checks;                                                             \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                     \
    } while (0)

static void H_Fail(const char *what)
{
    ++checks;
    ++failures;
    printf("FAILED: %s\n", what);
}

#define H_MAX_CALLS     2048
#define H_MAX_PUBLISHED 2048
#define H_MAX_PRESENCE  64

typedef struct H_Call
{
    char kind; /* O open, L line, T timeouts, P purge, E escape, W write, D drain, C close */
    uint64_t time;
    SDL_SerialLineConfig config;
    SDL_SerialTimeouts timeouts;
    uint32_t value;
    size_t length;
    uint8_t data[SDL_SERIAL_MAX_WRITE];
} H_Call;

typedef struct H_Presence
{
    int sub;
    uint32_t generation;
    uint64_t time;
    SDL_SerialIdentity identity;
} H_Presence;

typedef struct Harness
{
    SDL_SerialEngine engine;
    const SDL_SerialModule *module;
    void *state;
    uint64_t now;

    /* Fake port behavior */
    int open_failures;   /* Opens that fail before one succeeds */
    bool reject_toggle;  /* SetLine fails for RTS toggle mode */
    bool pend_writes;    /* Writes stay pending until H_CompleteWrite */
    bool fail_writes;
    uint32_t fail_rate;  /* SetLine fails for this rate */

    H_Call calls[H_MAX_CALLS];
    int ncalls;
    int cursor; /* Next call H_Expect checks */

    uint32_t presence[SDL_SERIAL_MAX_SUBDEVICES];
    SDL_SerialIdentity identity[SDL_SERIAL_MAX_SUBDEVICES];
    H_Presence presence_log[H_MAX_PRESENCE];
    int npresence;

    SDL_SerialQueueEntry published[H_MAX_PUBLISHED];
    int npublished;

    int nlogs;
    char last_log[128];
} Harness;

static H_Call *H_Record(Harness *h, char kind)
{
    static H_Call overflow;
    H_Call *call;

    if (h->ncalls >= H_MAX_CALLS) {
        H_Fail("call log full");
        return &overflow;
    }
    call = &h->calls[h->ncalls++];
    memset(call, 0, sizeof(*call));
    call->kind = kind;
    call->time = h->now;
    return call;
}

static bool H_Open(void *userdata)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'O');
    if (h->open_failures > 0) {
        --h->open_failures;
        return false;
    }
    return true;
}

static bool H_SetLine(void *userdata, const SDL_SerialLineConfig *config)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'L')->config = *config;
    if (h->fail_rate && config->BaudRate == h->fail_rate) {
        return false;
    }
    return !(h->reject_toggle && config->fRtsControl == SDL_SERIAL_RTS_CONTROL_TOGGLE);
}

static bool H_SetTimeouts(void *userdata, const SDL_SerialTimeouts *timeouts)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'T')->timeouts = *timeouts;
    return true;
}

static bool H_Purge(void *userdata, uint32_t flags)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'P')->value = flags;
    return true;
}

static bool H_Escape(void *userdata, uint32_t function)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'E')->value = function;
    return true;
}

static SDL_SerialIO H_Write(void *userdata, const uint8_t *data, size_t length)
{
    Harness *h = (Harness *)userdata;
    H_Call *call = H_Record(h, 'W');

    if (length <= sizeof(call->data)) {
        memcpy(call->data, data, length);
        call->length = length;
    }
    if (h->fail_writes) {
        return SDL_SERIAL_IO_FAILED;
    }
    return h->pend_writes ? SDL_SERIAL_IO_PENDING : SDL_SERIAL_IO_DONE;
}

static bool H_Drain(void *userdata)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'D');
    return true;
}

static void H_Close(void *userdata)
{
    Harness *h = (Harness *)userdata;

    H_Record(h, 'C');
}

static void H_PresenceChanged(void *userdata, int sub, uint32_t generation, const SDL_SerialIdentity *identity)
{
    Harness *h = (Harness *)userdata;

    h->presence[sub] = generation;
    if (identity) {
        h->identity[sub] = *identity;
    }
    if (h->npresence < H_MAX_PRESENCE) {
        H_Presence *p = &h->presence_log[h->npresence++];
        memset(p, 0, sizeof(*p));
        p->sub = sub;
        p->generation = generation;
        p->time = h->now;
        if (identity) {
            p->identity = *identity;
        }
    } else {
        H_Fail("presence log full");
    }
}

static void H_Publish(void *userdata, const SDL_SerialQueueEntry *entry)
{
    Harness *h = (Harness *)userdata;

    if (h->npublished < H_MAX_PUBLISHED) {
        h->published[h->npublished++] = *entry;
    } else {
        H_Fail("publish log full");
    }
}

static void H_Log(void *userdata, const char *text)
{
    Harness *h = (Harness *)userdata;
    size_t length = strlen(text);

    ++h->nlogs;
    if (length >= sizeof(h->last_log)) {
        length = sizeof(h->last_log) - 1;
    }
    memcpy(h->last_log, text, length);
    h->last_log[length] = '\0';
}

static const SDL_SerialPortOps harness_ops = {
    H_Open, H_SetLine, H_SetTimeouts, H_Purge, H_Escape, H_Write, H_Drain, H_Close, H_PresenceChanged, H_Publish, H_Log
};

static uint64_t H_NS(uint64_t ms)
{
    return ms * 1000000;
}

/* Runs the engine at every deadline up to and including t, then at t */
static void H_Advance(Harness *h, uint64_t t)
{
    int guard = 0;

    for (;;) {
        uint64_t deadline;

        if (!SDL_SerialEngine_GetDeadline(&h->engine, &deadline) || deadline > t) {
            break;
        }
        if (deadline > h->now) {
            h->now = deadline;
        }
        SDL_SerialEngine_Run(&h->engine, H_NS(h->now));
        if (++guard > 100000) {
            H_Fail("deadline loop");
            break;
        }
    }
    if (t > h->now) {
        h->now = t;
    }
    SDL_SerialEngine_Run(&h->engine, H_NS(h->now));
}

static Harness *H_Create(const SDL_SerialModule *module)
{
    Harness *h = (Harness *)calloc(1, sizeof(Harness));

    h->module = module;
    h->state = calloc(1, module->state_size);
    SDL_SerialEngine_Init(&h->engine, &harness_ops, h, module, h->state, 0);
    return h;
}

static void H_Destroy(Harness *h)
{
    free(h->state);
    free(h);
}

/* Starts the engine at the current time */
static void H_Start(Harness *h)
{
    SDL_SerialEngine_Run(&h->engine, H_NS(h->now));
}

/* Feeds from an exact-size heap copy, so a read past the count reaches the sanitizer */
static void H_Feed(Harness *h, const uint8_t *data, size_t length)
{
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (length) {
        memcpy(copy, data, length);
    }
    SDL_SerialEngine_Received(&h->engine, copy, length, H_NS(h->now));
    free(copy);
}

static void H_FeedAt(Harness *h, uint64_t t, const uint8_t *data, size_t length)
{
    H_Advance(h, t);
    H_Feed(h, data, length);
}

static void H_FeedText(Harness *h, const char *text)
{
    H_Feed(h, (const uint8_t *)text, strlen(text));
}

/* Feeds a buffer whose bytes past the count are 64 bytes of 00, then 64 of FF */
static void H_FeedPadded(Harness *h, const uint8_t *data, size_t length)
{
    uint8_t *buffer = (uint8_t *)malloc(length + 128);

    memcpy(buffer, data, length);
    memset(buffer + length, 0x00, 64);
    memset(buffer + length + 64, 0xFF, 64);
    SDL_SerialEngine_Received(&h->engine, buffer, length, H_NS(h->now));
    free(buffer);
}

static void H_CompleteWrite(Harness *h, bool success)
{
    SDL_SerialEngine_WriteDone(&h->engine, success, H_NS(h->now));
}

static void H_LosePort(Harness *h)
{
    SDL_SerialEngine_Lost(&h->engine, H_NS(h->now));
}

static const H_Call *H_NextCall(Harness *h)
{
    if (h->cursor >= h->ncalls) {
        return NULL;
    }
    return &h->calls[h->cursor++];
}

/* Skips to the end of the call log */
static void H_SkipCalls(Harness *h)
{
    h->cursor = h->ncalls;
}

static bool H_IsCall(const H_Call *call, char kind, uint64_t time)
{
    return call && call->kind == kind && call->time == time;
}

static bool H_IsWrite(const H_Call *call, const uint8_t *data, size_t length, uint64_t time)
{
    return call && call->kind == 'W' && call->time == time && call->length == length && memcmp(call->data, data, length) == 0;
}

static bool H_IsWriteText(const H_Call *call, const char *text, uint64_t time)
{
    return H_IsWrite(call, (const uint8_t *)text, strlen(text), time);
}

static bool H_IsEscape(const H_Call *call, uint32_t function, uint64_t time)
{
    return call && call->kind == 'E' && call->time == time && call->value == function;
}

/* The DCB fields every line gets, with the rate, framing and modem lines of the module */
static bool H_IsLine(const H_Call *call, uint32_t rate, int data_bits, int parity, int stop_bits, int rts_control, bool cts_flow, int dtr_control, uint64_t time)
{
    const SDL_SerialLineConfig *c;

    if (!call || call->kind != 'L' || call->time != time) {
        return false;
    }
    c = &call->config;
    return c->BaudRate == rate && c->ByteSize == data_bits && c->Parity == parity &&
           c->fParity == (parity != SDL_SERIAL_NOPARITY) &&
           c->StopBits == ((stop_bits == 2) ? SDL_SERIAL_TWOSTOPBITS : SDL_SERIAL_ONESTOPBIT) &&
           c->fBinary && !c->fNull && !c->fErrorChar && !c->fAbortOnError &&
           !c->fOutX && !c->fInX && !c->fTXContinueOnXoff && !c->fOutxDsrFlow && !c->fDsrSensitivity &&
           c->fOutxCtsFlow == cts_flow && c->fRtsControl == rts_control && c->fDtrControl == dtr_control;
}

/* A plain line with no flow control and both modem lines on */
static bool H_IsPlainLine(const H_Call *call, uint32_t rate, int data_bits, int parity, int stop_bits, uint64_t time)
{
    return H_IsLine(call, rate, data_bits, parity, stop_bits, SDL_SERIAL_RTS_CONTROL_ENABLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, time);
}

/* Open, the first line, the timeouts and the purge, in that order */
static bool H_ExpectOpened(Harness *h, uint32_t rate, int data_bits, int parity, int stop_bits, uint64_t time)
{
    const H_Call *open = H_NextCall(h);
    const H_Call *line = H_NextCall(h);
    const H_Call *timeouts = H_NextCall(h);
    const H_Call *purge = H_NextCall(h);

    return H_IsCall(open, 'O', time) && H_IsPlainLine(line, rate, data_bits, parity, stop_bits, time) &&
           H_IsCall(timeouts, 'T', time) &&
           timeouts->timeouts.ReadIntervalTimeout == 0xFFFFFFFFu &&
           timeouts->timeouts.ReadTotalTimeoutMultiplier == 0xFFFFFFFFu &&
           timeouts->timeouts.ReadTotalTimeoutConstant == 1000 &&
           timeouts->timeouts.WriteTotalTimeoutMultiplier == 20 &&
           timeouts->timeouts.WriteTotalTimeoutConstant == 1000 &&
           H_IsCall(purge, 'P', time) && purge->value == 0x0F;
}

static const SDL_SerialSnapshot *H_Snapshot(Harness *h, int sub)
{
    return h->module->GetSnapshot(h->state, sub);
}

static const SDL_SerialQueueEntry *H_Last(Harness *h)
{
    return h->npublished ? &h->published[h->npublished - 1] : NULL;
}

static int16_t H_Axis(Harness *h, int sub, int axis)
{
    return H_Snapshot(h, sub)->controls.axes[axis];
}

static bool H_Button(Harness *h, int sub, int button)
{
    return SDL_Serial_GetButton(&H_Snapshot(h, sub)->controls, button);
}

static bool H_NoButtons(Harness *h, int sub)
{
    int i;

    for (i = 0; i < SDL_SERIAL_MAX_BUTTONS; ++i) {
        if (H_Button(h, sub, i)) {
            return false;
        }
    }
    return true;
}

/* Exactly the buttons listed, ending with -1 */
static bool H_OnlyButtons(Harness *h, int sub, const int *buttons)
{
    SDL_SerialControls expected;
    int i;

    memset(&expected, 0, sizeof(expected));
    for (i = 0; buttons[i] >= 0; ++i) {
        SDL_Serial_SetButton(&expected, buttons[i], true);
    }
    return memcmp(expected.buttons, H_Snapshot(h, sub)->controls.buttons, sizeof(expected.buttons)) == 0;
}

static bool H_OnlyButton(Harness *h, int sub, int button)
{
    int list[2];

    list[0] = button;
    list[1] = -1;
    return H_OnlyButtons(h, sub, list);
}

/* Battery A. The harness is copied whole, so a copy restores at the same
 * address and every pointer inside stays valid. */
typedef struct H_Saved
{
    Harness harness;
    uint8_t *state;
} H_Saved;

static H_Saved *H_Save(Harness *h)
{
    H_Saved *saved = (H_Saved *)malloc(sizeof(H_Saved));

    saved->harness = *h;
    saved->state = (uint8_t *)malloc(h->module->state_size);
    memcpy(saved->state, h->state, h->module->state_size);
    return saved;
}

static void H_Restore(Harness *h, const H_Saved *saved)
{
    void *state = h->state;

    *h = saved->harness;
    h->state = state;
    memcpy(h->state, saved->state, h->module->state_size);
}

static void H_FreeSaved(H_Saved *saved)
{
    free(saved->state);
    free(saved);
}

/* The published entries since mark, compared without their stamps */
static bool H_SamePublished(const Harness *a, int mark_a, const Harness *b, int mark_b)
{
    int i;

    if (a->npublished - mark_a != b->npublished - mark_b) {
        return false;
    }
    for (i = 0; i < a->npublished - mark_a; ++i) {
        const SDL_SerialQueueEntry *x = &a->published[mark_a + i];
        const SDL_SerialQueueEntry *y = &b->published[mark_b + i];

        if (x->sub != y->sub || x->generation != y->generation ||
            !SDL_Serial_ControlsEqual(&x->controls, &y->controls) ||
            x->controls.ball[0] != y->controls.ball[0] || x->controls.ball[1] != y->controls.ball[1]) {
            return false;
        }
    }
    return true;
}

static bool H_SameState(Harness *a, Harness *b)
{
    int sub;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        const SDL_SerialSnapshot *x = H_Snapshot(a, sub);
        const SDL_SerialSnapshot *y = H_Snapshot(b, sub);

        if (x->present != y->present || !SDL_Serial_ControlsEqual(&x->controls, &y->controls)) {
            return false;
        }
        if (a->presence[sub] != b->presence[sub]) {
            return false;
        }
    }
    return true;
}

/* Brings a fresh or reset port back to a present device, for (A4) */
typedef void (*H_BringUpFunc)(Harness *h);

typedef struct H_Vector
{
    const char *label;
    const uint8_t *data;
    size_t length;
    int changes; /* Published entries one feed produces from the baseline */
    int repeat_changes; /* Entries the same packet produces again, 0 unless it pulses or moves a ball */
} H_Vector;

static void H_SetVector(H_Vector *v, const char *label, const uint8_t *bytes, size_t length, int changes, int repeat_changes)
{
    v->label = label;
    v->data = bytes;
    v->length = length;
    v->changes = changes;
    v->repeat_changes = repeat_changes;
}

static int battery_failures_before;

static void H_Report(const char *label, const char *step)
{
    if (failures != battery_failures_before) {
        printf("  in battery A step %s, vector %s\n", step, label);
        battery_failures_before = failures;
    }
}

/* Runs A1 to A5 on one vector from the state in h, which must hold a present
 * device. The harness is restored afterward. */
static void H_BatteryA(Harness *h, const H_Vector *v, H_BringUpFunc bring_up)
{
    H_Saved *base = H_Save(h);
    Harness *reference = (Harness *)malloc(sizeof(Harness));
    void *reference_state = malloc(h->module->state_size);
    const int mark = h->npublished;
    size_t k, i;

    battery_failures_before = failures;

    /* One feed is the reference */
    H_Feed(h, v->data, v->length);
    CHECK(h->npublished - mark == v->changes);
    *reference = *h;
    memcpy(reference_state, h->state, h->module->state_size);
    reference->state = reference_state;
    H_Report(v->label, "reference");

    /* (A1) */
    for (k = 1; k < v->length; ++k) {
        H_Restore(h, base);
        H_Feed(h, v->data, k);
        CHECK(h->npublished == mark);
        CHECK(h->npresence == base->harness.npresence);
    }
    H_Report(v->label, "A1");

    /* (A2) */
    for (k = 1; k < v->length; ++k) {
        H_Restore(h, base);
        H_Feed(h, v->data, k);
        H_Feed(h, v->data + k, v->length - k);
        CHECK(H_SamePublished(h, mark, reference, mark));
        CHECK(H_SameState(h, reference));
    }
    H_Restore(h, base);
    for (i = 0; i < v->length; ++i) {
        H_Feed(h, v->data + i, 1);
    }
    CHECK(H_SamePublished(h, mark, reference, mark));
    CHECK(H_SameState(h, reference));
    H_Report(v->label, "A2");

    /* (A3) */
    H_Restore(h, base);
    H_FeedPadded(h, v->data, v->length);
    CHECK(H_SamePublished(h, mark, reference, mark));
    CHECK(H_SameState(h, reference));
    H_Report(v->label, "A3");

    /* (A4): the port goes and returns after part of the packet. The module
     * starts over, so the part never joins the next packet. */
    if (bring_up && v->length > 1) {
        for (k = 1; k < v->length; ++k) {
            int mark4;

            H_Restore(h, base);
            H_Feed(h, v->data, k);
            H_LosePort(h);
            H_Advance(h, h->now + SDL_SERIAL_RETRY_MS);
            bring_up(h);
            CHECK(H_Snapshot(h, 0)->present);
            mark4 = h->npublished;
            H_Feed(h, v->data, v->length);
            CHECK(h->npublished - mark4 == v->changes);
            for (i = 0; i < SDL_SERIAL_MAX_SUBDEVICES; ++i) {
                CHECK(H_Snapshot(h, (int)i)->present == H_Snapshot(reference, (int)i)->present);
                CHECK(SDL_Serial_ControlsEqual(&H_Snapshot(h, (int)i)->controls, &H_Snapshot(reference, (int)i)->controls));
            }
        }
        H_Report(v->label, "A4");
    }

    /* (A5) */
    H_Restore(h, base);
    H_Feed(h, v->data, v->length);
    {
        const int mark5 = h->npublished;

        H_Feed(h, v->data, v->length);
        CHECK(h->npublished - mark5 == v->repeat_changes);
    }
    H_Report(v->label, "A5");

    H_Restore(h, base);
    H_FreeSaved(base);
    free(reference_state);
    free(reference);
}

/* (A6): a malformed packet changes nothing, then the valid one decodes */
static void H_BatteryA6(Harness *h, const char *label, const uint8_t *bad, size_t bad_length, const H_Vector *valid)
{
    H_Saved *base = H_Save(h);
    int mark;

    battery_failures_before = failures;
    mark = h->npublished;
    H_Feed(h, bad, bad_length);
    CHECK(h->npublished == mark);
    CHECK(h->npresence == base->harness.npresence);
    H_Feed(h, valid->data, valid->length);
    CHECK(h->npublished - mark == valid->changes);
    H_Report(label, "A6");
    H_Restore(h, base);
    H_FreeSaved(base);
}

static int H_Finish(void)
{
    if (failures) {
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("PASSED: %d checks, 0 failures\n", checks);
    return 0;
}

#endif /* testserialharness_h_ */
