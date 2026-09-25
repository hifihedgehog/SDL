/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* A harness for the RFCOMM replay tests of hifihedgehog/SDL#33 Part 11: one
   link over one family module on an injected clock, with every action the
   link asks for and every snapshot the module emits logged in order. */

#ifndef testrfcommharness_h_
#define testrfcommharness_h_

#include "testserialharness.h"
#include "../src/joystick/windows/SDL_rfcomm_proto.h"
#include "../src/joystick/windows/SDL_rfcomm_moga_proto.h"
#include "../src/joystick/windows/SDL_rfcomm_zeemote_proto.h"
#include "../src/joystick/windows/SDL_rfcomm_bgp100_proto.h"

#define R_MAX_LOG       256
#define R_MAX_SNAPSHOTS 256

typedef struct R_Action
{
    SDL_RFCOMMAction action;
    uint64_t time;
} R_Action;

typedef struct RHarness
{
    const SDL_RFCOMMModule *module;
    SDL_RFCOMMLink link;
    void *state;
    uint64_t now;
    R_Action log[R_MAX_LOG];
    int nlog;
    int cursor;
    SDL_SerialSnapshot snapshots[R_MAX_SNAPSHOTS]; /* The first ones */
    int nsnapshots;                                /* Every one emitted */
    SDL_SerialSnapshot latest;
} RHarness;

static void R_Changed(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    RHarness *r = (RHarness *)userdata;

    CHECK(sub == 0);
    if (r->nsnapshots < R_MAX_SNAPSHOTS) {
        r->snapshots[r->nsnapshots] = *snapshot;
    }
    ++r->nsnapshots;
    r->latest = *snapshot;
}

/* The number of snapshots a module emits for one feed right after its
   reset, with no link around it */
static void R_CountChanged(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    (void)sub;
    (void)snapshot;
    ++*(int *)userdata;
}

static int R_CountSnapshots(const SDL_RFCOMMModule *module, void *state, const uint8_t *data, size_t length)
{
    SDL_SerialSink sink;
    int count = 0;

    sink.userdata = &count;
    sink.changed = R_CountChanged;
    sink.log = NULL;
    module->Reset(state, &sink, "", -1, 0);
    module->Feed(state, data, length, 0);
    return count;
}

static void R_Pump(RHarness *r)
{
    SDL_RFCOMMAction action;

    while (SDL_RFCOMMLink_NextAction(&r->link, &action)) {
        if (r->nlog < R_MAX_LOG) {
            r->log[r->nlog].action = action;
            r->log[r->nlog].time = r->now;
            ++r->nlog;
        }
    }
}

/* Runs the link at every deadline up to and including to, then leaves the
   clock at to */
static void R_Advance(RHarness *r, uint64_t to)
{
    int guard = 0;

    for (;;) {
        uint64_t deadline;

        if (!SDL_RFCOMMLink_GetDeadline(&r->link, &deadline) || deadline > to) {
            break;
        }
        if (deadline > r->now) {
            r->now = deadline;
        }
        SDL_RFCOMMLink_Tick(&r->link, r->now);
        R_Pump(r);
        if (++guard > 100000) {
            H_Fail("the link never stopped asking for a tick");
            break;
        }
    }
    if (to > r->now) {
        r->now = to;
    }
}

/* A link over the module, with its first connect queued at 0 */
static RHarness *R_Create(const SDL_RFCOMMModule *module, const char *name)
{
    RHarness *r = (RHarness *)calloc(1, sizeof(*r));
    SDL_SerialSink sink;

    r->module = module;
    r->state = calloc(1, module->state_size);
    sink.userdata = r;
    sink.changed = R_Changed;
    sink.log = NULL;
    SDL_RFCOMMLink_Init(&r->link, module, r->state, &sink, name, 0);
    R_Advance(r, 0);
    return r;
}

static void R_Destroy(RHarness *r)
{
    free(r->state);
    free(r);
}

static void R_Connected(RHarness *r, SDL_RFCOMMConnectResult result)
{
    SDL_RFCOMMLink_Connected(&r->link, result, r->now);
    R_Pump(r);
}

