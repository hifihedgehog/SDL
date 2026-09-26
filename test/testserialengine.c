/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Engine tests for src/joystick/serial/SDL_serial_engine.c, the serial
   transport of hifihedgehog/SDL#33 Part 5, against a scripted fake port:
   the retry schedule, the DCB fields and timeouts, action order, presence
   cleared on loss and restored as a new instance after the port returns,
   snapshot order, the 64-entry queue, the hint list and its changes, the
   axis scaling rules and the helpers every module shares. */

#include "testserialharness.h"

/* A probe module. Reset sets 9600 8N1 and writes "HI". Each fed byte is a
   command: P and Q present sub-devices 0 and 1, A takes 0 away, a digit sets
   axis 0 of sub-device 0, M lowers RTS, D drains, T and R set RTS toggle and
   RTS/CTS lines, N a plain line again, W writes "W", X pulses button 0, B
   moves the ball by 5, S arms a 50 ms deadline, K starts a line break and k
   ends it. C asks for a close sequence, which writes "BYE" and ends 100 ms
   later, H asks for one that never ends, L for one that queues "LATE" as it
   ends, U for one of writes that each queue the next and that ends on the
   256th, and c asks for none. z says closed with no sequence running. */
typedef struct ProbeState
{
    SDL_SerialBase base;
    int dones;
    int failed_dones;
    uint8_t last_tag;
    bool timer;
    uint64_t timer_at;
    int ticks;
    int outputs;
    SDL_SerialOutput last_output;
    bool close_hangs;
    bool close_late;
    bool close_runaway;
    int runaway_dones;
    bool close_started;
    bool close_timer;
    uint64_t close_at;
} ProbeState;

static void Probe_Reset(void *state, const SDL_SerialSink *sink, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;
    const SDL_SerialLine line = SDL_Serial_Line(9600, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);

    (void)now;
    SDL_Serial_ResetBase(&s->base, sink);
    s->dones = 0;
    s->failed_dones = 0;
    s->last_tag = 0;
    s->timer = false;
    s->ticks = 0;
    s->outputs = 0;
    s->close_hangs = false;
    s->close_late = false;
    s->close_runaway = false;
    s->runaway_dones = 0;
    s->close_started = false;
    s->close_timer = false;
    SDL_Serial_QueueLine(&s->base, 1, &line);
    SDL_Serial_QueueWrite(&s->base, 2, (const uint8_t *)"HI", 2);
}

static void Probe_Feed(void *state, const uint8_t *data, size_t length, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;
    SDL_SerialIdentity identity;
    SDL_SerialControls controls;
    size_t i;

    for (i = 0; i < length; ++i) {
        const uint8_t c = data[i];

        switch (c) {
        case 'P':
        case 'Q':
            SDL_Serial_SetIdentity(&identity, (c == 'P') ? "Probe" : "Probe 2", SDL_SERIAL_TYPE_GAMEPAD, 2, 4, 1, 1);
            SDL_Serial_Present(&s->base, (c == 'P') ? 0 : 1, &identity);
            break;
        case 'A':
            SDL_Serial_Absent(&s->base, 0);
            break;
        case 'M':
            SDL_Serial_QueueModem(&s->base, 3, true, false);
            break;
        case 'D':
            SDL_Serial_QueueDrain(&s->base, 4);
            break;
        case 'T':
        {
            const SDL_SerialLine line = SDL_Serial_Line(115200, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_RTS_TOGGLE, true, true);
            SDL_Serial_QueueLine(&s->base, 5, &line);
            break;
        }
        case 'R':
        {
            const SDL_SerialLine line = SDL_Serial_Line(9600, 8, SDL_SERIAL_PARITY_NONE, 2, SDL_SERIAL_FLOW_RTSCTS, true, true);
            SDL_Serial_QueueLine(&s->base, 6, &line);
            break;
        }
        case 'N':
        {
            const SDL_SerialLine line = SDL_Serial_Line(9600, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_NONE, true, true);
            SDL_Serial_QueueLine(&s->base, 7, &line);
            break;
        }
        case 'W':
            SDL_Serial_QueueWrite(&s->base, 8, (const uint8_t *)"W", 1);
            break;
        case 'X':
            SDL_Serial_Pulse(&s->base, 0, 0, now);
            break;
        case 'B':
            controls = s->base.snapshots[0].controls;
            controls.ball[0] = 5;
            SDL_Serial_Commit(&s->base, 0, &controls);
            break;
        case 'S':
            s->timer = true;
            s->timer_at = now + 50;
            break;
        case 'Z':
            /* Controls built from nothing */
            memset(&controls, 0, sizeof(controls));
            SDL_Serial_Commit(&s->base, 0, &controls);
            break;
        case 'K':
            SDL_Serial_QueueBreak(&s->base, 9, true);
            break;
        case 'k':
            SDL_Serial_QueueBreak(&s->base, 10, false);
            break;
        case 'C':
            s->base.close_sequence = true;
            break;
        case 'H':
            s->base.close_sequence = true;
            s->close_hangs = true;
            break;
        case 'c':
            s->base.close_sequence = false;
            break;
        case 'L':
            s->base.close_sequence = true;
            s->close_late = true;
            break;
        case 'U':
            s->base.close_sequence = true;
            s->close_runaway = true;
            s->close_hangs = true;
            break;
        case 'z':
            s->base.closed = true;
            break;
        default:
            if (c >= '0' && c <= '9') {
                controls = s->base.snapshots[0].controls;
                controls.axes[0] = (int16_t)(c - '0');
                SDL_Serial_Commit(&s->base, 0, &controls);
            }
            break;
        }
    }
}

static void Probe_Tick(void *state, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    if (s->base.closing && !s->close_started) {
        s->close_started = true;
        SDL_Serial_ClearActions(&s->base);
        SDL_Serial_QueueWrite(&s->base, 11, (const uint8_t *)"BYE", 3);
        if (!s->close_hangs) {
            s->close_timer = true;
            s->close_at = now + 100;
        }
    }
    if (s->close_timer && now >= s->close_at) {
        s->close_timer = false;
        if (s->close_late) {
            SDL_Serial_QueueWrite(&s->base, 12, (const uint8_t *)"LATE", 4);
        }
        s->base.closed = true;
    }
    if (s->timer && now >= s->timer_at) {
        s->timer = false;
        ++s->ticks;
    }
    SDL_Serial_TickPulses(&s->base, now);
}

static void Probe_ActionDone(void *state, bool success, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;
    uint8_t tag;

    (void)now;
    if (SDL_Serial_FinishAction(&s->base, &tag)) {
        ++s->dones;
        if (!success) {
            ++s->failed_dones;
        }
        s->last_tag = tag;
        if (s->close_runaway && s->base.closing) {
            if (++s->runaway_dones == 256) {
                s->base.closed = true;
            } else {
                SDL_Serial_QueueWrite(&s->base, 13, (const uint8_t *)"U", 1);
            }
        }
    }
}

