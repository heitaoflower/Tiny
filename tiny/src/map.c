#include "common.h"
#include "map.h"

void Tiny_InitMap(Tiny_Map* map, Tiny_Context* ctx)
{
    map->ctx = ctx;

    map->cap = 0;
    map->used = 0;

    map->keys = NULL;
    map->values = NULL;
}

static void MapGrow(Tiny_Map* map, size_t newCap)
{
    if(newCap < 16) {
        newCap = 16;
    }

    Map newMap = {
        map->ctx,

        newCap, 0,

        TMalloc(map->ctx, sizeof(uint64_t) * newCap),
        TMalloc(map->ctx, sizeof(void*) * newCap)
    };
    
    memset(newMap.keys, 0, sizeof(uint64_t) * newCap);
    memset(newMap.values, 0, sizeof(void*) * newCap);

    for(size_t i = 0; i < map->cap; ++i) {
        if(map->keys[i]) {
            MapInsert(&newMap, map->keys[i], map->values[i]);
        }
    }

    DestroyMap(map);

    *map = newMap;
}

static void MapInsert(Map* map, uint64_t key, void* value)
{
    assert(key);

    if(map->used * 2 >= map->cap) {
        MapGrow(map, map->cap * 2);
    }

    size_t i = (size_t)HashUint64(key);    

    while(true) {
        i %= map->cap;

        if(!map->keys[i]) {
            map->keys[i] = key;
            map->values[i] = value;
            map->used++;
            return;
        } else if(map->keys[i] == key) {
            map->values[i] = value;
            return;
        }

        i += 1;
    }
}

void* Tiny_MapGet(Map* map, uint64_t key)
{
    if(map->used == 0) {
        return NULL;
    }

    size_t i = HashUint64(key);    

    while(true) {
        i %= map->cap;
        if(map->keys[i] == key) {
            return map->values[i];
        } else if(!map->keys[i]) {
            return NULL;
        }

        i += 1;
    }

    return NULL;
}

// Returns the removed value
void* Tiny_MapRemove(Map* map, uint64_t key)
{
    if(map->used == 0) {
        return;
    }

    size_t i = HashUint64(key);

    while(true) {
        i %= map->cap;
        if(map->keys[i] == key) {
            map->keys[i] = 0;
            return map->values[i];
        } else if(!map->keys[i]) {
            return NULL;
        }

        i += 1;
    }

    return NULL;
}

void Tiny_DestroyMap(Map* map)
{
    TFree(map->ctx, map->keys);
    TFree(map->ctx, map->values);
}
