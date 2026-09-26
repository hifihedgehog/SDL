/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "SDL_serial_engine.h"

#include <string.h>

/* A module that keeps a deadline in the past, or keeps queuing actions that
 * finish at once, cannot hold the port thread longer than this many steps */
#define SERIAL_MAX_STEPS 256

#define SERIAL_NS_PER_MS 1000000

void SDL_Serial_BuildLineConfig(const SDL_SerialLine *line, SDL_SerialLineConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->BaudRate = line->rate;
    config->ByteSize = line->data_bits;
    switch (line->parity) {
    case SDL_SERIAL_PARITY_ODD:
        config->Parity = SDL_SERIAL_ODDPARITY;
        config->fParity = true;
        break;
    case SDL_SERIAL_PARITY_EVEN:
        config->Parity = SDL_SERIAL_EVENPARITY;
        config->fParity = true;
        break;
    default:
        config->Parity = SDL_SERIAL_NOPARITY;
        config->fParity = false;
        break;
    }
    config->StopBits = (line->stop_bits == 2) ? SDL_SERIAL_TWOSTOPBITS : SDL_SERIAL_ONESTOPBIT;
    /* Binary mode is the only one Windows supports. fNull would discard the
     * 00 bytes several protocols carry, and fAbortOnError would stop all I/O
     * after an error until ClearCommError. */
    config->fBinary = true;
    config->fNull = false;
    config->fErrorChar = false;
    config->fAbortOnError = false;
    config->fOutX = false;
    config->fInX = false;
    config->fTXContinueOnXoff = false;
    config->fOutxDsrFlow = false;
    config->fDsrSensitivity = false;
    config->fDtrControl = line->dtr ? SDL_SERIAL_DTR_CONTROL_ENABLE : SDL_SERIAL_DTR_CONTROL_DISABLE;
    switch (line->flow) {
    case SDL_SERIAL_FLOW_RTSCTS:
        config->fOutxCtsFlow = true;
        config->fRtsControl = SDL_SERIAL_RTS_CONTROL_HANDSHAKE;
        break;
    case SDL_SERIAL_FLOW_RTS_TOGGLE:
        config->fOutxCtsFlow = false;
        config->fRtsControl = SDL_SERIAL_RTS_CONTROL_TOGGLE;
        break;
    default:
        config->fOutxCtsFlow = false;
        config->fRtsControl = line->rts ? SDL_SERIAL_RTS_CONTROL_ENABLE : SDL_SERIAL_RTS_CONTROL_DISABLE;
        break;
    }
}

void SDL_Serial_GetTimeouts(SDL_SerialTimeouts *timeouts)
{
    timeouts->ReadIntervalTimeout = SDL_SERIAL_MAXDWORD;
    timeouts->ReadTotalTimeoutMultiplier = SDL_SERIAL_MAXDWORD;
    timeouts->ReadTotalTimeoutConstant = SDL_SERIAL_READ_TOTAL_CONSTANT_MS;
    timeouts->WriteTotalTimeoutMultiplier = SDL_SERIAL_WRITE_TOTAL_MULTIPLIER;
    timeouts->WriteTotalTimeoutConstant = SDL_SERIAL_WRITE_TOTAL_CONSTANT_MS;
}

void SDL_Serial_ClearQueue(SDL_SerialSnapshotQueue *queue)
{
    queue->head = 0;
    queue->count = 0;
    queue->dropped = 0;
}

void SDL_Serial_PushQueue(SDL_SerialSnapshotQueue *queue, const SDL_SerialQueueEntry *entry)
{
    if (queue->count == SDL_SERIAL_QUEUE_ENTRIES) {
        queue->head = (queue->head + 1) % SDL_SERIAL_QUEUE_ENTRIES;
        --queue->count;
        ++queue->dropped;
    }
    queue->entries[(queue->head + queue->count) % SDL_SERIAL_QUEUE_ENTRIES] = *entry;
    ++queue->count;
}

bool SDL_Serial_PopQueue(SDL_SerialSnapshotQueue *queue, SDL_SerialQueueEntry *entry)
{
    if (queue->count == 0) {
        return false;
    }
    *entry = queue->entries[queue->head];
    queue->head = (queue->head + 1) % SDL_SERIAL_QUEUE_ENTRIES;
    --queue->count;
    return true;
}

