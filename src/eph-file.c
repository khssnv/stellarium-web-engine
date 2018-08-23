/* Stellarium Web Engine - Copyright (c) 2018 - Noctua Software Ltd
 *
 * This program is licensed under the terms of the GNU AGPL v3, or
 * alternatively under a commercial licence.
 *
 * The terms of the AGPL v3 license can be found in the main directory of this
 * repository.
 */

#include "eph-file.h"
#include "swe.h"
#include "zlib.h"

#include <assert.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdio.h>

/* The stars tile file format is as follow:
 *
 * 4 bytes magic string:    "EPHE"
 * 4 bytes file version:    <FILE_VERSION>
 * List of chunks
 *
 * chunk:
 *   4 bytes: type
 *   4 bytes: data len
 *   4 bytes: data
 *   4 bytes: CRC
 *
 * It's then up to the caller to parse the chunks data.  We add some helper
 * functions to parse common structures:
 *
 * Tile header:
 *   4 bytes: version
 *   8 bytes: nuniq hips tile pos
 *
 * Compressed data block:
 *   4 bytes: data size
 *   4 bytes: compressed data size
 *   n bytes: compressed data
 *
 * Tabular data:
 *   4 bytes: flags (1: data is shuffled)
 *   4 bytes: row size in bytes
 *   4 bytes: columns number
 *   4 bytes: row number
 *   Then for each column:
 *     4 bytes: id string
 *     4 bytes: type ('f', 'i', 'Q', 's')
 *     4 bytes: unit (one of EPH_UNIT value, e.g EPH_RAD or 0 to ignore)
 *     4 bytes: start offset in bytes
 *     4 bytes: data size
 */

#define FILE_VERSION 2

int eph_read_tile_header(const void *data, int data_size, int *data_ofs,
                         int *version, int *order, int *pix)
{
    uint64_t nuniq;
    data += *data_ofs;
    memcpy(version, data, 4);
    memcpy(&nuniq, data + 4, 8);
    *order = log2(nuniq / 4) / 2;
    *pix = nuniq - 4 * (1 << (2 * (*order)));
    *data_ofs += 12;
    return 0;
}

void *eph_read_compressed_block(const void *data, int data_size,
                                int *data_ofs, int *size)
{
    int comp_size;
    void *ret;
    unsigned long lsize;
    data += *data_ofs;
    memcpy(size, data, 4);
    memcpy(&comp_size, data + 4, 4);
    lsize = *size;
    ret = malloc(lsize);
    *data_ofs += 8 + comp_size;
    if (uncompress(ret, &lsize, data + 8, comp_size) != Z_OK) {
        LOG_E("Cannot uncompress data");
        return NULL;
    }
    return ret;
}

int eph_load(const void *data, int data_size, void *user,
             int (*callback)(const char type[4],
                             const void *data, int size, void *user))
{
    int version, chunk_data_size;
    char type[4];

    assert(data);
    CHECK(data_size >= 4);
    CHECK(strncmp(data, "EPHE", 4) == 0);
    memcpy(&version, data + 4, 4);
    data += 8; data_size -= 8;

    CHECK(version == FILE_VERSION);
    while (data_size) {
        CHECK(data_size >= 8);
        memcpy(type, data, 4);
        memcpy(&chunk_data_size, data + 4, 4);
        callback(type, data + 8, chunk_data_size, user);
        data += chunk_data_size + 12;
        data_size -= chunk_data_size + 12;
    }
    return 0;
}

// In place shuffle of the data bytes for optimized compression.
static void shuffle_bytes(uint8_t *data, int nb, int size)
{
    int i, j;
    uint8_t *buf = calloc(nb, size);
    memcpy(buf, data, nb * size);
    for (j = 0; j < size; j++) {
        for (i = 0; i < nb; i++) {
            data[j * nb + i] = buf[i * size + j];
        }
    }
    free(buf);
}