static void Probe_Output(void *state, const SDL_SerialOutput *request, uint64_t now)
{
    ProbeState *s = (ProbeState *)state;

    (void)now;
    ++s->outputs;
    s->last_output = *request;
}

static bool Probe_GetDeadline(void *state, uint64_t *deadline)
{
    ProbeState *s = (ProbeState *)state;
    bool have = false;

    if (s->timer) {
        SDL_Serial_EarlierDeadline(&have, deadline, s->timer_at);
    }
    if (s->close_timer) {
        SDL_Serial_EarlierDeadline(&have, deadline, s->close_at);
    }
    SDL_Serial_PulseDeadline(&s->base, &have, deadline);
    return have;
}

static const SDL_SerialModule probe_module = {
    "probe", sizeof(ProbeState), true, 1, 8,
    Probe_Reset, Probe_Feed, Probe_Tick, SDL_Serial_NextAction, Probe_ActionDone, Probe_Output, Probe_GetDeadline, SDL_Serial_GetSnapshot
};

static ProbeState *Probe(Harness *h)
{
    return (ProbeState *)h->state;
}

static void FeedText(Harness *h, const char *text)
{
    H_FeedText(h, text);
}

/* Opens that fail, then one that succeeds. A rescan retries at once. */
static void TestRetry(void)
{
    Harness *h = H_Create(&probe_module);
    const H_Call *call;

    h->open_failures = 2;
    H_Start(h);
    H_Advance(h, 2500);
    CHECK(H_IsCall(H_NextCall(h), 'O', 0));
    CHECK(H_IsCall(H_NextCall(h), 'O', 1000));
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 2000));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 2000));
    CHECK(H_NextCall(h) == NULL);
    CHECK(h->engine.opens == 1);
    CHECK(SDL_SerialEngine_IsReading(&h->engine));
    H_Destroy(h);

    /* A rescan retries before the 1000 ms are up */
    h = H_Create(&probe_module);
    h->open_failures = 1;
    H_Start(h);
    H_Advance(h, 300);
    CHECK(!SDL_SerialEngine_IsReading(&h->engine));
    SDL_SerialEngine_Rescan(&h->engine, H_NS(300));
    CHECK(H_IsCall(H_NextCall(h), 'O', 0));
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 300));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 300));
    /* A rescan of an open port does nothing */
    SDL_SerialEngine_Rescan(&h->engine, H_NS(400));
    CHECK(H_NextCall(h) == NULL);
    H_Destroy(h);

    /* Closed, the deadline is the retry */
    h = H_Create(&probe_module);
    h->open_failures = 5;
    H_Start(h);
    {
        uint64_t deadline = 0;
        CHECK(SDL_SerialEngine_GetDeadline(&h->engine, &deadline) && deadline == 1000);
    }
    /* Bytes that arrive while closed are ignored */
    FeedText(h, "P");
    CHECK(h->npresence == 0);
    call = H_NextCall(h);
    CHECK(H_IsCall(call, 'O', 0));
    H_Destroy(h);
}

static void TestLineConfig(void)
{
    SDL_SerialLineConfig c;
    SDL_SerialLine line;
    SDL_SerialTimeouts t;

    line = SDL_Serial_Line(1200, 7, SDL_SERIAL_PARITY_NONE, 2, SDL_SERIAL_FLOW_NONE, true, true);
    SDL_Serial_BuildLineConfig(&line, &c);
    CHECK(c.BaudRate == 1200 && c.ByteSize == 7 && c.Parity == 0 && !c.fParity && c.StopBits == 2);
    CHECK(c.fBinary && !c.fNull && !c.fErrorChar && !c.fAbortOnError);
    CHECK(!c.fOutX && !c.fInX && !c.fOutxDsrFlow && !c.fDsrSensitivity && !c.fOutxCtsFlow && !c.fTXContinueOnXoff);
    CHECK(c.fDtrControl == 1 && c.fRtsControl == 1);

    line = SDL_Serial_Line(4800, 8, SDL_SERIAL_PARITY_ODD, 1, SDL_SERIAL_FLOW_NONE, false, false);
    SDL_Serial_BuildLineConfig(&line, &c);
    CHECK(c.BaudRate == 4800 && c.ByteSize == 8 && c.Parity == 1 && c.fParity && c.StopBits == 0);
    CHECK(c.fDtrControl == 0 && c.fRtsControl == 0 && !c.fOutxCtsFlow);

    line = SDL_Serial_Line(9600, 8, SDL_SERIAL_PARITY_EVEN, 2, SDL_SERIAL_FLOW_RTSCTS, true, false);
    SDL_Serial_BuildLineConfig(&line, &c);
    CHECK(c.Parity == 2 && c.fParity && c.StopBits == 2);
    CHECK(c.fOutxCtsFlow && c.fRtsControl == 2 && c.fDtrControl == 1);

    line = SDL_Serial_Line(115200, 8, SDL_SERIAL_PARITY_NONE, 1, SDL_SERIAL_FLOW_RTS_TOGGLE, true, false);
    SDL_Serial_BuildLineConfig(&line, &c);
    CHECK(!c.fOutxCtsFlow && c.fRtsControl == 3 && c.fDtrControl == 1 && c.BaudRate == 115200);

    SDL_Serial_GetTimeouts(&t);
    CHECK(t.ReadIntervalTimeout == 0xFFFFFFFFu && t.ReadTotalTimeoutMultiplier == 0xFFFFFFFFu);
    CHECK(t.ReadTotalTimeoutConstant == 1000 && t.WriteTotalTimeoutMultiplier == 20 && t.WriteTotalTimeoutConstant == 1000);
}