static void Engine_SetClock(SDL_SerialEngine *engine, uint64_t now_ns)
{
    engine->stamp = now_ns;
    engine->now = now_ns / SERIAL_NS_PER_MS;
}

static void Engine_Changed(void *userdata, int sub, const SDL_SerialSnapshot *snapshot)
{
    SDL_SerialEngine *engine = (SDL_SerialEngine *)userdata;
    SDL_SerialQueueEntry entry;

    if (sub < 0 || sub >= SDL_SERIAL_MAX_SUBDEVICES) {
        return;
    }
    if (!snapshot->present) {
        if (engine->present[sub]) {
            engine->present[sub] = false;
            engine->ops->Presence(engine->userdata, sub, 0, NULL);
        }
        return;
    }
    if (!engine->present[sub]) {
        engine->present[sub] = true;
        if (++engine->generation[sub] == 0) {
            engine->generation[sub] = 1;
        }
        engine->ops->Presence(engine->userdata, sub, engine->generation[sub], &snapshot->identity);
    }
    memset(&entry, 0, sizeof(entry));
    entry.sub = sub;
    entry.generation = engine->generation[sub];
    entry.stamp_ns = engine->stamp;
    entry.controls = snapshot->controls;
    engine->ops->Publish(engine->userdata, &entry);
}

static void Engine_Log(void *userdata, const char *text)
{
    SDL_SerialEngine *engine = (SDL_SerialEngine *)userdata;

    if (engine->ops->Log) {
        engine->ops->Log(engine->userdata, text);
    }
}

static void Engine_ClearPresence(SDL_SerialEngine *engine)
{
    int sub;

    for (sub = 0; sub < SDL_SERIAL_MAX_SUBDEVICES; ++sub) {
        if (engine->present[sub]) {
            engine->present[sub] = false;
            engine->ops->Presence(engine->userdata, sub, 0, NULL);
        }
    }
}

static void Engine_Close(SDL_SerialEngine *engine)
{
    if (engine->open) {
        /* Nothing documents that closing a USB serial port ends a break, so
           a break the engine holds ends first */
        if (engine->breaking) {
            (void)engine->ops->Escape(engine->userdata, SDL_SERIAL_CLRBREAK);
        }
        engine->ops->Close(engine->userdata);
        engine->open = false;
    }
    engine->configured = false;
    engine->busy = false;
    engine->breaking = false;
    Engine_ClearPresence(engine);
}

static void Engine_Lose(SDL_SerialEngine *engine)
{
    Engine_Close(engine);
    ++engine->losses;
    engine->retry_at = engine->now + SDL_SERIAL_RETRY_MS;
}

static void Engine_Done(SDL_SerialEngine *engine, bool success)
{
    engine->module->ActionDone(engine->state, success, engine->now);
}

static void Engine_SetLine(SDL_SerialEngine *engine, const SDL_SerialLine *line)
{
    SDL_SerialLineConfig config;
    SDL_SerialLine applied = *line;

    SDL_Serial_BuildLineConfig(&applied, &config);
    if (!engine->ops->SetLine(engine->userdata, &config)) {
        if (applied.flow != SDL_SERIAL_FLOW_RTS_TOGGLE) {
            Engine_Lose(engine);
            return;
        }
        /* A port that rejects RTS toggle mode runs with RTS off. An adapter
         * with automatic direction control needs neither. */
        applied.flow = SDL_SERIAL_FLOW_NONE;
        applied.rts = false;
        SDL_Serial_BuildLineConfig(&applied, &config);
        if (!engine->ops->SetLine(engine->userdata, &config)) {
            Engine_Lose(engine);
            return;
        }
    }
    if (!engine->configured) {
        SDL_SerialTimeouts timeouts;

        SDL_Serial_GetTimeouts(&timeouts);
        if (!engine->ops->SetTimeouts(engine->userdata, &timeouts) ||
            !engine->ops->Purge(engine->userdata, SDL_SERIAL_PURGE_ALL)) {
            Engine_Lose(engine);
            return;
        }
        engine->configured = true;
    }
    engine->line = applied;
    Engine_Done(engine, true);
}

