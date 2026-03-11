#ifndef MOVEP_H
#define MOVEP_H

#include <stdint.h>

/* MOVEP: 0x0108-0x010F, 0x0148-0x014F, 0x0188-0x018F, 0x01C8-0x01CF. Returns cycles or 0. */
int op_movep(uint16_t op);

#endif /* MOVEP_H */