int eph_read_table_prepare(int version, void *data, int data_size,
                           int *data_ofs, int row_size,
                           int nb_columns, eph_table_column_t *columns)
{
    int i, j, start = 0, flags, n_col, n_row;
    char name[4], type[4];

    assert(*data_ofs == 0);
    data += *data_ofs;

    // Old style with no header support.
    // To remove as soon as all the eph file switch to the new format.
    if (version < 3) {
        if (row_size != 104) // Hack to handle DSO non shuffle.
            shuffle_bytes(data, row_size, data_size / row_size);
        for (i = 0; i < nb_columns; i++) {
            columns[i].row_size = row_size;
            columns[i].start = start;
            columns[i].src_unit = columns[i].unit;
            if (!columns[i].size) {
                switch (columns[i].type) {
                case 'i':
                case 'f': columns[i].size = 4; break;
                case 'Q': columns[i].size = 8; break;
                }
            }
            start += columns[i].size;
        }
        return data_size / row_size;
    }

    memcpy(&flags,    data + 0 , 4);
    memcpy(&row_size, data + 4 , 4);
    memcpy(&n_col,    data + 8 , 4);
    memcpy(&n_row,    data + 12, 4);

    for (i = 0; i < n_col; i++) {
        memcpy(name, data + 16 + i * 20, 4);
        memcpy(type, data + 20 + i * 20, 4);
        for (j = 0; j < nb_columns; j++) {
            if (strncmp(columns[j].name, name, 4) == 0) break;
        }
        if (j == nb_columns) continue;
        if (columns[j].type != *type) {
            LOG_E("Wrong type");
            return -1;
        }
        columns[j].row_size = row_size;
        memcpy(&columns[j].src_unit, data + 24 + i * 20, 4);
        memcpy(&columns[j].start, data + 28 + i * 20, 4);
        memcpy(&columns[j].size, data + 32 + i * 20, 4);
    }

    // Check that all the columns have been found.
    for (i = 0; i < nb_columns; i++) {
        if (!columns[i].row_size) {
            LOG_E("Cannot find column %.4s", columns[i].name);
            return -1;
        }
    }

    if (flags & 1)
        shuffle_bytes(data + 16 + n_col * 20, row_size, n_row);

    *data_ofs += 16 + n_col * 20;
    return n_row;

}

double eph_convert_f(int src_unit, int unit, double v)
{
    if (!unit || src_unit == unit) return v; // Most common case.

    // 1 -> deg to rad
    if ( (src_unit & 1) && !(unit & 1)) v *= DD2R;
    if (!(src_unit & 1) &&  (unit & 1)) v *= DR2D;
    // 2 -> 1/60
    if ( (src_unit & 2) && !(unit & 2)) v /= 60;
    if (!(src_unit & 2) &&  (unit & 2)) v *= 60;
    // 4 -> 1/60
    if ( (src_unit & 4) && !(unit & 4)) v /= 60;
    if (!(src_unit & 4) &&  (unit & 4)) v *= 60;
    // 8 -> 365.25
    if ( (src_unit & 8) && !(unit & 8)) v *= 365.25;
    if (!(src_unit & 8) &&  (unit & 8)) v /= 365.25;

    return v;
}

int eph_read_table_row(const void *data, int data_size, int *data_ofs,
                       int nb_columns, const eph_table_column_t *columns,
                       ...)
{
    int i;
    va_list ap;
    union {
        char    *s;
        int      i;
        float    f;
        uint64_t q;
    } v;

    assert(nb_columns > 0);
    data += *data_ofs;
    va_start(ap, columns);
    for (i = 0; i < nb_columns; i++) {
        switch (columns[i].type) {
        case 'i':
            memcpy(&v.i, data + columns[i].start, 4);
            *va_arg(ap, int*) = v.i;
            break;
        case 'f':
            memcpy(&v.f, data + columns[i].start, 4);
            *va_arg(ap, double*) = eph_convert_f(
                    columns[i].src_unit, columns[i].unit, v.f);
            break;
        case 'Q':
            memcpy(&v.q, data + columns[i].start, 8);
            *va_arg(ap, uint64_t*) = v.q;
            break;
        case 's':
            memcpy(va_arg(ap, char*), data + columns[i].start,
                   columns[i].size);
            break;
        }
    }
    va_end(ap);
    *data_ofs += columns[0].row_size;
    return 0;
}
