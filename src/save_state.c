#include "save_state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "data_win.h"
#include "gml_array.h"
#include "gml_method.h"
#include "instance.h"
#include "log.h"
#include "random.h"
#include "runner.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

// Layout: a fixed header, then a set of sections. Every reference to a GML array,
// method or struct is written as an id, so the graph can contain cycles (a struct
// holding itself, an array of arrays that share a row) without any special casing.
// Sections are ordered so that loading is one pass: the array shapes and methods
// exist before anything that can point at them, and all instances exist before any
// variable is restored.

#define SAVE_STATE_MAGIC   "BSSTATE1"
// 3: values are read after instances and methods exist, so that an array holding a
//    struct or a method restores instead of turning into undefined.
#define SAVE_STATE_VERSION 3u

// Value type tags used in the file. These mirror RValueType, but stay explicit so
// the format does not silently change when a new GML type is added.
#define SST_UNDEFINED 0
#define SST_REAL      1
#define SST_INT32     2
#define SST_INT64     3
#define SST_BOOL      4
#define SST_STRING    5
#define SST_ARRAY     6
#define SST_METHOD    7
#define SST_STRUCT    8
#define SST_ASSETREF  9

// ===[ Writer ]===
typedef struct
{
    FILE* file;
    bool ok;
}
Writer;

static void wrBytes(Writer* w, const void* data, size_t size)
{
    if (!w->ok || size == 0) return;
    if (fwrite(data, 1, size, w->file) != size) w->ok = false;
}

static void wrU8(Writer* w, uint8_t value)  { wrBytes(w, &value, sizeof(value)); }

static void wrU32(Writer* w, uint32_t value) { wrBytes(w, &value, sizeof(value)); }

static void wrI32(Writer* w, int32_t value) { wrBytes(w, &value, sizeof(value)); }

static void wrF32(Writer* w, float value)   { wrBytes(w, &value, sizeof(value)); }

static void wrF64(Writer* w, double value)  { wrBytes(w, &value, sizeof(value)); }

static void wrStr(Writer* w, const char* text)
{
    if (text == nullptr)
    {
        wrU32(w, 0xFFFFFFFFu);
        return;
    }
    const uint32_t length = (uint32_t) strlen(text);
    wrU32(w, length);
    wrBytes(w, text, length);
}

// ===[ Reader ]===
typedef struct
{
    const uint8_t* data;
    size_t size;
    size_t position;
    bool ok;
}
Reader;

static bool rdBytes(Reader* r, void* dst, size_t size)
{
    if (!r->ok) return false;
    if (r->size - r->position < size)
    {
        r->ok = false;
        return false;
    }
    memcpy(dst, r->data + r->position, size);
    r->position += size;
    return true;
}

static uint8_t rdU8(Reader* r)
{
    uint8_t value = 0;
    rdBytes(r, &value, sizeof(value));
    return value;
}

static uint32_t rdU32(Reader* r)
{
    uint32_t value = 0;
    rdBytes(r, &value, sizeof(value));
    return value;
}

static int32_t rdI32(Reader* r)
{
    int32_t value = 0;
    rdBytes(r, &value, sizeof(value));
    return value;
}

static float rdF32(Reader* r)
{
    float value = 0.0f;
    rdBytes(r, &value, sizeof(value));
    return value;
}

static double rdF64(Reader* r)
{
    double value = 0.0;
    rdBytes(r, &value, sizeof(value));
    return value;
}

// Returns a heap copy of the stored string, or nullptr when the stored value was
// NULL. The caller owns the result.
static char* rdStr(Reader* r)
{
    uint32_t length = rdU32(r);
    if (!r->ok || length == 0xFFFFFFFFu) return nullptr;
    if (length == 0) return safeStrdup("");
    if (r->size - r->position < length)
    {
        r->ok = false;
        return nullptr;
    }
    char* text = (char*) safeMalloc(length + 1);
    memcpy(text, r->data + r->position, length);
    text[length] = '\0';
    r->position += length;
    return text;
}

// ===[ Value graph ]===
//
// Arrays and methods are shared, mutable objects; the state has to preserve the
// sharing (two variables pointing at the same array must still do so after a load).
// Each object gets an id on first sight and is written once, in its own table.
typedef struct
{
    void* key;
    int32_t value;
}
PtrIdEntry;

typedef struct
{
    PtrIdEntry* arrayIds;
    GMLArray** arrays;

    PtrIdEntry* methodIds;
    GMLMethod** methods;
}
Graph;

static int32_t graphArrayId(Graph* g, GMLArray* arr)
{
    if (arr == nullptr) return -1;
    ptrdiff_t existing = hmgeti(g->arrayIds, arr);
    if (existing >= 0) return g->arrayIds[existing].value;
    return -1;
}

static int32_t graphMethodId(Graph* g, GMLMethod* method)
{
    if (method == nullptr) return -1;
    ptrdiff_t existing = hmgeti(g->methodIds, method);
    if (existing >= 0) return g->methodIds[existing].value;
    return -1;
}

static void graphWalkValue(Graph* g, RValue value);

// Visits every array a value can reach, so that ids exist before anything is
// written. Structs need no visit: they are instances, and every struct in the game
// is in the runner's struct registry, which the instance section walks.
static void graphWalkArray(Graph* g, GMLArray* arr)
{
    if (arr == nullptr || graphArrayId(g, arr) >= 0) return;
    hmput(g->arrayIds, arr, (int32_t) arrlen(g->arrays));
    arrput(g->arrays, arr);

    if (arr->type == GML_LEGACY_ARRAY)
    {
        repeat((int32_t) arr->legacy.rowCount, row)
        {
            GMLArrayRow* line = &arr->legacy.rows[row];
            repeat(line->length, col)
            {
                graphWalkValue(g, line->data[col]);
            }
        }
    }
    else
    {
        repeat(arr->modern.length, i)
        {
            graphWalkValue(g, arr->modern.data[i]);
        }
    }
}

static void graphWalkMethod(Graph* g, GMLMethod* method)
{
    if (method == nullptr || graphMethodId(g, method) >= 0) return;
    hmput(g->methodIds, method, (int32_t) arrlen(g->methods));
    arrput(g->methods, method);
}

static void graphWalkValue(Graph* g, RValue value)
{
    switch (value.type)
    {
        case RVALUE_ARRAY:  graphWalkArray(g, value.array); break;
        case RVALUE_METHOD: graphWalkMethod(g, value.method); break;
        default: break;
    }
}

static void graphFree(Graph* g)
{
    hmfree(g->arrayIds);
    hmfree(g->methodIds);
    arrfree(g->arrays);
    arrfree(g->methods);
    g->arrayIds = nullptr;
    g->methodIds = nullptr;
    g->arrays = nullptr;
    g->methods = nullptr;
}

