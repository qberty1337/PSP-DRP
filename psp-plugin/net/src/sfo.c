/**
 * SFO Parser Implementation
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdlib.h>

#include "sfo.h"

/* SFO file format structures */
typedef struct {
    uint32_t magic;          /* 0x46535000 ("\0PSF") */
    uint32_t version;        /* Usually 0x00000101 */
    uint32_t key_offset;     /* Offset to key table */
    uint32_t val_offset;     /* Offset to value table */
    uint32_t count;          /* Number of entries */
} __attribute__((packed)) SfoHeader;

typedef struct {
    uint16_t key_offset;     /* Offset in key table */
    uint16_t alignment;      /* Data alignment */
    uint32_t val_length;     /* Value length (used bytes) */
    uint32_t val_size;       /* Value size (total bytes) */
    uint32_t val_offset;     /* Offset in value table */
} __attribute__((packed)) SfoEntry;

#define SFO_MAGIC 0x46535000

/**
 * Parse SFO data from memory
 */
int sfo_parse_buffer(const uint8_t *buffer, uint32_t size, SfoData *data) {
    const SfoHeader *header;
    const SfoEntry *entry;
    const char *key_table;
    const uint8_t *val_table;
    uint32_t i;
    
    if (buffer == NULL || data == NULL || size < sizeof(SfoHeader)) {
        return -1;
    }
    
    memset(data, 0, sizeof(SfoData));
    
    header = (const SfoHeader *)buffer;
    
    /* Check magic */
    if (header->magic != SFO_MAGIC) {
        return -2;
    }
    
    /* Validate offsets */
    if (header->key_offset >= size || header->val_offset >= size) {
        return -3;
    }
    
    key_table = (const char *)(buffer + header->key_offset);
    val_table = buffer + header->val_offset;
    
    /* Parse each entry */
    entry = (const SfoEntry *)(buffer + sizeof(SfoHeader));
    
    for (i = 0; i < header->count; i++) {
        const char *key = key_table + entry[i].key_offset;
        const uint8_t *value = val_table + entry[i].val_offset;
        
        /* Look for TITLE */
        if (strcmp(key, "TITLE") == 0) {
            uint32_t len = entry[i].val_length;
            if (len > SFO_MAX_TITLE_LENGTH - 1) {
                len = SFO_MAX_TITLE_LENGTH - 1;
            }
            memcpy(data->title, value, len);
            data->title[len] = '\0';
        }
        /* Look for DISC_ID */
        else if (strcmp(key, "DISC_ID") == 0) {
            uint32_t len = entry[i].val_length;
            if (len > SFO_MAX_ID_LENGTH - 1) {
                len = SFO_MAX_ID_LENGTH - 1;
            }
            memcpy(data->disc_id, value, len);
            data->disc_id[len] = '\0';
        }
        /* Look for TITLE_ID */
        else if (strcmp(key, "TITLE_ID") == 0) {
            uint32_t len = entry[i].val_length;
            if (len > SFO_MAX_ID_LENGTH - 1) {
                len = SFO_MAX_ID_LENGTH - 1;
            }
            memcpy(data->title_id, value, len);
            data->title_id[len] = '\0';
        }
        /* Look for CATEGORY */
        else if (strcmp(key, "CATEGORY") == 0) {
            /* CATEGORY is typically a 2-byte string like "UG", "MG", etc. */
            if (entry[i].val_length >= 2) {
                data->category = (value[0] << 8) | value[1];
            }
        }
    }
    
    return 0;
}

/**
 * Parse an SFO file
 */
int sfo_parse_file(const char *path, SfoData *data) {
    SceUID fd;
    SceIoStat stat;
    uint8_t *buffer = NULL;
    int result = -1;
    
    if (path == NULL || data == NULL) {
        return -1;
    }
    
    /* Get file size */
    if (sceIoGetstat(path, &stat) < 0) {
        return -1;
    }
    
    /* Sanity check size */
    if (stat.st_size < sizeof(SfoHeader) || stat.st_size > 64 * 1024) {
        return -1;
    }
    
    /* Allocate buffer */
    buffer = (uint8_t *)malloc(stat.st_size);
    if (buffer == NULL) {
        return -1;
    }
    
    /* Open and read file */
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) {
        goto cleanup;
    }
    
    if (sceIoRead(fd, buffer, stat.st_size) != stat.st_size) {
        sceIoClose(fd);
        goto cleanup;
    }
    
    sceIoClose(fd);
    
    /* Parse the buffer */
    result = sfo_parse_buffer(buffer, stat.st_size, data);
    
cleanup:
    if (buffer) {
        free(buffer);
    }
    
    return result;
}
