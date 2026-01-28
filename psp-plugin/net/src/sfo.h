/**
 * SFO (System File Object) Parser
 * 
 * Parses PARAM.SFO files to extract game metadata.
 */

#ifndef SFO_H
#define SFO_H

#include <stdint.h>

/* Maximum value sizes */
#define SFO_MAX_TITLE_LENGTH 128
#define SFO_MAX_ID_LENGTH    10

/**
 * Parsed SFO data
 */
typedef struct {
    char title[SFO_MAX_TITLE_LENGTH];   /* TITLE */
    char disc_id[SFO_MAX_ID_LENGTH];    /* DISC_ID */
    char title_id[SFO_MAX_ID_LENGTH];   /* TITLE_ID */
    uint32_t category;                   /* CATEGORY value */
} SfoData;

/**
 * Parse an SFO file
 * 
 * @param path Path to PARAM.SFO file
 * @param data Output parsed data
 * @return 0 on success, negative on error
 */
int sfo_parse_file(const char *path, SfoData *data);

/**
 * Parse SFO data from memory
 * 
 * @param buffer SFO file contents
 * @param size Buffer size
 * @param data Output parsed data
 * @return 0 on success, negative on error
 */
int sfo_parse_buffer(const uint8_t *buffer, uint32_t size, SfoData *data);

#endif /* SFO_H */
