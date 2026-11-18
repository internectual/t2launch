/* YouTube "mesh projection" (ytmp / mshp) video: decode the mesh stored in an MP4's header and unwrap it to an
   equirectangular lookup that ffmpeg's remap filter applies.
   YouTube stores almost every 360 video as a custom mesh (the frame is not equirect: the front hemisphere sits in the
   top half and the rest is packed below it), so playing the frame as an equirect gives a scrambled sky. The format is
   Google's Spherical Video V2 mesh box: raw deflate, then float coordinates, delta/zig-zag coded vertices and index lists
   (https://github.com/google/spatial-media/blob/master/docs/spherical-video-v2-rfc.md).
   Included by t2_sky.h; the pure functions are unit-tested through tests/sky_views_test.c. */

#ifndef T2_YTMESH_H
#define T2_YTMESH_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── a small raw-deflate decoder (RFC 1951), after zlib's puff.c ───── */

struct ytm_inflate {
    const unsigned char *in; size_t in_length, in_position;
    unsigned bit_buffer; int bit_count;
    unsigned char *out; size_t out_length, out_capacity;
    int error;
};

struct ytm_huffman { short count[16]; short symbol[288]; };

static int ytm_bits(struct ytm_inflate *s, int need) {
    long value = s->bit_buffer;
    while (s->bit_count < need) {
        if (s->in_position >= s->in_length) { s->error = 1; return 0; }
        value |= (long)s->in[s->in_position++] << s->bit_count;
        s->bit_count += 8;
    }
    s->bit_buffer = (unsigned)(value >> need);
    s->bit_count -= need;
    return (int)(value & ((1L << need) - 1));
}

static int ytm_construct(struct ytm_huffman *h, const short *length, int n) {
    short offsets[16];
    for (int len = 0; len <= 15; len++) h->count[len] = 0;
    for (int symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offsets[1] = 0;
    for (int len = 1; len < 15; len++) offsets[len + 1] = (short)(offsets[len] + h->count[len]);
    for (int symbol = 0; symbol < n; symbol++) if (length[symbol] != 0) h->symbol[offsets[(int)length[symbol]]++] = (short)symbol;
    return left;
}

static int ytm_decode(struct ytm_inflate *s, const struct ytm_huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= ytm_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (s->error) return -1;
    }
    return -10;
}

static int ytm_put(struct ytm_inflate *s, unsigned char byte) {
    if (s->out_length >= s->out_capacity) {
        size_t capacity = s->out_capacity ? s->out_capacity * 2 : 65536;
        if (capacity > 16u * 1024 * 1024) return 0;
        unsigned char *grown = realloc(s->out, capacity);
        if (!grown) return 0;
        s->out = grown; s->out_capacity = capacity;
    }
    s->out[s->out_length++] = byte;
    return 1;
}