/* One action at a time, a pending write holding the next */
static void TestActions(void)
{
    Harness *h = H_Create(&probe_module);
    const H_Call *call;

    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 0));
    CHECK(Probe(h)->dones == 1 && Probe(h)->last_tag == 1);
    FeedText(h, "W");
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 10);
    H_CompleteWrite(h, true);
    CHECK(Probe(h)->dones == 2 && Probe(h)->last_tag == 2);
    CHECK(H_IsWriteText(H_NextCall(h), "W", 10));
    H_Advance(h, 20);
    H_CompleteWrite(h, false);
    CHECK(Probe(h)->dones == 3 && Probe(h)->failed_dones == 1 && Probe(h)->last_tag == 8);
    /* A completion with nothing pending is ignored */
    H_CompleteWrite(h, true);
    CHECK(Probe(h)->dones == 3);
    h->pend_writes = false;

    /* Modem lines with no flow control */
    FeedText(h, "M");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 20));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRRTS, 20));
    CHECK(Probe(h)->dones == 4 && Probe(h)->failed_dones == 1);
    CHECK(h->engine.line.dtr && !h->engine.line.rts);

    /* A drain with no flow control */
    FeedText(h, "D");
    CHECK(H_IsCall(H_NextCall(h), 'D', 20));
    CHECK(Probe(h)->dones == 5 && Probe(h)->failed_dones == 1);

    /* RTS/CTS: the line is set, then a drain and the modem lines are refused */
    FeedText(h, "R");
    call = H_NextCall(h);
    CHECK(H_IsLine(call, 9600, 8, SDL_SERIAL_NOPARITY, 2, SDL_SERIAL_RTS_CONTROL_HANDSHAKE, true, SDL_SERIAL_DTR_CONTROL_ENABLE, 20));
    FeedText(h, "DM");
    CHECK(H_NextCall(h) == NULL);
    CHECK(Probe(h)->dones == 8 && Probe(h)->failed_dones == 3);

    /* Only the first line sets the timeouts and purges */
    FeedText(h, "N");
    CHECK(H_IsPlainLine(H_NextCall(h), 9600, 8, SDL_SERIAL_NOPARITY, 1, 20));
    CHECK(H_NextCall(h) == NULL);

    /* RTS toggle, accepted, then the modem lines are refused */
    FeedText(h, "TM");
    CHECK(H_IsLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_TOGGLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 20));
    CHECK(H_NextCall(h) == NULL);
    CHECK(Probe(h)->failed_dones == 4);
    H_Destroy(h);

    /* A port that rejects RTS toggle runs with RTS off */
    h = H_Create(&probe_module);
    h->reject_toggle = true;
    H_Start(h);
    H_SkipCalls(h);
    FeedText(h, "T");
    CHECK(H_IsLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_TOGGLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 0));
    CHECK(H_IsLine(H_NextCall(h), 115200, 8, SDL_SERIAL_NOPARITY, 1, SDL_SERIAL_RTS_CONTROL_DISABLE, false, SDL_SERIAL_DTR_CONTROL_ENABLE, 0));
    CHECK(Probe(h)->last_tag == 5 && Probe(h)->failed_dones == 0);
    CHECK(h->engine.line.flow == SDL_SERIAL_FLOW_NONE && !h->engine.line.rts);
    /* So the modem lines move again */
    FeedText(h, "M");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETDTR, 0));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRRTS, 0));
    H_Destroy(h);

    /* A failed write is port loss, retried after 1000 ms */
    h = H_Create(&probe_module);
    h->fail_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 0));
    CHECK(H_IsCall(H_NextCall(h), 'C', 0));
    CHECK(!SDL_SerialEngine_IsReading(&h->engine) && h->engine.losses == 1);
    h->fail_writes = false;
    H_Advance(h, 999);
    CHECK(H_NextCall(h) == NULL);
    H_Advance(h, 1000);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 1000));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 1000));
    H_Destroy(h);
}

/* Presence, loss and return as a new instance, snapshot order */
static void TestPresence(void)
{
    Harness *h = H_Create(&probe_module);
    int i;

    H_Start(h);
    CHECK(h->npresence == 0);
    FeedText(h, "12");
    CHECK(h->npublished == 0); /* Nothing before presence */
    FeedText(h, "P");
    CHECK(h->npresence == 1 && h->presence_log[0].sub == 0 && h->presence_log[0].generation == 1);
    CHECK(strcmp(h->presence_log[0].identity.name, "Probe") == 0);
    CHECK(h->identity[0].type == SDL_SERIAL_TYPE_GAMEPAD && h->identity[0].naxes == 2 && h->identity[0].nbuttons == 4);
    CHECK(h->npublished == 1 && h->published[0].controls.axes[0] == 0);

    /* Snapshot order */
    H_Advance(h, 5);
    FeedText(h, "12345");
    CHECK(h->npublished == 6);
    for (i = 1; i <= 5; ++i) {
        CHECK(h->published[i].sub == 0 && h->published[i].generation == 1 && h->published[i].controls.axes[0] == i);
        CHECK(h->published[i].stamp_ns == H_NS(5));
    }
    /* No change, no snapshot */
    FeedText(h, "5");
    CHECK(h->npublished == 6);

    /* A second sub-device */
    FeedText(h, "Q");
    CHECK(h->presence[1] == 1 && h->npublished == 7 && h->published[6].sub == 1);

    /* Port loss clears every presence and closes the port */
    H_SkipCalls(h);
    H_Advance(h, 100);
    H_LosePort(h);
    CHECK(h->presence[0] == 0 && h->presence[1] == 0);
    CHECK(H_IsCall(H_NextCall(h), 'C', 100));
    /* A second loss report changes nothing */
    H_LosePort(h);
    CHECK(H_NextCall(h) == NULL && h->engine.losses == 1);

    /* The port returns: the module starts over and presents a new instance */
    H_Advance(h, 1100);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 1100));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 1100));
    CHECK(h->presence[0] == 0);
    FeedText(h, "P");
    CHECK(h->presence[0] == 2);
    CHECK(H_Last(h)->generation == 2 && H_Last(h)->controls.axes[0] == 0);

    /* Taking a sub-device away */
    FeedText(h, "A");
    CHECK(h->presence[0] == 0);
    FeedText(h, "7");
    CHECK(H_Last(h)->generation == 2 && H_Last(h)->controls.axes[0] == 0);
    FeedText(h, "P");
    CHECK(h->presence[0] == 3);

    /* Stop closes the port and clears presence */
    H_SkipCalls(h);
    SDL_SerialEngine_Stop(&h->engine);
    CHECK(H_IsCall(H_NextCall(h), 'C', 1100) && h->presence[0] == 0);
    CHECK(!SDL_SerialEngine_IsReading(&h->engine));
    H_Destroy(h);
}

