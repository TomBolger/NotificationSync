#include "system_dark_mode.h"

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO) || defined(PBL_PLATFORM_FLINT)
#define SYSTEM_DARK_MODE_SUPPORTED 1

// Positions in the firmware's function table (SDK revision 113, PebbleOS 4.45), in bytes (4 per function).
// Worked out with PebbleOS's tools/generate_native_sdk ordering, which reproduces all 669 entries of the 4.44
// SDK this app is built with.
#define OFFSET_DARK_MODE_SERVICE_PEEK 0xab0
#define OFFSET_DARK_MODE_SERVICE_SUBSCRIBE 0xab4
#define OFFSET_DARK_MODE_SERVICE_UNSUBSCRIBE 0xab8

#define STUB_PUSH 0xb40f      // push {r0, r1, r2, r3}, first instruction of every libpebble.a stub
#define JUMP_ADR_R3 0xa303    // add r3, pc, #12, first instruction of jump_to_pbl_function
#define JUMP_TABLE_WORD 0x10  // where jump_to_pbl_function keeps the firmware table's address

/**
 * Finds the firmware's function table the way the SDK's stubs do. Every libpebble.a function is
 * `push; mov r1, #offset; b.w jump_to_pbl_function`, and jump_to_pbl_function reads the table address the
 * loader stored next to it. That helper isn't a public symbol, so it's found by following one stub's branch.
 * Any instruction that isn't exactly what the 4.44 SDK produces makes this give up (returns NULL).
 */
static void* const* firmware_table(void)
{
    const uint16_t* stub = (const uint16_t*)((uintptr_t)&watch_info_get_firmware_version & ~(uintptr_t)1);
    if (stub[0] != STUB_PUSH)
    {
        return NULL;
    }

    // b.w (Thumb-2 encoding T4) at stub + 6
    const uint16_t first = stub[3];
    const uint16_t second = stub[4];
    if ((first & 0xf800) != 0xf000 || (second & 0xd000) != 0x9000)
    {
        return NULL;
    }
    const uint32_t s = (first >> 10) & 1;
    const uint32_t i1 = ~(((second >> 13) & 1) ^ s) & 1;
    const uint32_t i2 = ~(((second >> 11) & 1) ^ s) & 1;
    uint32_t offset = (s << 24) | (i1 << 23) | (i2 << 22) | ((uint32_t)(first & 0x3ff) << 12) |
                      ((uint32_t)(second & 0x7ff) << 1);
    if (s)
    {
        offset |= 0xfe000000;  // sign-extend from 25 bits
    }
    const uint8_t* jump = (const uint8_t*)stub + 6 + 4 + (int32_t)offset;

    if (*(const uint16_t*)jump != JUMP_ADR_R3)
    {
        return NULL;
    }
    return *(void* const* const*)(jump + JUMP_TABLE_WORD);
}

static void* firmware_function(const uint32_t offset)
{
    void* const* table = firmware_table();
    return table != NULL ? table[offset / sizeof(void*)] : NULL;
}
#endif

bool system_dark_mode_available(void)
{
#ifdef SYSTEM_DARK_MODE_SUPPORTED
    static int8_t available = -1;
    if (available < 0)
    {
        // Older firmware's table ends before these entries: reading past it would crash.
        const WatchInfoVersion version = watch_info_get_firmware_version();
        const bool new_enough = version.major > 4 || (version.major == 4 && version.minor >= 45);
        available = new_enough && firmware_function(OFFSET_DARK_MODE_SERVICE_PEEK) != NULL &&
                    firmware_function(OFFSET_DARK_MODE_SERVICE_SUBSCRIBE) != NULL &&
                    firmware_function(OFFSET_DARK_MODE_SERVICE_UNSUBSCRIBE) != NULL;
    }
    return available;
#else
    return false;
#endif
}

bool system_dark_mode_peek(void)
{
#ifdef SYSTEM_DARK_MODE_SUPPORTED
    if (system_dark_mode_available())
    {
        return ((bool (*)(void))firmware_function(OFFSET_DARK_MODE_SERVICE_PEEK))();
    }
#endif
    return false;
}

void system_dark_mode_subscribe(void (*handler)(bool is_dark))
{
#ifdef SYSTEM_DARK_MODE_SUPPORTED
    if (system_dark_mode_available())
    {
        ((void (*)(void (*)(bool)))firmware_function(OFFSET_DARK_MODE_SERVICE_SUBSCRIBE))(handler);
    }
#else
    (void)handler;
#endif
}

void system_dark_mode_unsubscribe(void)
{
#ifdef SYSTEM_DARK_MODE_SUPPORTED
    if (system_dark_mode_available())
    {
        ((void (*)(void))firmware_function(OFFSET_DARK_MODE_SERVICE_UNSUBSCRIBE))();
    }
#endif
}