static int ytm_codes(struct ytm_inflate *s, const struct ytm_huffman *lengths, const struct ytm_huffman *distances) {
    static const short base_length[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short extra_length[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short base_distance[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short extra_distance[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int symbol = ytm_decode(s, lengths);
        if (symbol < 0 || s->error) return 1;
        if (symbol < 256) { if (!ytm_put(s, (unsigned char)symbol)) return 1; }
        else if (symbol == 256) return 0;
        else {
            symbol -= 257;
            if (symbol >= 29) return 1;
            int length = base_length[symbol] + ytm_bits(s, extra_length[symbol]);
            int distance_symbol = ytm_decode(s, distances);
            if (distance_symbol < 0 || distance_symbol >= 30) return 1;
            size_t distance = (size_t)base_distance[distance_symbol] + (size_t)ytm_bits(s, extra_distance[distance_symbol]);
            if (s->error || distance > s->out_length) return 1;
            while (length--) if (!ytm_put(s, s->out[s->out_length - distance])) return 1;
        }
    }
}

static int ytm_dynamic(struct ytm_inflate *s) {
    static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[320];
    struct ytm_huffman length_code, distance_code, table;
    int nlen = ytm_bits(s, 5) + 257, ndist = ytm_bits(s, 5) + 1, ncode = ytm_bits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > 30) return 1;
    int index = 0;
    for (; index < ncode; index++) lengths[order[index]] = (short)ytm_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (ytm_construct(&table, lengths, 19) != 0) return 1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = ytm_decode(s, &table);
        if (symbol < 0 || s->error) return 1;
        if (symbol < 16) lengths[index++] = (short)symbol;
        else {
            int previous = 0, repeat;
            if (symbol == 16) { if (index == 0) return 1; previous = lengths[index - 1]; repeat = 3 + ytm_bits(s, 2); }
            else if (symbol == 17) repeat = 3 + ytm_bits(s, 3);
            else repeat = 11 + ytm_bits(s, 7);
            if (index + repeat > nlen + ndist) return 1;
            while (repeat--) lengths[index++] = (short)previous;
        }
    }
    if (lengths[256] == 0) return 1;
    if (ytm_construct(&length_code, lengths, nlen) < 0) return 1;
    if (ytm_construct(&distance_code, lengths + nlen, ndist) < 0) return 1;
    return ytm_codes(s, &length_code, &distance_code);
}

/* Inflate raw deflate data; the caller frees *out. 1 on success. */
static int ytm_inflate_raw(const unsigned char *in, size_t length, unsigned char **out, size_t *out_length) {
    struct ytm_inflate s;
    memset(&s, 0, sizeof s);
    s.in = in; s.in_length = length;
    int last, failed = 0;
    do {
        last = ytm_bits(&s, 1);
        int type = ytm_bits(&s, 2);
        if (s.error) { failed = 1; break; }
        if (type == 0) {
            s.bit_buffer = 0; s.bit_count = 0;
            if (s.in_position + 4 > s.in_length) { failed = 1; break; }
            unsigned stored = s.in[s.in_position] | (unsigned)s.in[s.in_position + 1] << 8;
            s.in_position += 4;
            if (s.in_position + stored > s.in_length) { failed = 1; break; }
            while (stored--) if (!ytm_put(&s, s.in[s.in_position++])) { failed = 1; break; }
        } else if (type == 1) {
            short lengths[320];
            struct ytm_huffman length_code, distance_code;
            int symbol = 0;
            for (; symbol < 144; symbol++) lengths[symbol] = 8;
            for (; symbol < 256; symbol++) lengths[symbol] = 9;
            for (; symbol < 280; symbol++) lengths[symbol] = 7;
            for (; symbol < 288; symbol++) lengths[symbol] = 8;
            ytm_construct(&length_code, lengths, 288);
            for (symbol = 0; symbol < 30; symbol++) lengths[symbol] = 5;
            ytm_construct(&distance_code, lengths, 30);
            failed = ytm_codes(&s, &length_code, &distance_code);
        } else if (type == 2) failed = ytm_dynamic(&s);
        else failed = 1;
    } while (!last && !failed);
    if (failed) { free(s.out); return 0; }
    *out = s.out; *out_length = s.out_length;
    return 1;
}

/* ── the mesh ──────────────────────────────────────────────────────── */

struct ytm_mesh {
    int vertex_count, triangle_count;
    float (*position)[3];      /* direction from the viewer */
    float (*uv)[2];            /* video texture coordinates, origin at the lower left */
    unsigned (*triangle)[3];
    int stereo;                /* st3d stereo_mode: 0 mono, 1 top-bottom, 2 left-right (the mesh covers one eye) */
};

struct ytm_bits_reader { const unsigned char *data; size_t length, bit; int error; };

static unsigned ytm_read(struct ytm_bits_reader *r, int count) {
    unsigned value = 0;
    for (int i = 0; i < count; i++) {
        size_t byte = r->bit >> 3;
        if (byte >= r->length) { r->error = 1; return 0; }
        value = value << 1 | ((r->data[byte] >> (7 - (r->bit & 7))) & 1u);
        r->bit++;
    }
    return value;
}

static void ytm_align(struct ytm_bits_reader *r) { r->bit = (r->bit + 7) & ~(size_t)7; }

static int ytm_zigzag(unsigned value) { return (int)(value >> 1) ^ -(int)(value & 1); }

static int ytm_size_in_bits(unsigned count) {   /* ceil(log2(count * 2)) */
    int bits = 0;
    unsigned long long limit = (unsigned long long)count * 2;
    while (((unsigned long long)1 << bits) < limit) bits++;
    return bits;
}

static void ytm_mesh_free(struct ytm_mesh *mesh) {
    free(mesh->position); free(mesh->uv); free(mesh->triangle);
    memset(mesh, 0, sizeof *mesh);
}

/* Decode the first mesh box of the decompressed mesh projection data. */
static int ytm_mesh_decode(const unsigned char *data, size_t length, struct ytm_mesh *mesh) {
    memset(mesh, 0, sizeof *mesh);
    if (length < 16 || memcmp(data + 4, "mesh", 4) != 0) return 0;
    size_t box = (size_t)data[0] << 24 | (size_t)data[1] << 16 | (size_t)data[2] << 8 | data[3];
    if (box < 16 || box > length) box = length;
    struct ytm_bits_reader r = {data + 8, box - 8, 0, 0};
    unsigned coordinate_count = ytm_read(&r, 32) & 0x7fffffffu;
    if (r.error || coordinate_count == 0 || coordinate_count > 1u << 20 || (size_t)coordinate_count * 4 > r.length) return 0;
    float *coordinate = malloc(coordinate_count * sizeof *coordinate);
    if (!coordinate) return 0;
    for (unsigned i = 0; i < coordinate_count; i++) {
        uint32_t raw = ytm_read(&r, 32);
        memcpy(&coordinate[i], &raw, 4);
    }
    unsigned vertex_count = ytm_read(&r, 32) & 0x7fffffffu;
    if (r.error || vertex_count == 0 || vertex_count > 1u << 20) { free(coordinate); return 0; }
    int ccsb = ytm_size_in_bits(coordinate_count), vcsb = ytm_size_in_bits(vertex_count);
    mesh->position = malloc(vertex_count * sizeof *mesh->position);
    mesh->uv = malloc(vertex_count * sizeof *mesh->uv);
    if (!mesh->position || !mesh->uv) { free(coordinate); ytm_mesh_free(mesh); return 0; }
    int previous[5] = {0, 0, 0, 0, 0};
    for (unsigned v = 0; v < vertex_count && !r.error; v++) {
        float value[5];
        for (int k = 0; k < 5; k++) {
            previous[k] += ytm_zigzag(ytm_read(&r, ccsb));
            if (previous[k] < 0 || (unsigned)previous[k] >= coordinate_count) { r.error = 1; value[k] = 0; }
            else value[k] = coordinate[previous[k]];
        }
        mesh->position[v][0] = value[0]; mesh->position[v][1] = value[1]; mesh->position[v][2] = value[2];
        mesh->uv[v][0] = value[3]; mesh->uv[v][1] = value[4];
    }
    free(coordinate);
    if (r.error) { ytm_mesh_free(mesh); return 0; }
    mesh->vertex_count = (int)vertex_count;
    ytm_align(&r);
    unsigned list_count = ytm_read(&r, 32) & 0x7fffffffu;
    size_t capacity = 0;
    for (unsigned l = 0; l < list_count && !r.error; l++) {
        unsigned texture = ytm_read(&r, 8), type = ytm_read(&r, 8);
        unsigned index_count = ytm_read(&r, 32) & 0x7fffffffu;
        if (r.error || index_count > 1u << 24) break;
        unsigned *index = malloc((index_count ? index_count : 1) * sizeof *index);
        if (!index) { r.error = 1; break; }
        int current = 0;
        for (unsigned i = 0; i < index_count && !r.error; i++) {
            current += ytm_zigzag(ytm_read(&r, vcsb));
            if (current < 0 || (unsigned)current >= vertex_count) r.error = 1;
            index[i] = (unsigned)current;
        }
        ytm_align(&r);
        if (!r.error && texture == 0) {
            size_t more = type == 0 ? index_count / 3 : index_count >= 3 ? index_count - 2 : 0;
            if ((size_t)mesh->triangle_count + more > capacity) {
                capacity = (size_t)mesh->triangle_count + more + 1024;
                unsigned (*grown)[3] = realloc(mesh->triangle, capacity * sizeof *grown);
                if (!grown) { free(index); r.error = 1; break; }
                mesh->triangle = grown;
            }
            for (size_t t = 0; t < more; t++) {
                unsigned a, b, c;
                if (type == 0) { a = index[t * 3]; b = index[t * 3 + 1]; c = index[t * 3 + 2]; }
                else if (type == 1) { a = index[t]; b = index[t + 1]; c = index[t + 2]; }
                else { a = index[0]; b = index[t + 1]; c = index[t + 2]; }
                mesh->triangle[mesh->triangle_count][0] = a; mesh->triangle[mesh->triangle_count][1] = b;
                mesh->triangle[mesh->triangle_count][2] = c; mesh->triangle_count++;
            }
        }
        free(index);
    }
    if (r.error || mesh->triangle_count == 0) { ytm_mesh_free(mesh); return 0; }
    return 1;
}

/* Find the mesh projection box (ytmp, or the standard mshp) in an MP4 header and decode it; 0 when there is none. */
static int ytm_mesh_from_header(const unsigned char *header, size_t length, struct ytm_mesh *mesh) {
    static const char *const types[2] = {"ytmp", "mshp"};
    for (int t = 0; t < 2; t++) {
        for (size_t i = 4; i + 16 <= length; i++) {
            if (memcmp(header + i, types[t], 4) != 0) continue;
            size_t size = (size_t)header[i - 4] << 24 | (size_t)header[i - 3] << 16 | (size_t)header[i - 2] << 8 | header[i - 1];
            if (size < 20 || i - 4 + size > length) continue;
            const unsigned char *body = header + i + 4;   /* version+flags, crc, encoding, data */
            size_t body_length = size - 8;
            if (body_length < 12) continue;
            const unsigned char *encoding = body + 8, *payload = body + 12;
            size_t payload_length = body_length - 12;
            unsigned char *raw = NULL;
            size_t raw_length = 0;
            int ok;
            if (!memcmp(encoding, "dfl8", 4)) ok = ytm_inflate_raw(payload, payload_length, &raw, &raw_length);
            else if (!memcmp(encoding, "raw ", 4)) { raw = malloc(payload_length ? payload_length : 1); ok = raw != NULL; if (ok) { memcpy(raw, payload, payload_length); raw_length = payload_length; } }
            else ok = 0;
            if (ok) {
                ok = ytm_mesh_decode(raw, raw_length, mesh);
                free(raw);
                if (ok) {
                    for (size_t j = 4; j + 9 <= length; j++)   /* st3d box: size, "st3d", version+flags, stereo_mode */
                        if (!memcmp(header + j, "st3d", 4) && header[j - 1] == 13 && header[j + 8] <= 3) { mesh->stereo = header[j + 8]; break; }
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Unwrap the mesh to an equirect lookup texture: for each pixel of an out_width x out_height equirect picture, where in
   the video frame (texture coordinates u across, v down from the top, each 16 bits) the mesh maps that direction.
   Packed RGBA8: r,g = u high,low; b,a = v high,low; all 255 where the mesh has no coverage. The sky shader samples it with
   NEAREST filtering and then samples the video with LINEAR, so the picture is filtered once and costs no CPU.
   Centre of the picture = forward (-Z), +X to the right: what the sky's equirect path expects.
   Each triangle is ray-cast per pixel (a direction d is inside when the solution of d = sum(b_i v_i) has b_i >= 0),
   so seams, the poles and triangles with unequal edges need no special treatment beyond the bounding box. */
static int ytm_render_lookup(const struct ytm_mesh *mesh, int out_width, int out_height, unsigned char *rgba) {
    if (out_width < 16 || out_height < 8) return 0;
    const double pi = 3.14159265358979323846;
    size_t pixels = (size_t)out_width * (size_t)out_height;
    memset(rgba, 255, pixels * 4);
    for (int t = 0; t < mesh->triangle_count; t++) {
        double v[3][3], lon[3], lat[3];
        int degenerate = 0;
        for (int k = 0; k < 3; k++) {
            const float *p = mesh->position[mesh->triangle[t][k]];
            double length = sqrt((double)p[0] * p[0] + (double)p[1] * p[1] + (double)p[2] * p[2]);
            if (length < 1e-9) { degenerate = 1; break; }
            v[k][0] = p[0] / length; v[k][1] = p[1] / length; v[k][2] = p[2] / length;
            lon[k] = atan2(v[k][0], -v[k][2]);
            lat[k] = asin(v[k][1] > 1 ? 1 : v[k][1] < -1 ? -1 : v[k][1]);
        }
        if (degenerate) continue;
        /* inverse of the matrix whose columns are the three directions, by cofactors */
        double m[3][3] = {{v[0][0], v[1][0], v[2][0]}, {v[0][1], v[1][1], v[2][1]}, {v[0][2], v[1][2], v[2][2]}}, inverse[3][3];
        double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                             m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (fabs(determinant) < 1e-12) continue;
        inverse[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / determinant; inverse[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / determinant;
        inverse[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / determinant; inverse[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / determinant;
        inverse[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / determinant; inverse[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / determinant;
        inverse[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / determinant; inverse[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / determinant;
        inverse[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / determinant;
        /* does the triangle cover a pole? then it spans every longitude */
        int pole = 0;
        for (int sign = -1; sign <= 1; sign += 2) {
            double b[3], total = 0;
            int inside = 1;
            for (int k = 0; k < 3; k++) { b[k] = inverse[k][1] * sign; total += b[k]; }
            for (int k = 0; k < 3; k++) if (b[k] < -1e-9 || total <= 0) inside = 0;
            if (inside) pole = sign;
        }
        double lon_min = lon[0], lon_max = lon[0], lat_min = lat[0], lat_max = lat[0];
        for (int k = 1; k < 3; k++) {
            if (lon[k] < lon_min) lon_min = lon[k];
            if (lon[k] > lon_max) lon_max = lon[k];
            if (lat[k] < lat_min) lat_min = lat[k];
            if (lat[k] > lat_max) lat_max = lat[k];
        }
        int all_columns = pole != 0;
        for (int k = 0; k < 3; k++) {   /* a vertex exactly at a pole has no longitude: the triangle fans over every column */
            if (v[k][1] > 0.99995) { all_columns = 1; if (!pole) pole = 1; }
            else if (v[k][1] < -0.99995) { all_columns = 1; if (!pole) pole = -1; }
        }
        if (!all_columns && lon_max - lon_min > pi) {   /* crosses the seam: shift the western longitudes up */
            lon_min = 1e9; lon_max = -1e9;
            for (int k = 0; k < 3; k++) { double l = lon[k] < 0 ? lon[k] + 2 * pi : lon[k]; if (l < lon_min) lon_min = l; if (l > lon_max) lon_max = l; }
        }
        /* an edge is a great-circle arc: it bulges toward the nearer pole beyond its end points, so measure along it */
        for (int k = 0; k < 3; k++) {
            const double *from = v[k], *to = v[(k + 1) % 3];
            for (int step = 1; step <= 3; step++) {
                double f = step / 4.0, q[3], length = 0;
                for (int c = 0; c < 3; c++) { q[c] = from[c] * (1 - f) + to[c] * f; length += q[c] * q[c]; }
                length = sqrt(length);
                if (length < 1e-9) continue;
                double edge_lat = asin(q[1] / length > 1 ? 1 : q[1] / length < -1 ? -1 : q[1] / length);
                if (edge_lat < lat_min) lat_min = edge_lat;
                if (edge_lat > lat_max) lat_max = edge_lat;
            }
        }
        if (pole > 0) lat_max = pi / 2; else if (pole < 0) lat_min = -pi / 2;
        int row_from = (int)floor((0.5 - lat_max / pi) * out_height) - 2, row_to = (int)ceil((0.5 - lat_min / pi) * out_height) + 2;
        int col_from = (int)floor((lon_min / (2 * pi) + 0.5) * out_width) - 2, col_to = (int)ceil((lon_max / (2 * pi) + 0.5) * out_width) + 2;
        if (all_columns || col_to - col_from >= out_width) { col_from = 0; col_to = out_width - 1; }
        if (row_from < 0) row_from = 0;
        if (row_to >= out_height) row_to = out_height - 1;
        const float *uv0 = mesh->uv[mesh->triangle[t][0]], *uv1 = mesh->uv[mesh->triangle[t][1]], *uv2 = mesh->uv[mesh->triangle[t][2]];
        for (int row = row_from; row <= row_to; row++) {
            double phi = (0.5 - (row + 0.5) / out_height) * pi, cos_phi = cos(phi), sin_phi = sin(phi);
            for (int column = col_from; column <= col_to; column++) {
                int wrapped = ((column % out_width) + out_width) % out_width;
                double theta = ((wrapped + 0.5) / out_width - 0.5) * 2 * pi;
                double d[3] = {cos_phi * sin(theta), sin_phi, -cos_phi * cos(theta)};
                double b0 = inverse[0][0] * d[0] + inverse[0][1] * d[1] + inverse[0][2] * d[2];
                double b1 = inverse[1][0] * d[0] + inverse[1][1] * d[1] + inverse[1][2] * d[2];
                double b2 = inverse[2][0] * d[0] + inverse[2][1] * d[1] + inverse[2][2] * d[2];
                double total = b0 + b1 + b2;
                if (total <= 0 || b0 < -1e-6 || b1 < -1e-6 || b2 < -1e-6) continue;
                double u = (b0 * uv0[0] + b1 * uv1[0] + b2 * uv2[0]) / total;
                double w = (b0 * uv0[1] + b1 * uv1[1] + b2 * uv2[1]) / total;
                if (mesh->stereo == 2) u *= 0.5;                 /* left eye of a side-by-side frame */
                else if (mesh->stereo == 1) w = w * 0.5 + 0.5;   /* left eye of a top-bottom frame (v runs up) */
                double v = 1.0 - w;
                u = u < 0 ? 0 : u > 1 ? 1 : u; v = v < 0 ? 0 : v > 1 ? 1 : v;
                unsigned uu = (unsigned)(u * 65534.0 + 0.5), vv = (unsigned)(v * 65534.0 + 0.5);
                unsigned char *texel = rgba + ((size_t)row * (size_t)out_width + (size_t)wrapped) * 4;
                texel[0] = (unsigned char)(uu >> 8); texel[1] = (unsigned char)(uu & 255);
                texel[2] = (unsigned char)(vv >> 8); texel[3] = (unsigned char)(vv & 255);
            }
        }
    }
    return 1;
}


#endif