static void graphWalkInstanceVars(Graph* g, Instance* inst)
{
    repeat((int32_t) inst->selfVars.capacity, i)
    {
        IntRValueEntry* entry = &inst->selfVars.entries[i];
        if (entry->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
        graphWalkValue(g, entry->value);
    }
}

// Looks a builtin's name up by pointer, which is the only portable way to store a
// method that refers to a builtin (a function pointer means nothing in another
// process).
static const char* findBuiltinName(VMContext* ctx, BuiltinFunc function)
{
    if (function == nullptr || ctx == nullptr) return nullptr;
    repeat((int32_t) shlenu(ctx->builtinMap), i)
    {
        if (ctx->builtinMap[i].value == function) return ctx->builtinMap[i].key;
    }
    return nullptr;
}

// ===[ Values ]===
static void wrValue(Writer* w, Graph* g, RValue value)
{
    switch (value.type)
    {
        case RVALUE_UNDEFINED:
            wrU8(w, SST_UNDEFINED);
            break;
        case RVALUE_REAL:
            wrU8(w, SST_REAL);
            wrF64(w, (double) value.real);
            break;
        case RVALUE_INT32:
            wrU8(w, SST_INT32);
            wrI32(w, value.int32);
            // Instance ids are carried in an int32 with this tag set; keep it so
            // that code looking for "is this an instance?" still finds one.
            wrU8(w, value.assetRefType);
            break;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64:
            wrU8(w, SST_INT64);
            wrBytes(w, &value.int64, sizeof(value.int64));
            break;
#endif
        case RVALUE_BOOL:
            wrU8(w, SST_BOOL);
            wrI32(w, value.int32);
            break;
        case RVALUE_STRING:
            wrU8(w, SST_STRING);
            wrStr(w, value.string);
            break;
        case RVALUE_ARRAY:
        {
            // An array that the collection pass never saw can only be reachable
            // from a part of the VM this state does not cover (persistent-room
            // snapshots, for instance). Writing it as undefined keeps the file
            // loadable instead of failing the whole load later.
            const int32_t id = graphArrayId(g, value.array);
            if (id < 0)
            {
                wrU8(w, SST_UNDEFINED);
                break;
            }
            wrU8(w, SST_ARRAY);
            wrI32(w, id);
            break;
        }
        case RVALUE_METHOD:
        {
            const int32_t id = graphMethodId(g, value.method);
            if (id < 0)
            {
                wrU8(w, SST_UNDEFINED);
                break;
            }
            wrU8(w, SST_METHOD);
            wrI32(w, id);
            break;
        }
        case RVALUE_STRUCT:
            wrU8(w, SST_STRUCT);
            wrI32(w, value.structInst != nullptr ? (int32_t) value.structInst->instanceId : -1);
            break;
        case RVALUE_ASSETREF:
            wrU8(w, SST_ASSETREF);
            wrI32(w, value.int32);
            wrU8(w, value.assetRefType);
            break;
        default:
            wrU8(w, SST_UNDEFINED);
            break;
    }
}

typedef struct
{
    Runner* runner;
    Graph* graph;
    GMLArray** arrays;
    GMLMethod** methods;
}
Loader;

static RValue rdValue(Reader* r, Loader* loader)
{
    const uint8_t tag = rdU8(r);
    switch (tag)
    {
        case SST_REAL:
            return RValue_makeReal((GMLReal) rdF64(r));
        case SST_INT32:
        {
            RValue value = RValue_makeInt32(rdI32(r));
            value.assetRefType = rdU8(r);
            return value;
        }
#ifndef NO_RVALUE_INT64
        case SST_INT64:
        {
            RValue value = RValue_makeInt64(0);
            rdBytes(r, &value.int64, sizeof(value.int64));
            return value;
        }
#endif
        case SST_BOOL:
            return RValue_makeBool(rdI32(r) != 0);
        case SST_STRING:
            return RValue_makeOwnedString(rdStr(r));
        case SST_ARRAY:
        {
            const int32_t id = rdI32(r);
            if (id < 0 || id >= (int32_t) arrlen(loader->arrays))
            {
                r->ok = false;
                return RValue_makeUndefined();
            }
            GMLArray* array = loader->arrays[id];
            GMLArray_incRef(array);
            return RValue_makeArray(array);
        }
        case SST_METHOD:
        {
            const int32_t id = rdI32(r);
            if (id < 0 || id >= (int32_t) arrlen(loader->methods))
            {
                r->ok = false;
                return RValue_makeUndefined();
            }
            GMLMethod* method = loader->methods[id];
            GMLMethod_incRef(method);
            return RValue_makeMethod(method);
        }
        case SST_STRUCT:
        {
            const int32_t instanceId = rdI32(r);
            Instance* inst = instanceId < 0 ? nullptr
                : hmget(loader->runner->instancesById, instanceId);
            if (inst == nullptr) return RValue_makeUndefined();
            return RValue_makeStructAndIncRef(inst);
        }
        case SST_ASSETREF:
        {
            const int32_t index = rdI32(r);
            const uint8_t type = rdU8(r);
            return RValue_makeAssetRef(index, type);
        }
        case SST_UNDEFINED:
        default:
            return RValue_makeUndefined();
    }
}

// ===[ Instances ]===
//
// Flags are packed into one word to keep the record compact and to make adding a
// flag later a one-line change on both sides.
#define INSTFLAG_ACTIVE         (1u << 0)
#define INSTFLAG_VISIBLE        (1u << 1)
#define INSTFLAG_SOLID          (1u << 2)
#define INSTFLAG_PERSISTENT     (1u << 3)
#define INSTFLAG_CREATEFIRED    (1u << 4)
#define INSTFLAG_OUTSIDEROOM    (1u << 5)
#define INSTFLAG_MOUSEOVER      (1u << 6)
#define INSTFLAG_CLEANUPFIRED   (1u << 7)
#define INSTFLAG_PINNED         (1u << 8)
#define INSTFLAG_TIMELINELOOP   (1u << 9)
#define INSTFLAG_TIMELINERUN    (1u << 10)

static uint32_t instanceFlags(const Instance* inst)
{
    uint32_t flags = 0;
    if (inst->active) flags |= INSTFLAG_ACTIVE;
    if (inst->visible) flags |= INSTFLAG_VISIBLE;
    if (inst->solid) flags |= INSTFLAG_SOLID;
    if (inst->persistent) flags |= INSTFLAG_PERSISTENT;
    if (inst->createEventFired) flags |= INSTFLAG_CREATEFIRED;
    if (inst->outsideRoom) flags |= INSTFLAG_OUTSIDEROOM;
    if (inst->mouseOver) flags |= INSTFLAG_MOUSEOVER;
    if (inst->cleanupEventFired) flags |= INSTFLAG_CLEANUPFIRED;
    if (inst->pinned) flags |= INSTFLAG_PINNED;
    if (inst->timelineLoop) flags |= INSTFLAG_TIMELINELOOP;
    if (inst->timelineRunning) flags |= INSTFLAG_TIMELINERUN;
    return flags;
}

static void wrInstanceFields(Writer* w, Instance* inst, bool isStruct)
{
    wrU8(w, isStruct ? 1 : 0);
    wrU32(w, inst->instanceId);
    wrI32(w, inst->objectIndex);
    wrI32(w, inst->roomIndex);
    wrI32(w, inst->constructorCodeIndex);
    wrI32(w, inst->staticParent != nullptr ? (int32_t) inst->staticParent->instanceId : -1);

    wrF32(w, inst->x);
    wrF32(w, inst->y);
    wrF32(w, inst->xprevious);
    wrF32(w, inst->yprevious);
    wrF32(w, inst->xstart);
    wrF32(w, inst->ystart);

    wrU32(w, instanceFlags(inst));
    wrU32(w, inst->activeAlarmMask);
    wrI32(w, inst->maskIndex);

    wrI32(w, inst->spriteIndex);
    wrF32(w, inst->imageSpeed);
    wrF32(w, inst->imageIndex);
    wrF32(w, inst->imageXscale);
    wrF32(w, inst->imageYscale);
    wrF32(w, inst->imageAlpha);
    wrF64(w, (double) inst->imageAngle);
    wrU32(w, inst->imageBlend);
    wrI32(w, inst->depth);
    wrI32(w, inst->layer);

    wrF32(w, inst->speed);
    wrF32(w, inst->direction);
    wrF32(w, inst->hspeed);
    wrF32(w, inst->vspeed);
    wrF32(w, inst->friction);
    wrF32(w, inst->gravity);
    wrF32(w, inst->gravityDirection);

    wrI32(w, inst->pathIndex);
    wrF32(w, inst->pathPosition);
    wrF32(w, inst->pathPositionPrevious);
    wrF32(w, inst->pathSpeed);
    wrF32(w, inst->pathScale);
    wrF32(w, inst->pathOrientation);
    wrI32(w, inst->pathEndAction);
    wrF32(w, inst->pathXStart);
    wrF32(w, inst->pathYStart);

    wrI32(w, inst->timelineIndex);
    wrF32(w, inst->timelinePosition);
    wrF32(w, inst->timelineSpeed);

    repeat(GML_ALARM_COUNT, i)
    {
        wrI32(w, inst->alarm[i]);
    }
}

static void rdInstanceFields(Reader* r, Instance* inst, int32_t* outStaticParentId, uint32_t* outAlarms)
{
    inst->roomIndex = rdI32(r);
    inst->constructorCodeIndex = rdI32(r);
    // Resolved after every instance exists.
    *outStaticParentId = rdI32(r);

    inst->x = rdF32(r);
    inst->y = rdF32(r);
    inst->xprevious = rdF32(r);
    inst->yprevious = rdF32(r);
    inst->xstart = rdF32(r);
    inst->ystart = rdF32(r);

    const uint32_t flags = rdU32(r);
    inst->active = (flags & INSTFLAG_ACTIVE) != 0;
    inst->visible = (flags & INSTFLAG_VISIBLE) != 0;
    inst->solid = (flags & INSTFLAG_SOLID) != 0;
    inst->persistent = (flags & INSTFLAG_PERSISTENT) != 0;
    inst->createEventFired = (flags & INSTFLAG_CREATEFIRED) != 0;
    inst->outsideRoom = (flags & INSTFLAG_OUTSIDEROOM) != 0;
    inst->mouseOver = (flags & INSTFLAG_MOUSEOVER) != 0;
    inst->cleanupEventFired = (flags & INSTFLAG_CLEANUPFIRED) != 0;
    inst->pinned = (flags & INSTFLAG_PINNED) != 0;
    inst->timelineLoop = (flags & INSTFLAG_TIMELINELOOP) != 0;
    inst->timelineRunning = (flags & INSTFLAG_TIMELINERUN) != 0;

    inst->activeAlarmMask = (uint16_t) rdU32(r);
    inst->maskIndex = rdI32(r);

    inst->spriteIndex = rdI32(r);
    inst->imageSpeed = rdF32(r);
    inst->imageIndex = rdF32(r);
    inst->imageXscale = rdF32(r);
    inst->imageYscale = rdF32(r);
    inst->imageAlpha = rdF32(r);
    inst->imageAngle = (GMLReal) rdF64(r);
    inst->imageBlend = rdU32(r);
    inst->depth = rdI32(r);
    inst->layer = rdI32(r);

    inst->speed = rdF32(r);
    inst->direction = rdF32(r);
    inst->hspeed = rdF32(r);
    inst->vspeed = rdF32(r);
    inst->friction = rdF32(r);
    inst->gravity = rdF32(r);
    inst->gravityDirection = rdF32(r);

    inst->pathIndex = rdI32(r);
    inst->pathPosition = rdF32(r);
    inst->pathPositionPrevious = rdF32(r);
    inst->pathSpeed = rdF32(r);
    inst->pathScale = rdF32(r);
    inst->pathOrientation = rdF32(r);
    inst->pathEndAction = rdI32(r);
    inst->pathXStart = rdF32(r);
    inst->pathYStart = rdF32(r);

    inst->timelineIndex = rdI32(r);
    inst->timelinePosition = rdF32(r);
    inst->timelineSpeed = rdF32(r);

    repeat(GML_ALARM_COUNT, i)
    {
        outAlarms[i] = rdI32(r);
    }
}

// ===[ Save ]===
uint32_t SaveState_gameId(DataWin* dataWin)
{
    if (dataWin == nullptr) return 0;

    // FNV-1a over the identity fields. A state is only valid for the exact
    // data.win it came from, so a mismatched file has to be rejected rather than
    // loaded into a game whose object/room indices mean something else.
    const uint8_t* name = (const uint8_t*) (dataWin->gen8.name != nullptr ? dataWin->gen8.name : "");
    uint32_t hash = 2166136261u;
    while (*name != 0)
    {
        hash = (hash ^ *name++) * 16777619u;
    }

    const uint32_t identity[] = {
        (uint32_t) dataWin->gen8.gameID,
        dataWin->room.count,
        dataWin->objt.count,
        dataWin->sprt.count,
        dataWin->code.count,
    };
    repeat((int32_t) (sizeof(identity) / sizeof(identity[0])), i)
    {
        const uint8_t* bytes = (const uint8_t*) &identity[i];
        repeat((int32_t) sizeof(identity[i]), b)
        {
            hash = (hash ^ bytes[b]) * 16777619u;
        }
    }
    return hash;
}

static void wrGlobals(Writer* w, Graph* g, Instance* globalScope)
{
    if (globalScope == nullptr)
    {
        wrU32(w, 0);
        return;
    }

    uint32_t count = 0;
    repeat((int32_t) globalScope->selfVars.capacity, i)
    {
        if (globalScope->selfVars.entries[i].key != INT_RVALUE_HASHMAP_EMPTY_KEY) count++;
    }
    wrU32(w, count);

    repeat((int32_t) globalScope->selfVars.capacity, i)
    {
        IntRValueEntry* entry = &globalScope->selfVars.entries[i];
        if (entry->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
        wrI32(w, entry->key);
        wrValue(w, g, entry->value);
    }
}

static void wrArrayTable(Writer* w, Graph* g)
{
    wrU32(w, (uint32_t) arrlen(g->arrays));
    repeat((int32_t) arrlen(g->arrays), i)
    {
        GMLArray* arr = g->arrays[i];
        if (arr->type == GML_LEGACY_ARRAY)
        {
            wrU32(w, (uint32_t) arr->legacy.rowCount);
            repeat((int32_t) arr->legacy.rowCount, row)
            {
                wrU32(w, (uint32_t) arr->legacy.rows[row].length);
            }
        }
        else
        {
            wrU32(w, arr->modern.length > 0 ? 1u : 0u);
            wrU32(w, (uint32_t) arr->modern.length);
        }
    }
}

static void wrArrayValues(Writer* w, Graph* g)
{
    repeat((int32_t) arrlen(g->arrays), i)
    {
        GMLArray* arr = g->arrays[i];
        if (arr->type == GML_LEGACY_ARRAY)
        {
            repeat((int32_t) arr->legacy.rowCount, row)
            {
                GMLArrayRow* line = &arr->legacy.rows[row];
                repeat(line->length, col)
                {
                    wrValue(w, g, line->data[col]);
                }
            }
        }
        else
        {
            repeat(arr->modern.length, col)
            {
                wrValue(w, g, arr->modern.data[col]);
            }
        }
    }
}

static void wrMethodTable(Writer* w, Graph* g, VMContext* ctx)
{
    wrU32(w, (uint32_t) arrlen(g->methods));
    repeat((int32_t) arrlen(g->methods), i)
    {
        GMLMethod* method = g->methods[i];
        wrI32(w, method->codeIndex);
        wrI32(w, method->boundInstanceId);
        if (method->builtin != nullptr)
        {
            wrU8(w, 1);
            wrStr(w, findBuiltinName(ctx, method->builtin));
        }
        else if (method->codeIndex < 0)
        {
            wrU8(w, 2);
            wrStr(w, method->unresolvedName);
        }
        else
        {
            wrU8(w, 0);
            wrStr(w, nullptr);
        }
    }
}

static void wrDsPools(Writer* w, Graph* g, Runner* runner)
{
    // ds ids are indices into these pools, and games store them in variables, so the
    // pool shape (including destroyed slots) has to survive exactly.
    wrU32(w, (uint32_t) arrlen(runner->dsMapPool));
    repeat((int32_t) arrlen(runner->dsMapPool), i)
    {
        DsMapEntry* map = runner->dsMapPool[i];
        if (map == nullptr)
        {
            wrU8(w, 0);
            continue;
        }
        wrU8(w, 1);
        wrU32(w, (uint32_t) shlenu(map));
        repeat((int32_t) shlenu(map), j)
        {
            wrStr(w, map[j].key);
            wrValue(w, g, map[j].value);
        }
    }

    wrU32(w, (uint32_t) arrlen(runner->dsListPool));
    repeat((int32_t) arrlen(runner->dsListPool), i)
    {
        DsList* list = &runner->dsListPool[i];
        wrU8(w, list->freed ? 1 : 0);
        wrU32(w, (uint32_t) arrlen(list->items));
        repeat(arrlen(list->items), j)
        {
            wrValue(w, g, list->items[j]);
        }
    }

    wrU32(w, (uint32_t) arrlen(runner->dsQueuePool));
    repeat((int32_t) arrlen(runner->dsQueuePool), i)
    {
        DsQueue* queue = &runner->dsQueuePool[i];
        wrU8(w, queue->freed ? 1 : 0);
        wrU32(w, (uint32_t) arrlen(queue->items));
        repeat(arrlen(queue->items), j)
        {
            wrValue(w, g, queue->items[j]);
        }
    }

    wrU32(w, (uint32_t) arrlen(runner->dsStackPool));
    repeat((int32_t) arrlen(runner->dsStackPool), i)
    {
        DsStack* stack = &runner->dsStackPool[i];
        wrU8(w, stack->freed ? 1 : 0);
        wrU32(w, (uint32_t) arrlen(stack->items));
        repeat(arrlen(stack->items), j)
        {
            wrValue(w, g, stack->items[j]);
        }
    }

    wrU32(w, (uint32_t) arrlen(runner->dsPriorityPool));
    repeat((int32_t) arrlen(runner->dsPriorityPool), i)
    {
        DsPriority* priority = &runner->dsPriorityPool[i];
        wrU8(w, priority->freed ? 1 : 0);
        wrU32(w, (uint32_t) arrlen(priority->items));
        repeat(arrlen(priority->items), j)
        {
            wrF64(w, (double) priority->items[j].depth);
            wrValue(w, g, priority->items[j].item);
        }
    }

    wrU32(w, (uint32_t) arrlen(runner->dsGridPool));
    repeat((int32_t) arrlen(runner->dsGridPool), i)
    {
        DsGrid* grid = &runner->dsGridPool[i];
        wrU8(w, grid->freed ? 1 : 0);
        wrI32(w, grid->width);
        wrI32(w, grid->height);
        const size_t cells = (size_t) grid->width * (size_t) grid->height;
        repeat((int32_t) cells, j)
        {
            wrValue(w, g, grid->items[j]);
        }
    }

    wrU32(w, (uint32_t) arrlen(runner->gmlBufferPool));
    repeat((int32_t) arrlen(runner->gmlBufferPool), i)
    {
        GmlBuffer* buffer = &runner->gmlBufferPool[i];
        wrU8(w, buffer->isValid ? 1 : 0);
        wrI32(w, buffer->size);
        wrI32(w, buffer->position);
        wrI32(w, buffer->usedSize);
        wrI32(w, buffer->alignment);
        wrI32(w, buffer->type);
        if (buffer->isValid && buffer->data != nullptr)
            wrBytes(w, buffer->data, (size_t) buffer->size);
    }
}

static void wrScalars(Writer* w, Runner* runner)
{
    // The room index comes first: a load has to know where to put the player before
    // the rest of the scalars are applied, because building the room rewrites some of
    // them (views, backgrounds, application surface size).
    wrI32(w, runner->currentRoomIndex);
    wrI32(w, runner->pendingRoom);
    wrI32(w, runner->frameCount);
    wrU32(w, runner->nextInstanceId);
    wrU32(w, runner->nextLayerId);

    wrF64(w, (double) runner->score);
    wrF64(w, (double) runner->lives);
    wrF64(w, (double) runner->health);
    wrF64(w, (double) runner->gameSpeedOverride);
    wrF64(w, (double) runner->deltaTime);
    wrF64(w, (double) runner->fps);

    uint32_t flags = 0;
    if (runner->viewsEnabled) flags |= 1u << 0;
    if (runner->drawEnabled) flags |= 1u << 1;
    if (runner->paused) flags |= 1u << 2;
    if (runner->appSurfaceEnabled) flags |= 1u << 3;
    if (runner->appSurfaceAutoDraw) flags |= 1u << 4;
    if (runner->usingAppSurface) flags |= 1u << 5;
    if (runner->gameStartFired) flags |= 1u << 6;
    if (runner->drawBackgroundColor) flags |= 1u << 7;
    if (runner->forceDrawDepth) flags |= 1u << 8;
    if (runner->applyOffsetForPrimitives) flags |= 1u << 9;
    if (runner->inGuiPass) flags |= 1u << 10;
    wrU32(w, flags);

    wrI32(w, runner->applicationWidth);
    wrI32(w, runner->applicationHeight);
    wrI32(w, runner->oldApplicationWidth);
    wrI32(w, runner->oldApplicationHeight);
    wrI32(w, runner->widescreenExtraWidth);
    wrI32(w, runner->widescreenExtraHeight);
    wrI32(w, runner->guiWidth);
    wrI32(w, runner->guiHeight);
    wrU32(w, runner->backgroundColor);

    wrBytes(w, &runner->random, sizeof(runner->random));
    wrBytes(w, runner->views, sizeof(runner->views));
    wrBytes(w, runner->defaultCameras, sizeof(runner->defaultCameras));
    wrBytes(w, runner->userCameras, sizeof(runner->userCameras));
    wrBytes(w, runner->backgrounds, sizeof(runner->backgrounds));

    wrU32(w, (uint32_t) hmlenu(runner->tileLayerMap));
    repeat((int32_t) hmlenu(runner->tileLayerMap), i)
    {
        wrI32(w, runner->tileLayerMap[i].key);
        wrBytes(w, &runner->tileLayerMap[i].value, sizeof(TileLayerState));
    }
}

// ===[ Layers ]===
// Layer element ids come from a counter that the room build advances, and GML keeps
// those ids in instance variables (a tilemap element id for tilemap_get_at_pixel, an
// element id for the layer_* calls). Rebuilding the room on load hands out fresh ids,
// so every stored id stops resolving and the runner dies on the first builtin that
// looks one up. The state therefore carries the layer element ids and their order,
// plus the layer fields the game can change at runtime.
static void wrLayers(Writer* w, Runner* runner)
{
    wrU32(w, runner->nextLayerDrawOrder);
    wrU32(w, (uint32_t) arrlen(runner->runtimeLayers));
    repeat((int32_t) arrlen(runner->runtimeLayers), i)
    {
        RuntimeLayer* layer = &runner->runtimeLayers[i];
        wrU32(w, layer->id);
        wrI32(w, layer->depth);
        wrU32(w, layer->drawOrder);
        wrF32(w, layer->xOffset);
        wrF32(w, layer->yOffset);
        wrF32(w, layer->hSpeed);
        wrF32(w, layer->vSpeed);
        wrU8(w, layer->visible ? 1 : 0);
        wrU8(w, layer->automaticDepth ? 1 : 0);
        wrI32(w, layer->shaderIndex);
        // Dynamic layers only exist because GML created them, so a load has to
        // recreate them: the room build alone never produces one.
        wrU8(w, layer->dynamic ? 1 : 0);
        wrI32(w, layer->beginScript);
        wrI32(w, layer->endScript);
        wrStr(w, layer->dynamicName);

        wrU32(w, (uint32_t) arrlen(layer->elements));
        repeat((int32_t) arrlen(layer->elements), j)
        {
            RuntimeLayerElement* el = &layer->elements[j];
            wrU32(w, el->id);
            wrU8(w, (uint8_t) el->type);
            wrU8(w, el->visible ? 1 : 0);
            wrF32(w, el->alpha);
            wrU32(w, el->blend);
            wrI32(w, el->instanceId);

            // Only tiles made by layer_tile_create have no room data to rebuild from,
            // so those are the only payloads that have to be written out.
            const bool ownedTile = el->type == RuntimeLayerElementType_Tile
                && el->tileElementOwned && el->tileElement != nullptr;
            wrU8(w, ownedTile ? 1 : 0);
            if (ownedTile) wrBytes(w, el->tileElement, sizeof(RoomTile));
        }
    }
}

bool SaveState_save(Runner* runner, const char* path)
{
    if (runner == nullptr || path == nullptr) return false;

    DataWin* dataWin = runner->dataWin;
    Instance* globalScope = runner->vmContext != nullptr ? runner->vmContext->globalScopeInstance : nullptr;

    Graph graph = {0};

    // Collect first: every array and method that any value can reach, so that ids
    // exist before they are written.
    if (globalScope != nullptr) graphWalkInstanceVars(&graph, globalScope);
    repeat(arrlen(runner->instances), i) graphWalkInstanceVars(&graph, runner->instances[i]);
    repeat(arrlen(runner->structInstances), i) graphWalkInstanceVars(&graph, runner->structInstances[i]);

    repeat((int32_t) arrlen(runner->dsMapPool), i)
    {
        DsMapEntry* map = runner->dsMapPool[i];
        if (map == nullptr) continue;
        repeat((int32_t) shlenu(map), j) graphWalkValue(&graph, map[j].value);
    }
    repeat((int32_t) arrlen(runner->dsListPool), i)
    {
        repeat(arrlen(runner->dsListPool[i].items), j) graphWalkValue(&graph, runner->dsListPool[i].items[j]);
    }
    repeat((int32_t) arrlen(runner->dsQueuePool), i)
    {
        repeat(arrlen(runner->dsQueuePool[i].items), j) graphWalkValue(&graph, runner->dsQueuePool[i].items[j]);
    }
    repeat((int32_t) arrlen(runner->dsStackPool), i)
    {
        repeat(arrlen(runner->dsStackPool[i].items), j) graphWalkValue(&graph, runner->dsStackPool[i].items[j]);
    }
    repeat((int32_t) arrlen(runner->dsPriorityPool), i)
    {
        repeat(arrlen(runner->dsPriorityPool[i].items), j) graphWalkValue(&graph, runner->dsPriorityPool[i].items[j].item);
    }
    repeat((int32_t) arrlen(runner->dsGridPool), i)
    {
        DsGrid* grid = &runner->dsGridPool[i];
        const size_t cells = (size_t) grid->width * (size_t) grid->height;
        repeat((int32_t) cells, j) graphWalkValue(&graph, grid->items[j]);
    }

    Writer w = {0};
    w.file = fopen(path, "wb");
    w.ok = w.file != nullptr;
    if (!w.ok)
    {
        logError("SaveState: cannot open '%s' for writing: %s\n", path, strerror(errno));
        graphFree(&graph);
        return false;
    }

    wrBytes(&w, SAVE_STATE_MAGIC, 8);
    wrU32(&w, SAVE_STATE_VERSION);
    wrU32(&w, SaveState_gameId(dataWin));

    wrScalars(&w, runner);

    // Instance records come first: a value can point at an instance (a struct value,
    // or a method bound to an instance), so those records have to be in place before
    // the first value is read back.
    const uint32_t instanceCount = (uint32_t) (arrlen(runner->instances) + arrlen(runner->structInstances));
    wrU32(&w, instanceCount);
    repeat(arrlen(runner->instances), i) wrInstanceFields(&w, runner->instances[i], false);
    repeat(arrlen(runner->structInstances), i) wrInstanceFields(&w, runner->structInstances[i], true);

    // Then the shapes of every array and every method, and only after both tables are
    // whole do the array elements follow - an element can be an array, a method or a
    // struct, and all three have to resolve when it is read.
    wrArrayTable(&w, &graph);
    wrMethodTable(&w, &graph, runner->vmContext);
    wrArrayValues(&w, &graph);

    repeat(arrlen(runner->instances), i)
    {
        Instance* inst = runner->instances[i];
        wrU32(&w, inst->selfVars.count);
        repeat((int32_t) inst->selfVars.capacity, slot)
        {
            IntRValueEntry* entry = &inst->selfVars.entries[slot];
            if (entry->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
            wrI32(&w, entry->key);
            wrValue(&w, &graph, entry->value);
        }
    }
    repeat(arrlen(runner->structInstances), i)
    {
        Instance* inst = runner->structInstances[i];
        wrU32(&w, inst->selfVars.count);
        repeat((int32_t) inst->selfVars.capacity, slot)
        {
            IntRValueEntry* entry = &inst->selfVars.entries[slot];
            if (entry->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
            wrI32(&w, entry->key);
            wrValue(&w, &graph, entry->value);
        }
    }

    wrGlobals(&w, &graph, globalScope);
    wrDsPools(&w, &graph, runner);
    // Last: the layer section points at instances, so it has to be applied after every
    // instance exists.
    wrLayers(&w, runner);

    if (w.file != nullptr)
    {
        if (fflush(w.file) != 0) w.ok = false;
        if (fclose(w.file) != 0) w.ok = false;
    }

    logInfo("SaveState: saved %u instances (%u arrays, %u methods) to '%s'\n",
            instanceCount, (unsigned) arrlen(graph.arrays), (unsigned) arrlen(graph.methods), path);

    graphFree(&graph);
    return w.ok;
}

// ===[ Load ]===
static void rdArrayTable(Reader* r, Loader* loader)
{
    const uint32_t count = rdU32(r);
    if (count > 1000000u)
    {
        r->ok = false;
        return;
    }

    repeat((int32_t) count, i)
    {
        GMLArray* arr = GMLArray_create(loader->runner->dataWin, 0);
        // The creator's reference is neutralised: ids are only kept alive by the
        // values that point at them, exactly as when the game built them.
        arr->refCount = 0;

        const uint32_t rowCount = rdU32(r);
        if (arr->type == GML_LEGACY_ARRAY)
        {
            if (rowCount > 0)
            {
                arr->legacy.rowCount = (int32_t) rowCount;
                arr->legacy.rowCapacity = (int32_t) rowCount;
                arr->legacy.rows = (GMLArrayRow*) safeCalloc(rowCount, sizeof(GMLArrayRow));
                repeat((int32_t) rowCount, row)
                {
                    const uint32_t length = rdU32(r);
                    arr->legacy.rows[row].length = (int32_t) length;
                    arr->legacy.rows[row].capacity = (int32_t) length;
                    arr->legacy.rows[row].data = length > 0
                        ? (RValue*) safeCalloc(length, sizeof(RValue))
                        : nullptr;
                }
            }
        }
        else
        {
            const uint32_t length = rdU32(r);
            if (length > 0)
            {
                arr->modern.length = (int32_t) length;
                arr->modern.capacity = (int32_t) length;
                arr->modern.data = (RValue*) safeCalloc(length, sizeof(RValue));
            }
        }

        arrput(loader->arrays, arr);
    }
}

static void rdArrayValues(Reader* r, Loader* loader)
{
    repeat((int32_t) arrlen(loader->arrays), i)
    {
        GMLArray* arr = loader->arrays[i];
        if (arr->type == GML_LEGACY_ARRAY)
        {
            repeat((int32_t) arr->legacy.rowCount, row)
            {
                GMLArrayRow* line = &arr->legacy.rows[row];
                repeat(line->length, col)
                {
                    line->data[col] = rdValue(r, loader);
                }
            }
        }
        else
        {
            repeat(arr->modern.length, col)
            {
                arr->modern.data[col] = rdValue(r, loader);
            }
        }
    }
}

static void rdMethodTable(Reader* r, Loader* loader)
{
    VMContext* ctx = loader->runner->vmContext;
    const uint32_t count = rdU32(r);
    if (count > 1000000u)
    {
        r->ok = false;
        return;
    }

    repeat((int32_t) count, i)
    {
        const int32_t codeIndex = rdI32(r);
        const int32_t boundInstanceId = rdI32(r);
        const uint8_t kind = rdU8(r);
        char* name = rdStr(r);

        GMLMethod* method = nullptr;
        if (kind == 1 && name != nullptr)
        {
            BuiltinFunc function = VM_findBuiltin(ctx, name);
            method = function != nullptr
                ? GMLMethod_createBuiltin(function, boundInstanceId)
                // The unresolved name is stored by pointer, so it has to outlive this
                // call; the method owns it.
                : GMLMethod_createUnresolved(safeStrdup(name), boundInstanceId);
        }
        else if (kind == 2)
        {
            method = GMLMethod_createUnresolved(safeStrdup(name != nullptr ? name : "?function?"),
                                                boundInstanceId);
        }
        else
        {
            method = GMLMethod_create(codeIndex, boundInstanceId);
        }

        free(name);
        method->refCount = 0;
        arrput(loader->methods, method);
    }
}

static void rdDsPools(Reader* r, Loader* loader)
{
    Runner* runner = loader->runner;

    const uint32_t mapCount = rdU32(r);
    repeat((int32_t) mapCount, i)
    {
        DsMapEntry* map = nullptr;
        if (rdU8(r) != 0)
        {
            const uint32_t entryCount = rdU32(r);
            repeat((int32_t) entryCount, j)
            {
                char* key = rdStr(r);
                RValue value = rdValue(r, loader);
                if (key == nullptr) continue;
                // stb_ds string maps keep the key pointer, and the teardown path
                // frees it, so the key has to be a heap copy.
                shput(map, key, value);
            }
        }
        arrput(runner->dsMapPool, map);
    }

    const uint32_t listCount = rdU32(r);
    repeat((int32_t) listCount, i)
    {
        DsList list = {0};
        list.freed = rdU8(r) != 0;
        const uint32_t itemCount = rdU32(r);
        repeat((int32_t) itemCount, j)
        {
            arrput(list.items, rdValue(r, loader));
        }
        arrput(runner->dsListPool, list);
    }

    const uint32_t queueCount = rdU32(r);
    repeat((int32_t) queueCount, i)
    {
        DsQueue queue = {0};
        queue.freed = rdU8(r) != 0;
        const uint32_t itemCount = rdU32(r);
        repeat((int32_t) itemCount, j)
        {
            arrput(queue.items, rdValue(r, loader));
        }
        arrput(runner->dsQueuePool, queue);
    }

    const uint32_t stackCount = rdU32(r);
    repeat((int32_t) stackCount, i)
    {
        DsStack stack = {0};
        stack.freed = rdU8(r) != 0;
        const uint32_t itemCount = rdU32(r);
        repeat((int32_t) itemCount, j)
        {
            arrput(stack.items, rdValue(r, loader));
        }
        arrput(runner->dsStackPool, stack);
    }

    const uint32_t priorityCount = rdU32(r);
    repeat((int32_t) priorityCount, i)
    {
        DsPriority priority = {0};
        priority.freed = rdU8(r) != 0;
        const uint32_t itemCount = rdU32(r);
        repeat((int32_t) itemCount, j)
        {
            DsPriorityItem item = {0};
            item.depth = (GMLReal) rdF64(r);
            item.item = rdValue(r, loader);
            arrput(priority.items, item);
        }
        arrput(runner->dsPriorityPool, priority);
    }

    const uint32_t gridCount = rdU32(r);
    repeat((int32_t) gridCount, i)
    {
        DsGrid grid = {0};
        grid.freed = rdU8(r) != 0;
        grid.width = rdI32(r);
        grid.height = rdI32(r);
        const size_t cells = (size_t) grid.width * (size_t) grid.height;
        if (cells > 0)
        {
            grid.items = (RValue*) safeCalloc(cells, sizeof(RValue));
            repeat((int32_t) cells, j)
            {
                grid.items[j] = rdValue(r, loader);
            }
        }
        arrput(runner->dsGridPool, grid);
    }

    const uint32_t bufferCount = rdU32(r);
    repeat((int32_t) bufferCount, i)
    {
        GmlBuffer buffer = {0};
        buffer.isValid = rdU8(r) != 0;
        buffer.size = rdI32(r);
        buffer.position = rdI32(r);
        buffer.usedSize = rdI32(r);
        buffer.alignment = rdI32(r);
        buffer.type = rdI32(r);
        if (buffer.isValid && buffer.size > 0)
        {
            buffer.data = (uint8_t*) safeMalloc((size_t) buffer.size);
            if (!rdBytes(r, buffer.data, (size_t) buffer.size))
            {
                free(buffer.data);
                buffer.data = nullptr;
                buffer.isValid = false;
            }
        }
        arrput(runner->gmlBufferPool, buffer);
    }
}

// Everything after the room index. Kept separate so a load can build the room
// between the two halves.
static void rdScalarsRest(Reader* r, Runner* runner)
{
    runner->pendingRoom = rdI32(r);
    runner->frameCount = rdI32(r);
    runner->nextInstanceId = rdU32(r);
    runner->nextLayerId = rdU32(r);

    runner->score = (GMLReal) rdF64(r);
    runner->lives = (GMLReal) rdF64(r);
    runner->health = (GMLReal) rdF64(r);
    runner->gameSpeedOverride = (GMLReal) rdF64(r);
    runner->deltaTime = (GMLReal) rdF64(r);
    runner->fps = rdF64(r);

    const uint32_t flags = rdU32(r);
    runner->viewsEnabled = (flags & (1u << 0)) != 0;
    runner->drawEnabled = (flags & (1u << 1)) != 0;
    runner->paused = (flags & (1u << 2)) != 0;
    runner->appSurfaceEnabled = (flags & (1u << 3)) != 0;
    runner->appSurfaceAutoDraw = (flags & (1u << 4)) != 0;
    runner->usingAppSurface = (flags & (1u << 5)) != 0;
    runner->gameStartFired = (flags & (1u << 6)) != 0;
    runner->drawBackgroundColor = (flags & (1u << 7)) != 0;
    runner->forceDrawDepth = (flags & (1u << 8)) != 0;
    runner->applyOffsetForPrimitives = (flags & (1u << 9)) != 0;
    runner->inGuiPass = (flags & (1u << 10)) != 0;

    runner->applicationWidth = rdI32(r);
    runner->applicationHeight = rdI32(r);
    runner->oldApplicationWidth = rdI32(r);
    runner->oldApplicationHeight = rdI32(r);
    runner->widescreenExtraWidth = rdI32(r);
    runner->widescreenExtraHeight = rdI32(r);
    runner->guiWidth = rdI32(r);
    runner->guiHeight = rdI32(r);
    runner->backgroundColor = rdU32(r);

    rdBytes(r, &runner->random, sizeof(runner->random));
    rdBytes(r, runner->views, sizeof(runner->views));
    rdBytes(r, runner->defaultCameras, sizeof(runner->defaultCameras));
    rdBytes(r, runner->userCameras, sizeof(runner->userCameras));
    rdBytes(r, runner->backgrounds, sizeof(runner->backgrounds));

    const uint32_t tileLayerCount = rdU32(r);
    repeat((int32_t) tileLayerCount, i)
    {
        const int32_t key = rdI32(r);
        TileLayerState state = {0};
        rdBytes(r, &state, sizeof(TileLayerState));
        hmput(runner->tileLayerMap, key, state);
    }
}

// Restores one instance's own variables. Instance_setSelfVar takes an independent
// reference, so the value read here is released immediately after.
static void rdInstanceVars(Reader* r, Loader* loader, Instance* inst)
{
    const uint32_t count = rdU32(r);
    repeat((int32_t) count, i)
    {
        const int32_t varId = rdI32(r);
        RValue value = rdValue(r, loader);
        if (!r->ok)
        {
            RValue_free(&value);
            return;
        }
        Instance_setSelfVar(inst, varId, value);
        RValue_free(&value);
    }
}

static void rdGlobals(Reader* r, Loader* loader)
{
    VMContext* ctx = loader->runner->vmContext;
    Instance* globalScope = ctx != nullptr ? ctx->globalScopeInstance : nullptr;

    const uint32_t count = rdU32(r);
    repeat((int32_t) count, i)
    {
        const int32_t varId = rdI32(r);
        RValue value = rdValue(r, loader);
        if (!r->ok)
        {
            RValue_free(&value);
            return;
        }
        if (globalScope != nullptr) Instance_setSelfVar(globalScope, varId, value);
        RValue_free(&value);
    }
}

// ===[ Layers ]===
typedef struct
{
    uint32_t id;
    uint32_t type;
    bool visible;
    float alpha;
    uint32_t blend;
    int32_t instanceId;
    bool ownedTile;
    RoomTile tile;
}
SavedLayerElement;

typedef struct
{
    uint32_t id;
    int32_t depth;
    uint32_t drawOrder;
    float xOffset;
    float yOffset;
    float hSpeed;
    float vSpeed;
    bool visible;
    bool automaticDepth;
    int32_t shaderIndex;
    bool dynamic;
    int32_t beginScript;
    int32_t endScript;
    char* dynamicName; // owned
    SavedLayerElement* elements; // stb_ds array
}
SavedLayer;

// Rebuilds one layer's element array from the state: the room supplies the payloads
// for the static elements it just created (backgrounds, sprites, tilemaps and room
// tiles), while instance elements and layer_tile_create tiles are rebuilt from the
// file. Ids and order both come from the state, so stored ids keep resolving and the
// in-layer draw order matches the frame the state was taken in.
static void applySavedLayer(Runner* runner, const SavedLayer* saved)
{
    RuntimeLayer* layer = Runner_findRuntimeLayerById(runner, (int32_t) saved->id);
    if (layer == nullptr)
    {
        if (!saved->dynamic)
        {
            logWarn("SaveState: state has layer %u, which this room does not have\n", (unsigned) saved->id);
            return;
        }
        // Recreate what layer_create made: same id, so stored layer ids keep working.
        RuntimeLayer created = {0};
        created.id = saved->id;
        created.depth = saved->depth;
        created.drawOrder = saved->drawOrder;
        created.visible = saved->visible;
        created.dynamic = true;
        created.automaticDepth = saved->automaticDepth;
        created.dynamicName = saved->dynamicName != nullptr ? safeStrdup(saved->dynamicName) : nullptr;
        created.beginScript = saved->beginScript;
        created.endScript = saved->endScript;
        created.shaderIndex = saved->shaderIndex;
        arrput(runner->runtimeLayers, created);
        layer = &arrlast(runner->runtimeLayers);
    }

    // Keep the room's own static elements, in the order the room built them.
    RuntimeLayerElement* statics = nullptr;
    repeat((int32_t) arrlen(layer->elements), i)
    {
        const RuntimeLayerElement el = layer->elements[i];
        if (el.type == RuntimeLayerElementType_Instance) continue;
        arrput(statics, el);
    }
    // Tiles made with layer_tile_create are ours to free; everything else in the
    // array is either borrowed room data or owned elsewhere.
    repeat((int32_t) arrlen(layer->elements), i)
    {
        RuntimeLayerElement* el = &layer->elements[i];
        if (el->tileElementOwned && el->tileElement != nullptr) free(el->tileElement);
    }
    arrsetlen(layer->elements, 0);

    int32_t nextStatic = 0;
    uint32_t maxId = 0;
    repeat((int32_t) arrlen(saved->elements), i)
    {
        const SavedLayerElement* src = &saved->elements[i];
        RuntimeLayerElement el = {0};
        el.id = src->id;
        el.type = (RuntimeLayerElementType) src->type;
        el.visible = src->visible;
        el.alpha = src->alpha;
        el.blend = src->blend;
        el.instanceId = -1;

        if (el.type == RuntimeLayerElementType_Instance)
        {
            // An instance whose record did not make it back has no element either.
            if (hmget(runner->instancesById, src->instanceId) == nullptr) continue;
            el.instanceId = src->instanceId;
        }
        else if (src->ownedTile)
        {
            RoomTile* tile = (RoomTile*) safeMalloc(sizeof(RoomTile));
            memcpy(tile, &src->tile, sizeof(RoomTile));
            el.tileElement = tile;
            el.tileElementOwned = true;
        }
        else
        {
            if (nextStatic >= arrlen(statics)) continue;
            const RuntimeLayerElement fresh = statics[nextStatic];
            if (fresh.type != el.type)
            {
                // The room no longer has the element at this position (the game must
                // have created or destroyed one); leave it unmatched rather than
                // handing over a payload of the wrong type.
                continue;
            }
            nextStatic++;
            el.backgroundElement = fresh.backgroundElement;
            el.spriteElement = fresh.spriteElement;
            el.tilemapData = fresh.tilemapData;
            el.tileElement = fresh.tileElement;
            el.tileElementOwned = fresh.tileElementOwned;
            if (el.id == 0) el.id = fresh.id;
        }

        if (el.id > maxId) maxId = el.id;
        arrput(layer->elements, el);
    }

    // Whatever the state did not name keeps the id the room gave it.
    for (int32_t i = nextStatic; i < (int32_t) arrlen(statics); i++)
    {
        if (statics[i].id > maxId) maxId = statics[i].id;
        arrput(layer->elements, statics[i]);
    }
    arrfree(statics);

    layer->depth = saved->depth;
    layer->drawOrder = saved->drawOrder;
    layer->xOffset = saved->xOffset;
    layer->yOffset = saved->yOffset;
    layer->hSpeed = saved->hSpeed;
    layer->vSpeed = saved->vSpeed;
    layer->visible = saved->visible;
    layer->automaticDepth = saved->automaticDepth;
    layer->shaderIndex = saved->shaderIndex;

    if (maxId >= runner->nextLayerId) runner->nextLayerId = maxId + 1;
}

static void rdLayers(Reader* r, Runner* runner)
{
    runner->nextLayerDrawOrder = rdU32(r);
    const uint32_t layerCount = rdU32(r);
    if (!r->ok || layerCount > 100000u)
    {
        r->ok = false;
        return;
    }

    repeat((int32_t) layerCount, i)
    {
        SavedLayer saved = {0};
        saved.id = rdU32(r);
        saved.depth = rdI32(r);
        saved.drawOrder = rdU32(r);
        saved.xOffset = rdF32(r);
        saved.yOffset = rdF32(r);
        saved.hSpeed = rdF32(r);
        saved.vSpeed = rdF32(r);
        saved.visible = rdU8(r) != 0;
        saved.automaticDepth = rdU8(r) != 0;
        saved.shaderIndex = rdI32(r);
        saved.dynamic = rdU8(r) != 0;
        saved.beginScript = rdI32(r);
        saved.endScript = rdI32(r);
        saved.dynamicName = rdStr(r);

        const uint32_t elementCount = rdU32(r);
        if (!r->ok || elementCount > 1000000u)
        {
            r->ok = false;
            arrfree(saved.elements);
            free(saved.dynamicName);
            return;
        }
        repeat((int32_t) elementCount, j)
        {
            SavedLayerElement el = {0};
            el.id = rdU32(r);
            el.type = rdU8(r);
            el.visible = rdU8(r) != 0;
            el.alpha = rdF32(r);
            el.blend = rdU32(r);
            el.instanceId = rdI32(r);
            el.ownedTile = rdU8(r) != 0;
            if (el.ownedTile) rdBytes(r, &el.tile, sizeof(RoomTile));
            arrput(saved.elements, el);
            if (!r->ok) break;
        }
        if (!r->ok)
        {
            arrfree(saved.elements);
            free(saved.dynamicName);
            return;
        }

        applySavedLayer(runner, &saved);
        arrfree(saved.elements);
        free(saved.dynamicName);
    }
}

bool SaveState_load(Runner* runner, const char* path)
{
    if (runner == nullptr || path == nullptr) return false;

    // Declared before the first goto target so the failure path can always clean up.
    Loader loader = {0};

    FILE* file = fopen(path, "rb");
    if (file == nullptr)
    {
        logWarn("SaveState: no state file at '%s'\n", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    const long length = ftell(file);
    if (length < 16) { fclose(file); return false; }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }

    uint8_t* bytes = (uint8_t*) safeMalloc((size_t) length);
    const size_t read = fread(bytes, 1, (size_t) length, file);
    fclose(file);
    if (read != (size_t) length)
    {
        free(bytes);
        return false;
    }

    Reader r = { .data = bytes, .size = (size_t) length, .position = 0, .ok = true };

    char magic[8];
    rdBytes(&r, magic, sizeof(magic));
    const uint32_t version = rdU32(&r);
    const uint32_t fingerprint = rdU32(&r);

    if (!r.ok || memcmp(magic, SAVE_STATE_MAGIC, sizeof(magic)) != 0)
    {
        logError("SaveState: '%s' is not a save state\n", path);
        free(bytes);
        return false;
    }
    if (version != SAVE_STATE_VERSION)
    {
        logError("SaveState: '%s' was written by another version (%u, expected %u)\n",
                 path, (unsigned) version, (unsigned) SAVE_STATE_VERSION);
        free(bytes);
        return false;
    }
    if (fingerprint != SaveState_gameId(runner->dataWin))
    {
        logError("SaveState: '%s' belongs to a different game\n", path);
        free(bytes);
        return false;
    }

    // Everything checks out, so the live game can be torn down. From here on a
    // truncated file leaves a freshly reset game rather than a half-restored one.
    Runner_reset(runner);

    loader.runner = runner;

    // Room first: build its layers, backgrounds and views, but no instances and no
    // game code, because the instances come from the state.
    const int32_t roomIndex = rdI32(&r);
    if (r.ok && roomIndex >= 0 && (uint32_t) roomIndex < runner->dataWin->room.count)
    {
        Runner_quietLoadRoom(runner, roomIndex);
    }
    else
    {
        logError("SaveState: state names room %d, which this game does not have\n",
                 (int) roomIndex);
        free(bytes);
        return false;
    }

    rdScalarsRest(&r, runner);
    if (!r.ok) goto failed;

    {
        const uint32_t instanceCount = rdU32(&r);
        if (!r.ok || instanceCount > 1000000u) goto failed;

        // Pass 1: every instance record creates its instance. Values can point at
        // instances (a struct value, a method's bound self), so the instances have to
        // exist before the first value is read.
        typedef struct
        {
            uint32_t instanceId;
            int32_t objectIndex;
            int32_t staticParentId;
            bool isStruct;
            Instance* inst;
        }
        PendingInstance;

        PendingInstance* pending = nullptr;
        repeat((int32_t) instanceCount, i)
        {
            PendingInstance entry = {0};
            entry.isStruct = rdU8(&r) != 0;
            entry.instanceId = rdU32(&r);
            entry.objectIndex = rdI32(&r);
            if (!r.ok) break;

            Instance* inst = entry.isStruct
                ? Runner_restoreStruct(runner, entry.instanceId)
                : Runner_restoreInstance(runner, entry.instanceId, entry.objectIndex, 0.0, 0.0);
            if (inst == nullptr)
            {
                logError("SaveState: state refers to object %d, which this game does not have\n",
                         entry.objectIndex);
                r.ok = false;
                break;
            }
            entry.inst = inst;

            uint32_t alarms[GML_ALARM_COUNT] = {0};
            rdInstanceFields(&r, inst, &entry.staticParentId, alarms);
            repeat(GML_ALARM_COUNT, a) inst->alarm[a] = (int32_t) alarms[a];
            arrput(pending, entry);
            if (!r.ok) break;
        }

        if (r.ok)
        {
            // Tables first, values second: an array element can be an array, a method
            // or a struct, and all three have to resolve while it is read.
            rdArrayTable(&r, &loader);
            rdMethodTable(&r, &loader);
            rdArrayValues(&r, &loader);
        }

        if (r.ok)
        {
            // Pass 2: cross-instance pointers, and re-register the restored
            // instances as elements of the layer they were on.
            repeat(arrlen(pending), i)
            {
                Instance* inst = pending[i].inst;
                if (pending[i].staticParentId >= 0)
                    inst->staticParent = hmget(runner->instancesById, pending[i].staticParentId);

                if (!pending[i].isStruct && inst->layer >= 0
                    && Runner_findRuntimeLayerById(runner, inst->layer) != nullptr)
                {
                    Runner_addInstanceLayerElement(runner, inst->layer, inst->instanceId);
                }

                // Instances are normally inserted into the spatial grid by their first
                // movement. A restored instance may never move, so mark it dirty and let
                // the next collision update place it.
                inst->spatialGridDirty = true;
            }

            // Pass 3: variables. All arrays, methods and instances exist by now.
            repeat(arrlen(pending), i)
            {
                rdInstanceVars(&r, &loader, pending[i].inst);
                if (!r.ok) break;
            }
        }

        runner->drawableListStructureDirty = true;
        arrfree(pending);
    }
    if (!r.ok) goto failed;

    rdGlobals(&r, &loader);
    if (!r.ok) goto failed;
    rdDsPools(&r, &loader);
    if (!r.ok) goto failed;

    // Layers last: the elements point at instances, and the rebuilt arrays and
    // restored ids change what the draw list has to contain.
    rdLayers(&r, runner);
    if (!r.ok) goto failed;
    runner->drawableListStructureDirty = true;
    runner->drawableListSortDirty = true;

failed:
    free(bytes);

    // Arrays and methods are created with a creator reference of zero while loading,
    // so an entry nothing points at has no owner. Those are deliberately left alone:
    // an unreferenced array can still hold another table entry, so releasing it here
    // would free a child this sweep has not reached yet and the table would then be
    // walking dangling pointers. A state that leaves entries unreferenced is already
    // inconsistent, and a handful of tiny leaks beats corrupting the heap mid-load.
    int32_t orphanedArrays = 0;
    repeat(arrlen(loader.arrays), i)
    {
        if (loader.arrays[i]->refCount == 0) orphanedArrays++;
    }
    arrfree(loader.arrays);

    int32_t orphanedMethods = 0;
    repeat(arrlen(loader.methods), i)
    {
        if (loader.methods[i]->refCount == 0) orphanedMethods++;
    }
    arrfree(loader.methods);

    if (orphanedArrays > 0 || orphanedMethods > 0)
    {
        logWarn("SaveState: '%s' left %d arrays and %d methods unreferenced\n",
                path, (int) orphanedArrays, (int) orphanedMethods);
    }

    if (!r.ok)
    {
        logError("SaveState: '%s' could not be fully restored\n", path);
        return false;
    }

    logInfo("SaveState: loaded room %d, %d instances, %d globals\n",
            runner->currentRoomIndex, (int) arrlen(runner->instances),
            (int) (runner->vmContext != nullptr && runner->vmContext->globalScopeInstance != nullptr
                   ? runner->vmContext->globalScopeInstance->selfVars.count : 0u));
    return true;
}