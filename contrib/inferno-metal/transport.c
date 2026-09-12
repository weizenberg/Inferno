/*
 * Inferno guest-to-host Metal execution bridge.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "transport.h"

#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Inferno Metal guest transport requires little-endian AArch64"
#endif

static void barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t read_reg(const ImtlTransport *t, unsigned offset)
{
    uint32_t value = t->registers[offset / 4];
    barrier();
    return value;
}

static void write_reg(ImtlTransport *t, unsigned offset, uint32_t value)
{
    barrier();
    t->registers[offset / 4] = value;
    barrier();
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, value >> 32);
}

void imtl_encode(uint8_t raw[INFERNO_METAL_DESCRIPTOR_SIZE],
                 const InfernoMetalCommand *c)
{
    for (unsigned i = 0; i < INFERNO_METAL_DESCRIPTOR_SIZE; i++) {
        raw[i] = 0;
    }
    put32(raw, INFERNO_METAL_VERSION);
    put32(raw + 4, c->opcode);
    put64(raw + 8, c->sequence);
    put64(raw + 16, c->source_gpa);
    put32(raw + 24, c->source_size);
    put32(raw + 28, c->input_size);
    put64(raw + 32, c->input_gpa);
    put64(raw + 40, c->output_gpa);
    put32(raw + 48, c->output_size);
    put32(raw + 52, c->width);
    put32(raw + 56, c->height);
    put32(raw + 60, c->depth);
    for (unsigned i = 0; i < 64; i++) {
        raw[64 + i] = (uint8_t)c->function[i];
        raw[128 + i] = (uint8_t)c->fragment[i];
    }
    put32(raw + INFERNO_METAL_DESCRIPTOR_OPTIONS_OFFSET, c->options);
}

ImtlResult imtl_open(ImtlTransport *t, volatile void *registers, size_t size)
{
    if (!t || !registers || ((uintptr_t)registers & 3) ||
        size < INFERNO_METAL_REGISTER_SIZE) {
        return IMTL_BAD_ARGUMENT;
    }
    if (t->phase != IMTL_CLOSED) {
        return IMTL_BUSY;
    }
    t->registers = registers;
    if (read_reg(t, INFERNO_METAL_REG_MAGIC) != INFERNO_METAL_MAGIC ||
        read_reg(t, INFERNO_METAL_REG_VERSION) != INFERNO_METAL_VERSION) {
        t->registers = NULL;
        return IMTL_BAD_DEVICE;
    }
    if (read_reg(t, INFERNO_METAL_REG_STATUS) != INFERNO_METAL_IDLE ||
        read_reg(t, INFERNO_METAL_REG_PENDING)) {
        t->registers = NULL;
        return IMTL_BUSY;
    }
    write_reg(t, INFERNO_METAL_REG_IRQ_ENABLE, 0);
    t->sequence = 0;
    t->phase = IMTL_IDLE;
    return IMTL_OK;
}

ImtlResult imtl_submit(ImtlTransport *t, uint64_t descriptor_gpa,
                       uint64_t sequence, bool interrupt)
{
    if (!t || t->phase == IMTL_CLOSED) {
        return IMTL_BAD_ARGUMENT;
    }
    if (t->phase != IMTL_IDLE) {
        return IMTL_BUSY;
    }
    if (read_reg(t, INFERNO_METAL_REG_STATUS) != INFERNO_METAL_IDLE ||
        read_reg(t, INFERNO_METAL_REG_PENDING)) {
        return IMTL_BAD_DEVICE;
    }
    t->sequence = sequence;
    t->phase = IMTL_SUBMITTED;
    write_reg(t, INFERNO_METAL_REG_DESCRIPTOR_LO, (uint32_t)descriptor_gpa);
    write_reg(t, INFERNO_METAL_REG_DESCRIPTOR_HI, descriptor_gpa >> 32);
    write_reg(t, INFERNO_METAL_REG_IRQ_ENABLE, interrupt ? 1 : 0);
    write_reg(t, INFERNO_METAL_REG_SUBMIT, 1);
    return IMTL_OK;
}

ImtlResult imtl_poll(ImtlTransport *t, ImtlCompletion *completion)
{
    uint32_t state, error;
    uint64_t sequence;

    if (!t || t->phase == IMTL_CLOSED || t->phase == IMTL_IDLE ||
        (!completion && t->phase != IMTL_DRAINING)) {
        return IMTL_BAD_ARGUMENT;
    }
    state = read_reg(t, INFERNO_METAL_REG_STATUS);
    if (t->phase == IMTL_DRAINING) {
        if (state == INFERNO_METAL_BUSY) {
            return IMTL_AGAIN;
        }
        if (state != INFERNO_METAL_IDLE ||
            read_reg(t, INFERNO_METAL_REG_PENDING)) {
            return IMTL_BAD_DEVICE;
        }
        t->phase = IMTL_IDLE;
        t->sequence = 0;
        return IMTL_OK;
    }
    if (state == INFERNO_METAL_BUSY) {
        return IMTL_AGAIN;
    }
    if ((state != INFERNO_METAL_DONE && state != INFERNO_METAL_FAILED) ||
        read_reg(t, INFERNO_METAL_REG_PENDING) != 1) {
        return IMTL_BAD_DEVICE;
    }
    sequence = read_reg(t, INFERNO_METAL_REG_SEQUENCE_LO);
    sequence |= (uint64_t)read_reg(t, INFERNO_METAL_REG_SEQUENCE_HI) << 32;
    error = read_reg(t, INFERNO_METAL_REG_ERROR);
    if (sequence != t->sequence) {
        return IMTL_SEQUENCE_MISMATCH;
    }
    if ((state == INFERNO_METAL_DONE) != (error == INFERNO_METAL_OK) ||
        error > INFERNO_METAL_BACKEND_ERROR) {
        return IMTL_BAD_DEVICE;
    }
    completion->sequence = sequence;
    completion->error = error;
    t->phase = IMTL_COMPLETED;
    return IMTL_OK;
}

ImtlResult imtl_ack(ImtlTransport *t)
{
    if (!t || t->phase != IMTL_COMPLETED) {
        return IMTL_BAD_ARGUMENT;
    }
    write_reg(t, INFERNO_METAL_REG_ACK, 1);
    if (read_reg(t, INFERNO_METAL_REG_STATUS) != INFERNO_METAL_IDLE ||
        read_reg(t, INFERNO_METAL_REG_PENDING)) {
        return IMTL_BAD_DEVICE;
    }
    t->phase = IMTL_IDLE;
    t->sequence = 0;
    return IMTL_OK;
}

ImtlResult imtl_reset(ImtlTransport *t)
{
    if (!t || t->phase == IMTL_CLOSED) {
        return IMTL_BAD_ARGUMENT;
    }
    if (t->phase != IMTL_DRAINING) {
        t->phase = IMTL_DRAINING;
        write_reg(t, INFERNO_METAL_REG_IRQ_ENABLE, 0);
        write_reg(t, INFERNO_METAL_REG_RESET, 1);
    }
    return imtl_poll(t, NULL);
}

ImtlResult imtl_close(ImtlTransport *t)
{
    if (!t || t->phase == IMTL_CLOSED) {
        return IMTL_BAD_ARGUMENT;
    }
    if (t->phase != IMTL_IDLE) {
        return IMTL_BUSY;
    }
    if (read_reg(t, INFERNO_METAL_REG_STATUS) != INFERNO_METAL_IDLE ||
        read_reg(t, INFERNO_METAL_REG_PENDING)) {
        return IMTL_BAD_DEVICE;
    }
    write_reg(t, INFERNO_METAL_REG_IRQ_ENABLE, 0);
    t->registers = NULL;
    t->phase = IMTL_CLOSED;
    return IMTL_OK;
}