static void R_Receive(RHarness *r, const uint8_t *data, size_t length)
{
    /* An exact heap copy, so a read past the length reaches the sanitizer */
    uint8_t *copy = (uint8_t *)malloc(length ? length : 1);

    if (length) {
        memcpy(copy, data, length);
    }
    SDL_RFCOMMLink_Received(&r->link, copy, length, r->now);
    free(copy);
    R_Pump(r);
}

static void R_Lost(RHarness *r)
{
    SDL_RFCOMMLink_Lost(&r->link, r->now);
    R_Pump(r);
}

static void R_SetPlayer(RHarness *r, int player_index)
{
    SDL_RFCOMMLink_SetPlayerIndex(&r->link, player_index, r->now);
    R_Pump(r);
}

static const R_Action *R_Next(RHarness *r)
{
    return (r->cursor < r->nlog) ? &r->log[r->cursor++] : NULL;
}

static void R_Skip(RHarness *r)
{
    r->cursor = r->nlog;
}

static bool R_IsConnect(const R_Action *a, uint8_t channel, uint64_t time)
{
    return a && a->action.kind == SDL_RFCOMM_ACTION_CONNECT && a->action.channel == channel && a->time == time;
}

static bool R_IsClose(const R_Action *a, uint64_t time)
{
    return a && a->action.kind == SDL_RFCOMM_ACTION_CLOSE && a->time == time;
}

static bool R_IsSend(const R_Action *a, const uint8_t *data, size_t length, uint64_t time)
{
    return a && a->action.kind == SDL_RFCOMM_ACTION_SEND && a->action.length == length &&
           memcmp(a->action.data, data, length) == 0 && a->time == time;
}

/* A link whose connect succeeded at the current time, with the actions
   so far consumed */
static RHarness *R_Up(const SDL_RFCOMMModule *module, const char *name)
{
    RHarness *r = R_Create(module, name);

    R_Connected(r, SDL_RFCOMM_CONNECT_OK);
    R_Skip(r);
    return r;
}

static const SDL_SerialSnapshot *R_Last(const RHarness *r)
{
    return (r->nsnapshots > 0) ? &r->latest : NULL;
}

static bool R_Present(const RHarness *r)
{
    const SDL_SerialSnapshot *last = R_Last(r);

    return last && last->present;
}

/* The buttons of the last snapshot as a bit mask, button n at bit n */
static uint32_t R_Buttons(const RHarness *r)
{
    const SDL_SerialSnapshot *last = R_Last(r);
    uint32_t mask = 0;
    int i;

    if (!last) {
        return 0;
    }
    for (i = 0; i < 32; ++i) {
        if (SDL_Serial_GetButton(&last->controls, i)) {
            mask |= (uint32_t)1 << i;
        }
    }
    return mask;
}

static int16_t R_Axis(const RHarness *r, int axis)
{
    const SDL_SerialSnapshot *last = R_Last(r);

    return last ? last->controls.axes[axis] : 0;
}

static bool R_SameSnapshots(const RHarness *a, const RHarness *b)
{
    int i;

    if (a->nsnapshots != b->nsnapshots || a->nsnapshots > R_MAX_SNAPSHOTS) {
        return false;
    }
    for (i = 0; i < a->nsnapshots; ++i) {
        if (a->snapshots[i].present != b->snapshots[i].present ||
            !SDL_Serial_ControlsEqual(&a->snapshots[i].controls, &b->snapshots[i].controls)) {
            return false;
        }
    }
    return true;
}

/* Feeds a byte stream whole, one byte at a time and split at every point,
   and checks that every way gives the same snapshots as the whole feed */
static void R_CheckSplits(const SDL_RFCOMMModule *module, const char *name, const uint8_t *data, size_t length)
{
    RHarness *whole = R_Up(module, name);
    RHarness *bytes = R_Up(module, name);
    size_t i, split;

    R_Receive(whole, data, length);
    CHECK(whole->nsnapshots > 0);
    for (i = 0; i < length; ++i) {
        R_Receive(bytes, data + i, 1);
    }
    CHECK(R_SameSnapshots(whole, bytes));
    for (split = 1; split < length; ++split) {
        RHarness *two = R_Up(module, name);

        R_Receive(two, data, split);
        R_Receive(two, data + split, length - split);
        CHECK(R_SameSnapshots(whole, two));
        R_Destroy(two);
    }
    R_Destroy(whole);
    R_Destroy(bytes);
}

#endif /* testrfcommharness_h_ */