/* Pulses, ball motion, deadlines and output */
static void TestPulseAndOutput(void)
{
    Harness *h = H_Create(&probe_module);
    SDL_SerialOutput request;
    uint64_t deadline;
    int mark;

    H_Start(h);
    FeedText(h, "P");
    mark = h->npublished;
    H_Advance(h, 1000);
    FeedText(h, "X");
    CHECK(h->npublished == mark + 1 && SDL_Serial_GetButton(&H_Last(h)->controls, 0));
    CHECK(SDL_SerialEngine_GetDeadline(&h->engine, &deadline) && deadline == 1100);
    /* A repeat at +50 releases, presses and restarts the pulse */
    H_Advance(h, 1050);
    FeedText(h, "X");
    CHECK(h->npublished == mark + 3);
    CHECK(!SDL_Serial_GetButton(&h->published[mark + 1].controls, 0));
    CHECK(SDL_Serial_GetButton(&h->published[mark + 2].controls, 0));
    H_Advance(h, 1149);
    CHECK(h->npublished == mark + 3 && H_Button(h, 0, 0));
    H_Advance(h, 1150);
    CHECK(h->npublished == mark + 4 && !H_Button(h, 0, 0));
    CHECK(!SDL_SerialEngine_GetDeadline(&h->engine, &deadline));
    /* A commit during a pulse keeps the button held, even one built from nothing */
    FeedText(h, "X3");
    CHECK(H_Button(h, 0, 0) && H_Axis(h, 0, 0) == 3);
    FeedText(h, "Z");
    CHECK(H_Button(h, 0, 0) && H_Axis(h, 0, 0) == 0);

    /* Every ball movement is a snapshot, the delta carried once */
    mark = h->npublished;
    FeedText(h, "BB");
    CHECK(h->npublished == mark + 2 && h->published[mark].controls.ball[0] == 5 && h->published[mark + 1].controls.ball[0] == 5);
    CHECK(H_Snapshot(h, 0)->controls.ball[0] == 0);

    /* Module deadlines run at their time */
    FeedText(h, "S");
    H_Advance(h, 1150 + 49);
    CHECK(Probe(h)->ticks == 0);
    H_Advance(h, 1150 + 50);
    CHECK(Probe(h)->ticks == 1);

    /* Output reaches the module only while the port is open */
    memset(&request, 0, sizeof(request));
    request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
    request.low_frequency_rumble = 0x1234;
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(Probe(h)->outputs == 1 && Probe(h)->last_output.low_frequency_rumble == 0x1234);
    H_LosePort(h);
    SDL_SerialEngine_Output(&h->engine, &request, H_NS(h->now));
    CHECK(Probe(h)->outputs == 1);
    H_Destroy(h);
}

static void TestQueue(void)
{
    SDL_SerialSnapshotQueue *queue = (SDL_SerialSnapshotQueue *)calloc(1, sizeof(*queue));
    SDL_SerialQueueEntry entry;
    int i;

    SDL_Serial_ClearQueue(queue);
    CHECK(!SDL_Serial_PopQueue(queue, &entry));
    memset(&entry, 0, sizeof(entry));
    for (i = 0; i < 70; ++i) {
        entry.controls.axes[0] = (int16_t)i;
        entry.stamp_ns = (uint64_t)i;
        SDL_Serial_PushQueue(queue, &entry);
    }
    CHECK(queue->count == 64 && queue->dropped == 6);
    for (i = 6; i < 70; ++i) {
        CHECK(SDL_Serial_PopQueue(queue, &entry) && entry.controls.axes[0] == i && entry.stamp_ns == (uint64_t)i);
    }
    CHECK(!SDL_Serial_PopQueue(queue, &entry));
    /* Wrapping keeps the order */
    for (i = 0; i < 100; ++i) {
        entry.controls.axes[0] = (int16_t)i;
        SDL_Serial_PushQueue(queue, &entry);
        CHECK(SDL_Serial_PopQueue(queue, &entry) && entry.controls.axes[0] == i);
    }
    free(queue);
}

static const SDL_SerialModule other_module = {
    "Other", sizeof(ProbeState), false, 0, 0,
    Probe_Reset, Probe_Feed, Probe_Tick, SDL_Serial_NextAction, Probe_ActionDone, Probe_Output, Probe_GetDeadline, SDL_Serial_GetSnapshot
};

static int log_calls;
static char log_last[256];

static void Log(void *userdata, const char *entry, size_t length, const char *reason)
{
    (void)userdata;
    (void)reason;
    ++log_calls;
    if (length >= sizeof(log_last)) {
        length = sizeof(log_last) - 1;
    }
    memcpy(log_last, entry, length);
    log_last[length] = '\0';
}

static void TestHint(void)
{
    const SDL_SerialModule *modules[2];
    SDL_SerialPortEntry entries[SDL_SERIAL_MAX_PORTS];
    SDL_SerialPortEntry old_entries[3];
    SDL_SerialPortChange old_changes[3], new_changes[3];
    unsigned int number = 0;
    int n, i;
    char many[2048];
    size_t used;

    modules[0] = &probe_module;
    modules[1] = &other_module;

    CHECK(SDL_Serial_ParseHint(NULL, modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL) == 0);
    CHECK(SDL_Serial_ParseHint("", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL) == 0);
    CHECK(log_calls == 0);

    n = SDL_Serial_ParseHint("COM3=probe,COM4=other", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == 2 && strcmp(entries[0].key, "COM3") == 0 && entries[0].module == &probe_module);
    CHECK(strcmp(entries[1].key, "COM4") == 0 && entries[1].module == &other_module);

    /* Spaces around tokens, case, a trailing comma */
    n = SDL_Serial_ParseHint("  com12 = PROBE ,\tCOM7=Other , ", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == 2 && strcmp(entries[0].key, "com12") == 0 && entries[0].module == &probe_module);
    CHECK(strcmp(entries[1].key, "COM7") == 0 && entries[1].module == &other_module);
    CHECK(log_calls == 0);

    /* Malformed entries and unknown protocols are skipped, one log call each */
    n = SDL_Serial_ParseHint("COM3,COM4=,=probe,LPT1=probe,COM0=probe,COM01=probe,COM12345=probe,COM5=nothing,COM6=probe", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == 1 && strcmp(entries[0].key, "COM6") == 0);
    CHECK(log_calls == 8);
    CHECK(strcmp(log_last, "COM5=nothing") == 0);

    /* A repeated port keeps its last entry, in its new place */
    log_calls = 0;
    n = SDL_Serial_ParseHint("COM3=probe,COM4=other,com3=other", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == 2 && strcmp(entries[0].key, "COM4") == 0 && strcmp(entries[1].key, "com3") == 0 && entries[1].module == &other_module);
    CHECK(log_calls == 0);

    /* Device instance ID prefixes */
    n = SDL_Serial_ParseHint("FTDIBUS\\VID_0403+PID_6001+A1B2C3D4A\\0000=probe , USB\\VID_2CA3&PID_1020&MI_02=other", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == 2 && strcmp(entries[0].key, "FTDIBUS\\VID_0403+PID_6001+A1B2C3D4A\\0000") == 0);
    CHECK(strcmp(entries[1].key, "USB\\VID_2CA3&PID_1020&MI_02") == 0);
    CHECK(SDL_Serial_InstanceMatches(entries[1].key, "USB\\VID_2CA3&PID_1020&MI_02\\7&1A2B&0&0002"));
    CHECK(SDL_Serial_InstanceMatches("usb\\vid_2ca3", "USB\\VID_2CA3&PID_1020"));
    CHECK(!SDL_Serial_InstanceMatches("USB\\VID_2CA3&PID_1021", "USB\\VID_2CA3&PID_1020&MI_02"));
    CHECK(!SDL_Serial_InstanceMatches("USB\\VID_2CA3&PID_1020&MI_02\\X", "USB\\VID_2CA3&PID_1020&MI_02"));
    CHECK(!SDL_Serial_InstanceMatches("", "USB"));
    CHECK(log_calls == 0);

    /* Too many ports */
    many[0] = '\0';
    used = 0;
    for (i = 1; i <= SDL_SERIAL_MAX_PORTS + 2; ++i) {
        used += (size_t)snprintf(many + used, sizeof(many) - used, "%sCOM%d=probe", (i > 1) ? "," : "", i);
    }
    n = SDL_Serial_ParseHint(many, modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == SDL_SERIAL_MAX_PORTS && log_calls == 2);

    CHECK(SDL_Serial_IsComKey("COM1", &number) && number == 1);
    CHECK(SDL_Serial_IsComKey("cOm9999", &number) && number == 9999);
    CHECK(!SDL_Serial_IsComKey("COM", NULL) && !SDL_Serial_IsComKey("COM1a", NULL) && !SDL_Serial_IsComKey("COM10000", NULL));
    CHECK(SDL_Serial_KeysEqual("COM3", "com3") && !SDL_Serial_KeysEqual("COM3", "COM31"));

    /* Changes between two hints */
    memset(old_entries, 0, sizeof(old_entries));
    memcpy(old_entries[0].key, "COM3", 5);
    old_entries[0].module = &probe_module;
    memcpy(old_entries[1].key, "COM4", 5);
    old_entries[1].module = &probe_module;
    memcpy(old_entries[2].key, "COM5", 5);
    old_entries[2].module = &other_module;
    n = SDL_Serial_ParseHint("com5=other,COM4=other,COM6=probe", modules, 2, entries, SDL_SERIAL_MAX_PORTS, Log, NULL);
    CHECK(n == 3);
    SDL_Serial_DiffPorts(old_entries, 3, entries, n, old_changes, new_changes);
    CHECK(old_changes[0] == SDL_SERIAL_PORT_STOP);
    CHECK(old_changes[1] == SDL_SERIAL_PORT_RESTART);
    CHECK(old_changes[2] == SDL_SERIAL_PORT_KEEP);
    CHECK(new_changes[0] == SDL_SERIAL_PORT_KEEP && new_changes[1] == SDL_SERIAL_PORT_KEEP && new_changes[2] == SDL_SERIAL_PORT_START);
}

