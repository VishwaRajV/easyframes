#include "ef.h"
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <arpa/inet.h>

hdr_t *hdr_tmpls[HDR_TMPL_SIZE];

void hexdump(void *_d, int s) {
    int i;
    uint8_t *d = (uint8_t *)_d;
    uint8_t *e = d + s;

    while (d != e) {
        printf("%08tx: ", (void *)d - (void *)_d);
        for (i = 0; i < 16 && d != e; ++i, ++d)
            printf("%02hhx ", *d);
        printf("\n");
    }
}

typedef void (*destruct_cb_t)(void *buf);

void destruct_free(void *buf, void *cb_) {
    destruct_cb_t cb = (destruct_cb_t)cb_;
    if (!buf)
        return;

    cb(buf);
    free(buf);
}


void field_destruct(field_t *f) {
    if (!f)
        return;

    if (f->def)
        bfree(f->def);

    if (f->val)
        bfree(f->val);

    memset(f, 0, sizeof(*f));
};

int field_copy(field_t *dst, const field_t *src) {
    memcpy(dst, src, sizeof(*src));
    dst->val = bclone(src->val);
    dst->def = bclone(src->def);

    // TODO, handle error

    return 0;
}

void hdr_destruct(hdr_t *h) {
    int i;

    if (!h)
        return;

    for (i = 0; i < h->fields_size; ++i) {
        field_destruct(&(h->fields[i]));
    }

    if (h->fields)
        free(h->fields);

    memset(h, 0, sizeof(*h));
}

int hdr_copy(hdr_t *dst, const hdr_t *src) {
    int i;
    memcpy(dst, src, sizeof(*src));

    if (src->fields_size && src->fields) {
        dst->fields = malloc(src->fields_size * sizeof(field_t));
        if (!dst->fields)
            return -1;
    }

    for (i = 0; i < src->fields_size; ++i) {
        field_copy(&dst->fields[i], &src->fields[i]);
        // TODO, handle error
    }

    return 0;
}

void frame_destruct(frame_t *f) {
    int i;

    for (i = 0; i < f->stack_size; ++i) {
        hdr_free(f->stack[i]);
    }

    memset(f, 0, sizeof(*f));
}