static void Engine_Execute(SDL_SerialEngine *engine, const SDL_SerialAction *action)
{
    switch (action->kind) {
    case SDL_SERIAL_ACTION_SET_LINE:
        Engine_SetLine(engine, &action->line);
        break;
    case SDL_SERIAL_ACTION_SET_MODEM:
        /* EscapeCommFunction may not move RTS while it is in handshake or
         * toggle mode */
        if (engine->line.flow != SDL_SERIAL_FLOW_NONE) {
            Engine_Done(engine, false);
            break;
        }
        if (!engine->ops->Escape(engine->userdata, action->line.dtr ? SDL_SERIAL_SETDTR : SDL_SERIAL_CLRDTR) ||
            !engine->ops->Escape(engine->userdata, action->line.rts ? SDL_SERIAL_SETRTS : SDL_SERIAL_CLRRTS)) {
            Engine_Lose(engine);
            break;
        }
        engine->line.dtr = action->line.dtr;
        engine->line.rts = action->line.rts;
        Engine_Done(engine, true);
        break;
    case SDL_SERIAL_ACTION_WRITE:
        switch (engine->ops->Write(engine->userdata, action->data, action->length)) {
        case SDL_SERIAL_IO_DONE:
            Engine_Done(engine, true);
            break;
        case SDL_SERIAL_IO_PENDING:
            engine->busy = true;
            break;
        default:
            Engine_Lose(engine);
            break;
        }
        break;
    case SDL_SERIAL_ACTION_DRAIN:
        /* A drain waits for the bytes to leave, which flow control can hold
         * back without limit */
        if (engine->line.flow == SDL_SERIAL_FLOW_RTSCTS) {
            Engine_Done(engine, false);
            break;
        }
        if (!engine->ops->Drain(engine->userdata)) {
            Engine_Lose(engine);
            break;
        }
        Engine_Done(engine, true);
        break;
    case SDL_SERIAL_ACTION_BREAK:
        if (!engine->ops->Escape(engine->userdata, action->on ? SDL_SERIAL_SETBREAK : SDL_SERIAL_CLRBREAK)) {
            Engine_Lose(engine);
            break;
        }
        engine->breaking = action->on;
        Engine_Done(engine, true);
        break;
    default:
        Engine_Done(engine, false);
        break;
    }
}

/* True once a close sequence is done and the port has closed */
static bool Engine_CloseIfDone(SDL_SerialEngine *engine)
{
    if (engine->stopping && ((const SDL_SerialBase *)engine->state)->closed) {
        Engine_Close(engine);
        return true;
    }
    return false;
}

static void Engine_Pump(SDL_SerialEngine *engine)
{
    int step;

    for (step = 0; step < SERIAL_MAX_STEPS && engine->open; ++step) {
        SDL_SerialAction action;
        uint64_t deadline;

        if (Engine_CloseIfDone(engine)) {
            return;
        }
        if (!engine->busy && engine->module->NextAction(engine->state, &action)) {
            Engine_Execute(engine, &action);
            continue;
        }
        if (engine->configured && engine->module->GetDeadline(engine->state, &deadline) && deadline <= engine->now) {
            engine->module->Tick(engine->state, engine->now);
            continue;
        }
        break;
    }
    (void)Engine_CloseIfDone(engine);
}

static void Engine_TryOpen(SDL_SerialEngine *engine)
{
    SDL_SerialSink sink;

    if (!engine->ops->Open(engine->userdata)) {
        engine->retry_at = engine->now + SDL_SERIAL_RETRY_MS;
        return;
    }
    engine->open = true;
    engine->configured = false;
    engine->busy = false;
    memset(&engine->line, 0, sizeof(engine->line));
    ++engine->opens;
    sink.userdata = engine;
    sink.changed = Engine_Changed;
    sink.log = Engine_Log;
    engine->module->Reset(engine->state, &sink, engine->now);
}

void SDL_SerialEngine_Init(SDL_SerialEngine *engine, const SDL_SerialPortOps *ops, void *userdata, const SDL_SerialModule *module, void *state, uint64_t now_ns)
{
    memset(engine, 0, sizeof(*engine));
    engine->ops = ops;
    engine->userdata = userdata;
    engine->module = module;
    engine->state = state;
    Engine_SetClock(engine, now_ns);
    engine->retry_at = engine->now;
}