static void TestAutoRules(void)
{
    static const SDL_SerialAutoRule rules[3] = {
        { "USB\\VID_2CA3&PID_1020&MI_02", "probe", 0x2CA3, 0x1020 },
        { "USB\\VID_1CCF&PID_804C", "other", 0x1CCF, 0x804C },
        { "USB\\VID_1CCF", "probe", 0x1CCF, 0 }
    };

    CHECK(SDL_Serial_MatchAuto(rules, 3, "USB\\VID_2CA3&PID_1020&MI_02\\7&2F&0&0002") == &rules[0]);
    CHECK(SDL_Serial_MatchAuto(rules, 3, "usb\\vid_2ca3&pid_1020&mi_02\\x") == &rules[0]);
    CHECK(SDL_Serial_MatchAuto(rules, 3, "USB\\VID_2CA3&PID_1020&MI_03\\x") == NULL);
    /* The first matching row wins */
    CHECK(SDL_Serial_MatchAuto(rules, 3, "USB\\VID_1CCF&PID_804C\\1") == &rules[1]);
    CHECK(SDL_Serial_MatchAuto(rules, 3, "USB\\VID_1CCF&PID_8040\\1") == &rules[2]);
    CHECK(SDL_Serial_MatchAuto(rules, 3, "FTDIBUS\\VID_0403") == NULL);
    CHECK(SDL_Serial_MatchAuto(rules, 0, "USB\\VID_2CA3&PID_1020&MI_02") == NULL);
    CHECK(SDL_Serial_MatchAuto(NULL, 3, "USB") == NULL && SDL_Serial_MatchAuto(rules, 3, NULL) == NULL);
}

/* Part 6: ? in a rule matches any one character, as the DJI rule matches
 * any PID on interface 2 */
static void TestAutoWildcard(void)
{
    static const SDL_SerialAutoRule rules[1] = {
        { "USB\\VID_2CA3&PID_????&MI_02\\", "dji", 0x2CA3, 0 }
    };
    uint16_t vendor = 0, product = 0;

    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA3&PID_1020&MI_02\\7&2F&0&0002") == &rules[0]);
    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA3&PID_1030&MI_02\\1") == &rules[0]);
    CHECK(SDL_Serial_MatchAuto(rules, 1, "usb\\vid_2ca3&pid_001f&mi_02\\1") == &rules[0]);
    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA3&PID_1020&MI_04\\1") == NULL);
    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA4&PID_1020&MI_02\\1") == NULL);
    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA3&PID_10200&MI_02\\1") == NULL);
    /* ? needs a character: an ID that ends inside the pattern fails */
    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA3&PID_10") == NULL);
    CHECK(SDL_Serial_MatchAuto(rules, 1, "USB\\VID_2CA3&PID_1020&MI_02") == NULL);
    /* A hint prefix still compares ? as itself */
    CHECK(!SDL_Serial_InstanceMatches("USB\\VID_2CA3&PID_????", "USB\\VID_2CA3&PID_1020"));
    CHECK(SDL_Serial_InstanceMatches("USB\\VID_2CA3&PID_????", "USB\\VID_2CA3&PID_????&MI_02"));

    CHECK(SDL_Serial_ParseUSBIds("USB\\VID_2CA3&PID_1020&MI_02\\7&2F&0&0002", &vendor, &product) && vendor == 0x2CA3 && product == 0x1020);
    CHECK(SDL_Serial_ParseUSBIds("usb\\vid_2ca3&pid_001f\\1", &vendor, &product) && vendor == 0x2CA3 && product == 0x001F);
    CHECK(SDL_Serial_ParseUSBIds("FTDIBUS\\VID_0403+PID_6001+A1B2C3D4A\\0000", &vendor, &product) && vendor == 0x0403 && product == 0x6001);
    vendor = product = 7;
    CHECK(!SDL_Serial_ParseUSBIds("USB\\VID_2CA3&PID_10G0", &vendor, &product) && vendor == 7 && product == 7);
    CHECK(!SDL_Serial_ParseUSBIds("USB\\VID_2CA3&PID_102", &vendor, &product));
    CHECK(!SDL_Serial_ParseUSBIds("USB\\VID_2CA3", &vendor, &product));
    CHECK(!SDL_Serial_ParseUSBIds("USB\\PID_1020", &vendor, &product));
    CHECK(!SDL_Serial_ParseUSBIds(NULL, &vendor, &product));
    CHECK(!SDL_Serial_ParseUSBIds("", &vendor, &product));
}

