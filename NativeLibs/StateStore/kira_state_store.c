#include "kira_state_store.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char* key;
    void* value;
} kira_state_entry;

static kira_state_entry* entries = NULL;
static int32_t entry_count = 0;
static int32_t entry_capacity = 0;

static int32_t find_entry(const char* key) {
    for (int32_t i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].key, key) == 0) return i;
    }
    return -1;
}

int32_t kira_state_slot_has(const char* key) {
    if (key == NULL) return 0;
    return find_entry(key) >= 0 ? 1 : 0;
}

void* kira_state_slot_get(const char* key) {
    if (key == NULL) return NULL;
    const int32_t index = find_entry(key);
    return index >= 0 ? entries[index].value : NULL;
}

void kira_state_slot_put(const char* key, void* value) {
    if (key == NULL) return;
    const int32_t index = find_entry(key);
    if (index >= 0) {
        entries[index].value = value;
        return;
    }
    if (entry_count == entry_capacity) {
        const int32_t next_capacity = entry_capacity == 0 ? 16 : entry_capacity * 2;
        kira_state_entry* next = realloc(entries, (size_t)next_capacity * sizeof(kira_state_entry));
        if (next == NULL) return;
        entries = next;
        entry_capacity = next_capacity;
    }
    entries[entry_count].key = strdup(key);
    entries[entry_count].value = value;
    entry_count++;
}

void kira_state_slot_reset(void) {
    for (int32_t i = 0; i < entry_count; i++) {
        free(entries[i].key);
    }
    entry_count = 0;
}

int32_t kira_state_slot_count(void) {
    return entry_count;
}
