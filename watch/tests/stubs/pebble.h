#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define PBL_PLATFORM_TYPE_CURRENT 0
#define PBL_PLATFORM_SWITCH(platform,a,b,c,d,e,f,g) e
#define E_DOES_NOT_EXIST -1
typedef struct AppTimer { void (*callback)(void*); } AppTimer;
typedef int AppMessageResult;
typedef struct DictionaryIterator DictionaryIterator;
AppTimer* app_timer_register(uint32_t, void (*)(void*), void*);
void app_timer_cancel(AppTimer*);
int persist_read_data(uint32_t, void*, size_t);
int persist_write_data(uint32_t, const void*, size_t);
int persist_delete(uint32_t);