/* Part 6: a half axis in a gamepad mapping */
static void TestHalfAxis(void)
{
    const SDL_SerialMapInput positive = SDL_Serial_MapHalfAxis(4, true);
    const SDL_SerialMapInput negative = SDL_Serial_MapHalfAxis(5, false);

    CHECK(positive.kind == SDL_SERIAL_MAP_AXIS_POSITIVE && positive.target == 4);
    CHECK(negative.kind == SDL_SERIAL_MAP_AXIS_NEGATIVE && negative.target == 5);
}

static void TestScaling(void)
{
    CHECK(SDL_Serial_ScaleSigned(0, -64, 63) == 0);
    CHECK(SDL_Serial_ScaleSigned(-64, -64, 63) == -32768);
    CHECK(SDL_Serial_ScaleSigned(63, -64, 63) == 32767);
    CHECK(SDL_Serial_ScaleSigned(-100, -64, 63) == -32768);
    CHECK(SDL_Serial_ScaleSigned(100, -64, 63) == 32767);
    CHECK(SDL_Serial_ScaleSigned(-32, -255, 256) == -4112); /* -32 x 32768 / 255 = -4112.06, toward zero */
    CHECK(SDL_Serial_ScaleSigned(32, -255, 256) == 4095);   /* 32 x 32767 / 256 = 4095.9 */
    CHECK(SDL_Serial_ScaleSigned(-1, -2, 1) == -16384);
    CHECK(SDL_Serial_ScaleSigned(-1, -128, 127) == -256);
    CHECK(SDL_Serial_ScaleUnsigned(50, 50, 200) == -32768);
    CHECK(SDL_Serial_ScaleUnsigned(200, 50, 200) == 32767);
    CHECK(SDL_Serial_ScaleUnsigned(125, 50, 200) == 0);
    CHECK(SDL_Serial_ScaleUnsigned(0, 50, 200) == -32768);
    CHECK(SDL_Serial_ScaleUnsigned(1500, 1000, 2000) == 0);
    CHECK(SDL_Serial_ScaleUnsigned(1000, 1000, 2000) == -32768);
    CHECK(SDL_Serial_ScaleUnsigned(2000, 1000, 2000) == 32767);
    CHECK(SDL_Serial_ScaleUnsigned(4095, 1000, 2000) == 32767);
}

/* Part 14: a line break, as the ACIO reset holds one */
static bool escape_fails;

static bool Failing_Escape(void *userdata, uint32_t function)
{
    (void)H_Escape(userdata, function);
    return !escape_fails;
}

static const SDL_SerialPortOps failing_escape_ops = {
    H_Open, H_SetLine, H_SetTimeouts, H_Purge, Failing_Escape, H_Write, H_Drain, H_Close, H_PresenceChanged, H_Publish, H_Log
};

static void TestBreak(void)
{
    Harness *h = H_Create(&probe_module);
    ProbeState *s;
    SDL_SerialAction action;
    uint8_t tag = 0;
    int i;

    CHECK(SDL_SERIAL_SETBREAK == 8 && SDL_SERIAL_CLRBREAK == 9);
    CHECK(SDL_SERIAL_TYPE_DANCE_PAD == 5 && SDL_SERIAL_TYPE_DRUM_KIT == 7);
    H_Start(h);
    H_SkipCalls(h);
    H_Advance(h, 10);
    FeedText(h, "K");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 10) && H_NextCall(h) == NULL);
    CHECK(Probe(h)->dones == 3 && Probe(h)->failed_dones == 0 && Probe(h)->last_tag == 9 && h->engine.breaking);
    FeedText(h, "k");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 10) && !h->engine.breaking);
    CHECK(Probe(h)->dones == 4 && Probe(h)->last_tag == 10);
    /* A loss during a break ends the break, then closes */
    FeedText(h, "K");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 10));
    H_Advance(h, 20);
    H_LosePort(h);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 20) && H_IsCall(H_NextCall(h), 'C', 20));
    CHECK(H_NextCall(h) == NULL && !h->engine.breaking);
    /* The port returns out of the break, and a loss then closes only */
    H_Advance(h, 1020);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 1020));
    CHECK(H_IsWriteText(H_NextCall(h), "HI", 1020));
    H_LosePort(h);
    CHECK(H_IsCall(H_NextCall(h), 'C', 1020) && H_NextCall(h) == NULL);
    /* Stop after a break has ended closes only */
    H_Advance(h, 2020);
    H_SkipCalls(h);
    FeedText(h, "Kk");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 2020) && H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 2020));
    SDL_SerialEngine_Stop(&h->engine);
    CHECK(H_IsCall(H_NextCall(h), 'C', 2020) && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* Stop during a break ends it first */
    h = H_Create(&probe_module);
    H_Start(h);
    H_SkipCalls(h);
    FeedText(h, "K");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 0));
    SDL_SerialEngine_Stop(&h->engine);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 0) && H_IsCall(H_NextCall(h), 'C', 0));
    H_Destroy(h);

    /* Flow control does not hold a break back, and a break waits for a
       pending write */
    h = H_Create(&probe_module);
    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0) && H_IsWriteText(H_NextCall(h), "HI", 0));
    FeedText(h, "K");
    CHECK(H_NextCall(h) == NULL);
    H_CompleteWrite(h, true);
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 0) && Probe(h)->last_tag == 9);
    FeedText(h, "Rk");
    CHECK(H_IsLine(H_NextCall(h), 9600, 8, SDL_SERIAL_NOPARITY, 2, SDL_SERIAL_RTS_CONTROL_HANDSHAKE, true, SDL_SERIAL_DTR_CONTROL_ENABLE, 0));
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 0) && Probe(h)->failed_dones == 0 && Probe(h)->last_tag == 10);
    H_Destroy(h);

    /* A break that fails is port loss */
    h = H_Create(&probe_module);
    SDL_SerialEngine_Init(&h->engine, &failing_escape_ops, h, h->module, h->state, 0);
    H_Start(h);
    H_SkipCalls(h);
    escape_fails = true;
    FeedText(h, "K");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 0) && H_IsCall(H_NextCall(h), 'C', 0) && H_NextCall(h) == NULL);
    CHECK(h->engine.losses == 1 && !h->engine.breaking && Probe(h)->dones == 2);
    /* An end of break that fails is port loss, and the close tries once more */
    escape_fails = false;
    H_Advance(h, 1000);
    H_SkipCalls(h);
    FeedText(h, "K");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_SETBREAK, 1000) && h->engine.breaking);
    escape_fails = true;
    FeedText(h, "k");
    CHECK(H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 1000) && H_IsEscape(H_NextCall(h), SDL_SERIAL_CLRBREAK, 1000));
    CHECK(H_IsCall(H_NextCall(h), 'C', 1000) && h->engine.losses == 2 && !h->engine.breaking);
    escape_fails = false;
    H_Destroy(h);

    /* The queue helper */
    s = (ProbeState *)calloc(1, sizeof(ProbeState));
    SDL_Serial_ResetBase(&s->base, NULL);
    CHECK(SDL_Serial_QueueBreak(&s->base, 3, true) && SDL_Serial_QueueBreak(&s->base, 4, false));
    CHECK(SDL_Serial_NextAction(s, &action) && action.kind == SDL_SERIAL_ACTION_BREAK && action.on && action.tag == 3);
    CHECK(SDL_Serial_FinishAction(&s->base, &tag) && tag == 3);
    CHECK(SDL_Serial_NextAction(s, &action) && action.kind == SDL_SERIAL_ACTION_BREAK && !action.on && action.tag == 4);
    for (i = 0; i < SDL_SERIAL_MAX_ACTIONS; ++i) {
        CHECK(SDL_Serial_QueueBreak(&s->base, 5, true));
    }
    CHECK(!SDL_Serial_QueueBreak(&s->base, 6, true));
    free(s);
}