void SDL_SerialEngine_Run(SDL_SerialEngine *engine, uint64_t now_ns)
{
    Engine_SetClock(engine, now_ns);
    if (!engine->open) {
        if (engine->stopping || engine->now < engine->retry_at) {
            return;
        }
        Engine_TryOpen(engine);
    }
    if (engine->stopping && engine->now >= engine->stop_at) {
        /* The close sequence ran out of time */
        Engine_Close(engine);
        return;
    }
    Engine_Pump(engine);
}

void SDL_SerialEngine_Received(SDL_SerialEngine *engine, const uint8_t *data, size_t length, uint64_t now_ns)
{
    Engine_SetClock(engine, now_ns);
    if (!SDL_SerialEngine_IsReading(engine) || !data || length == 0) {
        return;
    }
    engine->module->Feed(engine->state, data, length, engine->now);
    Engine_Pump(engine);
}

void SDL_SerialEngine_WriteDone(SDL_SerialEngine *engine, bool success, uint64_t now_ns)
{
    Engine_SetClock(engine, now_ns);
    if (!engine->open || !engine->busy) {
        return;
    }
    engine->busy = false;
    Engine_Done(engine, success);
    Engine_Pump(engine);
}

void SDL_SerialEngine_Lost(SDL_SerialEngine *engine, uint64_t now_ns)
{
    Engine_SetClock(engine, now_ns);
    if (engine->open) {
        Engine_Lose(engine);
    }
}

void SDL_SerialEngine_Rescan(SDL_SerialEngine *engine, uint64_t now_ns)
{
    Engine_SetClock(engine, now_ns);
    if (!engine->open && !engine->stopping) {
        engine->retry_at = engine->now;
        Engine_TryOpen(engine);
        Engine_Pump(engine);
    }
}

void SDL_SerialEngine_Output(SDL_SerialEngine *engine, const SDL_SerialOutput *request, uint64_t now_ns)
{
    Engine_SetClock(engine, now_ns);
    if (engine->stopping || !SDL_SerialEngine_IsReading(engine) || !request) {
        return;
    }
    engine->module->Output(engine->state, request, engine->now);
    Engine_Pump(engine);
}

bool SDL_SerialEngine_GetDeadline(const SDL_SerialEngine *engine, uint64_t *deadline)
{
    bool have;

    if (!engine->open) {
        if (engine->stopping) {
            return false;
        }
        *deadline = engine->retry_at;
        return true;
    }
    if (!engine->configured) {
        return false;
    }
    have = engine->module->GetDeadline(engine->state, deadline);
    if (engine->stopping) {
        SDL_Serial_EarlierDeadline(&have, deadline, engine->stop_at);
    }
    return have;
}

bool SDL_SerialEngine_IsReading(const SDL_SerialEngine *engine)
{
    return engine->open && engine->configured;
}

void SDL_SerialEngine_Stop(SDL_SerialEngine *engine)
{
    Engine_Close(engine);
}

bool SDL_SerialEngine_BeginStop(SDL_SerialEngine *engine, uint64_t now_ns)
{
    SDL_SerialBase *base = (SDL_SerialBase *)engine->state;

    Engine_SetClock(engine, now_ns);
    if (engine->stopping) {
        return SDL_SerialEngine_IsStopping(engine);
    }
    engine->stopping = true;
    if (!SDL_SerialEngine_IsReading(engine) || !base->close_sequence) {
        Engine_Close(engine);
        return false;
    }
    engine->stop_at = engine->now + SDL_SERIAL_CLOSE_MS;
    base->closing = true;
    engine->module->Tick(engine->state, engine->now);
    Engine_Pump(engine);
    return SDL_SerialEngine_IsStopping(engine);
}

bool SDL_SerialEngine_IsStopping(const SDL_SerialEngine *engine)
{
    return engine->stopping && engine->open;
}