int frame_copy(frame_t *dst, const frame_t *src) {
    int i;
    memcpy(dst, src, sizeof(*src));

    for (i = 0; i < src->stack_size; ++i) {
        dst->stack[i] = hdr_clone(src->stack[i]);

        // TODO, handle error
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////

// ipv6
// tcp
// icmp
// icmpv6
// dhcp (maybe)
// ifh (jr2, ocelot, maybe-other)

uint16_t inet_chksum(uint32_t sum, const uint16_t *buf, int length) {
    while (length > 1) {
        sum += *buf++;
        length -= 2;
    }

    if (length == 1) {
        uint16_t tmp = *(uint8_t *)buf;
#ifdef __BIG_ENDIAN__
        tmp <<= 8;
#endif
        sum += tmp;
    }

    sum = ~((sum >> 16) + (sum & 0xffff));
    sum &= 0xffff;

    return htons(sum);
}

///////////////////////////////////////////////////////////////////////////////
int ether_type_fill_defaults(struct frame *f, int stack_idx) {
    char buf[16];
    hdr_t *h = f->stack[stack_idx];
    field_t *et = find_field(h, "et");

    if (et->val)
        return 0;

    if (stack_idx + 1 < f->stack_size) {
        snprintf(buf, 16, "%d", f->stack[stack_idx + 1]->type);
        buf[15] = 0;

        et->val = parse_bytes(buf, 2);
    }

    return 0;
}

void def_offset(hdr_t *h) {
    int i;
    int offset = 0;

    for (i = 0; i < h->fields_size; ++i) {
        h->fields[i].bit_offset = offset;
        offset += h->fields[i].bit_width;
    }

    h->size = BIT_TO_BYTE(offset);
}

field_t *find_field(hdr_t *h, const char *field) {
    int i;

    for (i = 0; i < h->fields_size; ++i)
        if (!strcmp(field, h->fields[i].name))
            return &h->fields[i];

    return 0;
}

void def_val(hdr_t *h, const char *field, const char *def) {
    field_t *f = find_field(h, field);

    if (!f)
        return;

    f->def = parse_bytes(def, BIT_TO_BYTE(f->bit_width));
}

///////////////////////////////////////////////////////////////////////////////

void uninit_frame_data(hdr_t *h) {
    int i;

    for (i = 0; i < h->fields_size; ++i)
        bfree(h->fields[i].def);
}

static int bit_get(const buf_t *val, size_t bit_pos)
{
    size_t byte_pos        =      bit_pos / 8;
    size_t bit_within_byte = 7 - (bit_pos % 8);

    assert(byte_pos < val->size);

    return (val->data[byte_pos] >> bit_within_byte) & 0x1;
}

static void bit_set(buf_t *b, size_t bit_pos, int value)
{
     size_t byte_pos        =      bit_pos / 8;
     size_t bit_within_byte = 7 - (bit_pos % 8);

     assert(byte_pos < b->size);

     if (value) {
         b->data[byte_pos] |= (1 << bit_within_byte);
     } else {
         b->data[byte_pos] &= ~(1 << bit_within_byte);
     }
}

void hdr_write_field(buf_t *b, int offset, const field_t *f, const buf_t *val)
{
    size_t pos, bits_to_1st_valid;

    // b             = Output
    // b->size       = Number of bytes in output
    // b->data       = Buffer of b->size bytes.
    // f             = Field to encode
    // f->bit_width  = Number of bits to take from #val and place in #b
    // f->bit_offset = Position of msbit of #val when put in #b
    // val           = Value to write
    // val->size     = Number of bytes in value to write
    // val->data     = Buffer of val->size bytes.
    assert(8 * b->size >= f->bit_width + f->bit_offset + offset * 8);

    // How many bits do we have to move into the value to encode before we reach
    // the first valid bit given the field width?
    bits_to_1st_valid = 8 * val->size - f->bit_width;

    for (pos = 0; pos < f->bit_width; pos++)
        bit_set(b, f->bit_offset + pos + (8 * offset),
                bit_get(val, pos + bits_to_1st_valid));
 }


buf_t *frame_def(hdr_t *hdr) {
    int i;
    buf_t *b = balloc(hdr->size);

    if (!b)
        return b;

    for (i = 0; i < hdr->fields_size; ++i) {
        field_t *f = &hdr->fields[i];
        if (!f->def)
            continue;

        hdr_write_field(b, 0, f, f->def);
    }

    return b;
}

void frame_reset(frame_t *f) {
    int i;

    for (i = 0; i < FRAME_STACK_MAX; ++i) {
        if (f->stack[i]) {
            if (f->stack[i]->fields)
                free(f->stack[i]->fields);

            free(f->stack[i]);
        }
    }

    memset(f, 0, sizeof(*f));
}

hdr_t *frame_clone_and_push_hdr(frame_t *f, hdr_t *h) {
    hdr_t *new_hdr;
    field_t *new_fields;
    new_hdr = malloc(sizeof(*new_hdr));
    new_fields = malloc(sizeof(*new_fields) * h->fields_size);

    if (!new_hdr || !new_fields) {
        if (new_hdr)
            free(new_hdr);
        if (new_fields)
            free(new_fields);

        return 0;
    }

    assert(f->stack_size < FRAME_STACK_MAX);
    assert(!f->stack[f->stack_size]);

    memcpy(new_hdr, h, sizeof(*h));
    memcpy(new_fields, h->fields, sizeof(*new_fields) * h->fields_size);
    new_hdr->fields = new_fields;

    f->stack[f->stack_size] = new_hdr;

    f->stack_size ++;

    return new_hdr;
}

int hdr_parse_fields(hdr_t *hdr, int argc, const char *argv[]) {
    int i;
    field_t *f;

    for (i = 0; i < argc; ++i) {
        f = find_field(hdr, argv[i]);

        if (!f)
            return i;

        i += 1;

        // Check to see if we have a value argument
        if (i >= argc)
            return -1;

        printf("Assigned value for %s\n", f->name);
        f->val = parse_bytes(argv[i], BIT_TO_BYTE(f->bit_width));
    }

    return i;
}

int hdr_copy_to_buf(hdr_t *hdr, int offset, buf_t *buf) {
    int i;
    buf_t *v;
    field_t *f;

    for (i = 0, f = hdr->fields; i < hdr->fields_size; ++i, ++f) {
        if (BIT_TO_BYTE(f->bit_width) + offset > buf->size) {
            printf("Buf over flow\n");
            return -1;
        }

        if (f->val) {
            printf("val %s\n", f->name);
            v = f->val;
        } else if (f->def) {
            printf("def %s\n", f->name);
            v = f->def;
        } else {
            v = 0;
        }

        if (v)
            hdr_write_field(buf, offset, f, v);
    }

    return i;
}

buf_t *frame_to_buf(frame_t *f) {
    int i;
    buf_t *buf;
    int frame_size = 0, offset = 0;

    printf("Stack size: %d\n", f->stack_size);
    for (i = 0; i < f->stack_size; ++i) {
        f->stack[i]->offset_in_frame = frame_size;
        frame_size += f->stack[i]->size;
    }

    if (frame_size < 64)
        frame_size = 64;

    f->buf_size = frame_size;

    for (i = f->stack_size - 1; i >= 0; --i)
        if (f->stack[i]->frame_fill_defaults)
            f->stack[i]->frame_fill_defaults(f, i);

    buf = balloc(frame_size);

    for (i = 0; i < f->stack_size; ++i) {
        hdr_copy_to_buf(f->stack[i], offset, buf);
        offset += f->stack[i]->size;
    }

    return buf;
}

void eth_init();
void vlan_init();
void arp_init();
void ipv4_init();
void udp_init();
void payload_init();

void init() __attribute__ ((constructor));
void init() {
    eth_init();
    vlan_init();
    arp_init();
    ipv4_init();
    udp_init();
    payload_init();
}

void eth_uninit();
void vlan_uninit();
void arp_uninit();
void ipv4_uninit();
void udp_uninit();
void payload_uninit();

void uninit() __attribute__ ((destructor));
void uninit() {
    eth_uninit();
    vlan_uninit();
    arp_uninit();
    ipv4_uninit();
    udp_uninit();
    payload_uninit();
}