/* Part 14: a close sequence keeps the port open until the module ends it,
   as the PANB's reset before close does */
static void TestCloseSequence(void)
{
    Harness *h;
    uint64_t deadline;
    int dones;

    /* No sequence: the port closes at once and never opens again */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "P");
    H_SkipCalls(h);
    H_Advance(h, 10);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(10)));
    CHECK(H_IsCall(H_NextCall(h), 'C', 10) && H_NextCall(h) == NULL);
    CHECK(!h->engine.open && !SDL_SerialEngine_IsStopping(&h->engine) && h->presence[0] == 0 && !Probe(h)->base.closing);
    CHECK(!SDL_SerialEngine_GetDeadline(&h->engine, &deadline));
    H_Advance(h, 5000);
    SDL_SerialEngine_Rescan(&h->engine, H_NS(5000));
    SDL_SerialEngine_Run(&h->engine, H_NS(6000));
    CHECK(H_NextCall(h) == NULL && h->engine.opens == 1);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(6000)) && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* A sequence: the module queues its bytes, reads go on, and the port
       closes when the module says so */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "PC");
    H_SkipCalls(h);
    H_Advance(h, 10);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(10)));
    CHECK(Probe(h)->base.closing && !Probe(h)->base.closed && SDL_SerialEngine_IsStopping(&h->engine));
    CHECK(H_IsWriteText(H_NextCall(h), "BYE", 10) && H_NextCall(h) == NULL && Probe(h)->last_tag == 11);
    CHECK(h->engine.open && h->presence[0] == 1);
    CHECK(SDL_SerialEngine_GetDeadline(&h->engine, &deadline) && deadline == 110);
    /* A second call changes nothing */
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(10)) && H_NextCall(h) == NULL);
    H_Advance(h, 50);
    FeedText(h, "5");
    CHECK(H_Last(h)->controls.axes[0] == 5);
    /* Output waits for nothing and goes nowhere, and a rescan opens nothing */
    {
        SDL_SerialOutput request;
        const int outputs = Probe(h)->outputs;

        memset(&request, 0, sizeof(request));
        request.kind = SDL_SERIAL_OUTPUT_RUMBLE;
        SDL_SerialEngine_Output(&h->engine, &request, H_NS(50));
        CHECK(Probe(h)->outputs == outputs);
    }
    SDL_SerialEngine_Rescan(&h->engine, H_NS(50));
    H_Advance(h, 109);
    CHECK(H_NextCall(h) == NULL && h->engine.open);
    H_Advance(h, 110);
    CHECK(H_IsCall(H_NextCall(h), 'C', 110) && H_NextCall(h) == NULL);
    CHECK(!h->engine.open && !SDL_SerialEngine_IsStopping(&h->engine) && h->presence[0] == 0);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(110)));
    H_Advance(h, 10000);
    SDL_SerialEngine_Rescan(&h->engine, H_NS(10000));
    CHECK(H_NextCall(h) == NULL && h->engine.opens == 1 && !SDL_SerialEngine_GetDeadline(&h->engine, &deadline));
    H_Destroy(h);

    /* A sequence that never ends has SDL_SERIAL_CLOSE_MS from the call, which
       brings its own clock */
    CHECK(SDL_SERIAL_CLOSE_MS == 3000);
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "H");
    H_SkipCalls(h);
    H_Advance(h, 5);
    h->now = 10;
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(10)));
    CHECK(H_IsWriteText(H_NextCall(h), "BYE", 10));
    CHECK(SDL_SerialEngine_GetDeadline(&h->engine, &deadline) && deadline == 10 + SDL_SERIAL_CLOSE_MS);
    /* A later call does not move the end */
    H_Advance(h, 1000);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(1000)) && H_NextCall(h) == NULL);
    CHECK(SDL_SerialEngine_GetDeadline(&h->engine, &deadline) && deadline == 10 + SDL_SERIAL_CLOSE_MS);
    H_Advance(h, 10 + SDL_SERIAL_CLOSE_MS - 1);
    CHECK(H_NextCall(h) == NULL && SDL_SerialEngine_IsStopping(&h->engine));
    H_Advance(h, 10 + SDL_SERIAL_CLOSE_MS);
    CHECK(H_IsCall(H_NextCall(h), 'C', 10 + SDL_SERIAL_CLOSE_MS) && !SDL_SerialEngine_IsStopping(&h->engine));
    H_Destroy(h);

    /* A write still out: the sequence's bytes follow it, and its completion
       is not the sequence's */
    h = H_Create(&probe_module);
    h->pend_writes = true;
    H_Start(h);
    CHECK(H_ExpectOpened(h, 9600, 8, SDL_SERIAL_NOPARITY, 1, 0) && H_IsWriteText(H_NextCall(h), "HI", 0));
    FeedText(h, "C");
    dones = Probe(h)->dones;
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(0)) && H_NextCall(h) == NULL);
    H_Advance(h, 5);
    H_CompleteWrite(h, true);
    CHECK(H_IsWriteText(H_NextCall(h), "BYE", 5) && Probe(h)->dones == dones);
    H_Advance(h, 6);
    H_CompleteWrite(h, true);
    CHECK(Probe(h)->dones == dones + 1 && Probe(h)->last_tag == 11 && h->engine.open);
    H_Advance(h, 100);
    CHECK(H_IsCall(H_NextCall(h), 'C', 100));
    H_Destroy(h);

    /* A loss during the sequence closes the port for good */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "C");
    H_SkipCalls(h);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(0)));
    H_Advance(h, 50);
    H_LosePort(h);
    CHECK(H_IsWriteText(H_NextCall(h), "BYE", 0) && H_IsCall(H_NextCall(h), 'C', 50));
    CHECK(!SDL_SerialEngine_IsStopping(&h->engine) && h->engine.losses == 1);
    H_Advance(h, 5000);
    CHECK(H_NextCall(h) == NULL && h->engine.opens == 1);
    H_Destroy(h);

    /* A module that no longer asks for a sequence closes at once */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "Cc");
    H_SkipCalls(h);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(0)) && H_IsCall(H_NextCall(h), 'C', 0));
    H_Destroy(h);

    /* Closed without a close running changes nothing */
    h = H_Create(&probe_module);
    H_Start(h);
    H_SkipCalls(h);
    FeedText(h, "zW");
    H_Advance(h, 500);
    CHECK(H_IsWriteText(H_NextCall(h), "W", 0) && h->engine.open && !SDL_SerialEngine_IsStopping(&h->engine));
    H_Destroy(h);

    /* A sequence that ends with an action still queued closes before it */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "L");
    H_SkipCalls(h);
    CHECK(SDL_SerialEngine_BeginStop(&h->engine, H_NS(0)) && H_IsWriteText(H_NextCall(h), "BYE", 0));
    H_Advance(h, 100);
    CHECK(H_IsCall(H_NextCall(h), 'C', 100) && H_NextCall(h) == NULL);
    H_Destroy(h);

    /* A sequence that ends on the pump's last step closes the port there */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "U");
    H_SkipCalls(h);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(0)) && !h->engine.open);
    {
        int i, writes = 0;

        for (i = h->cursor; i < h->ncalls; ++i) {
            if (H_IsWriteText(&h->calls[i], (i == h->cursor) ? "BYE" : "U", 0)) {
                ++writes;
            }
        }
        CHECK(writes == 256 && h->calls[h->ncalls - 1].kind == 'C' && Probe(h)->runaway_dones == 256);
    }
    H_Destroy(h);

    /* A port lost after its module asked for a sequence runs none */
    h = H_Create(&probe_module);
    H_Start(h);
    FeedText(h, "C");
    H_LosePort(h);
    H_SkipCalls(h);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(0)) && H_NextCall(h) == NULL);
    CHECK(!Probe(h)->base.closing && !Probe(h)->close_started);
    H_Advance(h, 5000);
    CHECK(H_NextCall(h) == NULL && h->engine.opens == 1);
    H_Destroy(h);

    /* A port still retrying its open never opens */
    h = H_Create(&probe_module);
    h->open_failures = 1;
    H_Start(h);
    CHECK(H_IsCall(H_NextCall(h), 'O', 0) && H_NextCall(h) == NULL);
    CHECK(!SDL_SerialEngine_BeginStop(&h->engine, H_NS(10)) && !Probe(h)->base.closing);
    H_Advance(h, 5000);
    SDL_SerialEngine_Rescan(&h->engine, H_NS(5000));
    CHECK(H_NextCall(h) == NULL && h->engine.opens == 0);
    H_Destroy(h);
}