static char Serial_Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool SDL_Serial_KeysEqual(const char *a, const char *b)
{
    while (*a && *b) {
        if (Serial_Lower(*a) != Serial_Lower(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

bool SDL_Serial_InstanceMatches(const char *prefix, const char *instance_id)
{
    if (!prefix || !instance_id || !*prefix) {
        return false;
    }
    while (*prefix) {
        if (!*instance_id || Serial_Lower(*prefix) != Serial_Lower(*instance_id)) {
            return false;
        }
        ++prefix;
        ++instance_id;
    }
    return true;
}

bool SDL_Serial_IsComKey(const char *key, unsigned int *number)
{
    unsigned int value = 0;
    int digits = 0;

    if (Serial_Lower(key[0]) != 'c' || Serial_Lower(key[1]) != 'o' || Serial_Lower(key[2]) != 'm') {
        return false;
    }
    key += 3;
    if (*key == '0') {
        return false;
    }
    while (*key >= '0' && *key <= '9') {
        if (++digits > 4) {
            return false;
        }
        value = value * 10 + (unsigned int)(*key - '0');
        ++key;
    }
    if (*key || digits == 0) {
        return false;
    }
    if (number) {
        *number = value;
    }
    return true;
}

static bool Serial_IsSpace(char c)
{
    return c == ' ' || c == '\t';
}

static void Serial_Trim(const char **start, const char **end)
{
    while (*start < *end && Serial_IsSpace(**start)) {
        ++*start;
    }
    while (*end > *start && Serial_IsSpace((*end)[-1])) {
        --*end;
    }
}

static const SDL_SerialModule *Serial_FindModule(const SDL_SerialModule *const *modules, int nmodules, const char *token, size_t length)
{
    int i;

    for (i = 0; i < nmodules; ++i) {
        const char *name = modules[i]->token;
        size_t j;

        for (j = 0; j < length && name[j]; ++j) {
            if (Serial_Lower(token[j]) != Serial_Lower(name[j])) {
                break;
            }
        }
        if (j == length && name[j] == '\0') {
            return modules[i];
        }
    }
    return NULL;
}

static bool Serial_ValidKey(const char *key)
{
    const char *p;

    if (SDL_Serial_IsComKey(key, NULL)) {
        return true;
    }
    if (!strchr(key, '\\')) {
        return false;
    }
    for (p = key; *p; ++p) {
        if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7E) {
            return false;
        }
    }
    return true;
}

int SDL_Serial_ParseHint(const char *hint, const SDL_SerialModule *const *modules, int nmodules, SDL_SerialPortEntry *entries, int max_entries, SDL_SerialLogFunc log, void *userdata)
{
    int count = 0;
    const char *p = hint;

    if (!hint) {
        return 0;
    }
    while (*p) {
        const char *entry_start = p;
        const char *entry_end = strchr(p, ',');
        const char *equals, *port_start, *port_end, *proto_start, *proto_end;
        const SDL_SerialModule *module;
        char key[SDL_SERIAL_KEY_LENGTH];
        size_t key_length;
        int i;

        if (!entry_end) {
            entry_end = p + strlen(p);
        }
        p = (*entry_end == ',') ? entry_end + 1 : entry_end;

        Serial_Trim(&entry_start, &entry_end);
        if (entry_start == entry_end) {
            continue; /* An empty entry, as after a trailing comma */
        }
        equals = entry_start;
        while (equals < entry_end && *equals != '=') {
            ++equals;
        }
        if (equals == entry_end) {
            if (log) {
                log(userdata, entry_start, (size_t)(entry_end - entry_start), "no '=' between port and protocol");
            }
            continue;
        }
        port_start = entry_start;
        port_end = equals;
        proto_start = equals + 1;
        proto_end = entry_end;
        Serial_Trim(&port_start, &port_end);
        Serial_Trim(&proto_start, &proto_end);
        key_length = (size_t)(port_end - port_start);
        if (key_length == 0 || key_length >= sizeof(key)) {
            if (log) {
                log(userdata, entry_start, (size_t)(entry_end - entry_start), "port name empty or too long");
            }
            continue;
        }
        memcpy(key, port_start, key_length);
        key[key_length] = '\0';
        if (!Serial_ValidKey(key)) {
            if (log) {
                log(userdata, entry_start, (size_t)(entry_end - entry_start), "port is neither COMn nor a device instance ID prefix");
            }
            continue;
        }
        module = Serial_FindModule(modules, nmodules, proto_start, (size_t)(proto_end - proto_start));
        if (!module) {
            if (log) {
                log(userdata, entry_start, (size_t)(entry_end - entry_start), "unknown protocol");
            }
            continue;
        }
        for (i = 0; i < count; ++i) {
            if (SDL_Serial_KeysEqual(entries[i].key, key)) {
                break;
            }
        }
        if (i < count) {
            /* A repeated port keeps its last entry */
            memmove(&entries[i], &entries[i + 1], (size_t)(count - i - 1) * sizeof(entries[0]));
            --count;
        } else if (count == max_entries) {
            if (log) {
                log(userdata, entry_start, (size_t)(entry_end - entry_start), "too many ports");
            }
            continue;
        }
        memcpy(entries[count].key, key, key_length + 1);
        entries[count].module = module;
        ++count;
    }
    return count;
}

/* Like SDL_Serial_InstanceMatches, and ? in the pattern matches any one
 * character */
static bool Serial_PatternMatches(const char *pattern, const char *instance_id)
{
    if (!pattern || !instance_id || !*pattern) {
        return false;
    }
    while (*pattern) {
        if (!*instance_id || (*pattern != '?' && Serial_Lower(*pattern) != Serial_Lower(*instance_id))) {
            return false;
        }
        ++pattern;
        ++instance_id;
    }
    return true;
}

const SDL_SerialAutoRule *SDL_Serial_MatchAuto(const SDL_SerialAutoRule *rules, int nrules, const char *instance_id)
{
    int i;

    if (!rules || !instance_id) {
        return NULL;
    }
    for (i = 0; i < nrules; ++i) {
        if (Serial_PatternMatches(rules[i].prefix, instance_id)) {
            return &rules[i];
        }
    }
    return NULL;
}

static bool Serial_ParseHexField(const char *text, const char *field, uint16_t *value)
{
    const size_t field_length = strlen(field);
    uint16_t result = 0;
    size_t i, j;

    for (i = 0; text[i]; ++i) {
        for (j = 0; j < field_length && text[i + j] && Serial_Lower(text[i + j]) == Serial_Lower(field[j]); ++j) {
        }
        if (j < field_length) {
            continue;
        }
        for (j = 0; j < 4; ++j) {
            const char c = Serial_Lower(text[i + field_length + j]);

            if (c >= '0' && c <= '9') {
                result = (uint16_t)((result << 4) | (uint16_t)(c - '0'));
            } else if (c >= 'a' && c <= 'f') {
                result = (uint16_t)((result << 4) | (uint16_t)(c - 'a' + 10));
            } else {
                return false;
            }
        }
        *value = result;
        return true;
    }
    return false;
}

bool SDL_Serial_ParseUSBIds(const char *instance_id, uint16_t *vendor_id, uint16_t *product_id)
{
    uint16_t vendor, product;

    if (!instance_id || !Serial_ParseHexField(instance_id, "VID_", &vendor) ||
        !Serial_ParseHexField(instance_id, "PID_", &product)) {
        return false;
    }
    *vendor_id = vendor;
    *product_id = product;
    return true;
}

void SDL_Serial_DiffPorts(const SDL_SerialPortEntry *old_entries, int nold, const SDL_SerialPortEntry *new_entries, int nnew, SDL_SerialPortChange *old_changes, SDL_SerialPortChange *new_changes)
{
    int i, j;

    for (i = 0; i < nold; ++i) {
        old_changes[i] = SDL_SERIAL_PORT_STOP;
        for (j = 0; j < nnew; ++j) {
            if (SDL_Serial_KeysEqual(old_entries[i].key, new_entries[j].key)) {
                old_changes[i] = (old_entries[i].module == new_entries[j].module) ? SDL_SERIAL_PORT_KEEP : SDL_SERIAL_PORT_RESTART;
                break;
            }
        }
    }
    for (j = 0; j < nnew; ++j) {
        new_changes[j] = SDL_SERIAL_PORT_START;
        for (i = 0; i < nold; ++i) {
            if (SDL_Serial_KeysEqual(old_entries[i].key, new_entries[j].key)) {
                new_changes[j] = SDL_SERIAL_PORT_KEEP;
                break;
            }
        }
    }
}