static void TestBase(void)
{
    ProbeState *s = (ProbeState *)calloc(1, sizeof(ProbeState));
    SDL_SerialAction action;
    SDL_SerialIdentity identity;
    uint8_t data[SDL_SERIAL_MAX_WRITE + 1];
    uint8_t tag = 0;
    int i;

    memset(data, 0x5A, sizeof(data));
    SDL_Serial_ResetBase(&s->base, NULL);
    /* The queue holds SDL_SERIAL_MAX_ACTIONS actions in order */
    for (i = 0; i < SDL_SERIAL_MAX_ACTIONS; ++i) {
        CHECK(SDL_Serial_QueueWrite(&s->base, (uint8_t)i, data, 1));
    }
    CHECK(!SDL_Serial_QueueWrite(&s->base, 99, data, 1));
    CHECK(!SDL_Serial_QueueDrain(&s->base, 99));
    for (i = 0; i < SDL_SERIAL_MAX_ACTIONS; ++i) {
        CHECK(SDL_Serial_NextAction(s, &action) && action.tag == i && action.kind == SDL_SERIAL_ACTION_WRITE);
        CHECK(SDL_Serial_FinishAction(&s->base, &tag) && tag == i);
    }
    CHECK(!SDL_Serial_NextAction(s, &action));
    CHECK(!SDL_Serial_FinishAction(&s->base, &tag));
    /* Too long or empty writes are refused */
    CHECK(!SDL_Serial_QueueWrite(&s->base, 1, data, SDL_SERIAL_MAX_WRITE + 1));
    CHECK(!SDL_Serial_QueueWrite(&s->base, 1, data, 0));
    CHECK(SDL_Serial_QueueWrite(&s->base, 1, data, SDL_SERIAL_MAX_WRITE));
    /* The done of an action taken before a clear is ignored */
    CHECK(SDL_Serial_NextAction(s, &action));
    SDL_Serial_ClearActions(&s->base);
    CHECK(!SDL_Serial_FinishAction(&s->base, &tag));
    /* As after a reset */
    CHECK(SDL_Serial_QueueDrain(&s->base, 7) && SDL_Serial_NextAction(s, &action));
    SDL_Serial_ResetBase(&s->base, NULL);
    CHECK(!SDL_Serial_FinishAction(&s->base, &tag));

    /* Identity limits and names */
    SDL_Serial_SetIdentity(&identity, "A name longer than the sixty-three characters an identity holds in all", 1, 40, 300, 5, 3);
    CHECK(strlen(identity.name) == SDL_SERIAL_NAME_LENGTH - 1);
    CHECK(identity.naxes == SDL_SERIAL_MAX_AXES && identity.nbuttons == SDL_SERIAL_MAX_BUTTONS);
    CHECK(identity.nhats == SDL_SERIAL_MAX_HATS && identity.nballs == SDL_SERIAL_MAX_BALLS);

    /* Controls of an absent sub-device are not kept */
    {
        SDL_SerialControls controls;
        memset(&controls, 0, sizeof(controls));
        controls.axes[0] = 9;
        SDL_Serial_Commit(&s->base, 0, &controls);
        CHECK(s->base.snapshots[0].controls.axes[0] == 0 && s->base.snapshots[0].changes == 0);
        SDL_Serial_Pulse(&s->base, 0, 0, 0);
        CHECK(s->base.pulse_end[0][0] == 0);
    }
    free(s);
}

int main(void)
{
    TestRetry();
    TestLineConfig();
    TestActions();
    TestPresence();
    TestPulseAndOutput();
    TestQueue();
    TestHint();
    TestAutoRules();
    TestAutoWildcard();
    TestHalfAxis();
    TestScaling();
    TestBase();
    TestBreak();
    TestCloseSequence();
    return H_Finish();
}
