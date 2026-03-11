#ifndef MOVEM_H
#define MOVEM_H

#include <stdint.h>

int movem_store_ea_valid(int mode, int reg);
int movem_load_ea_valid(int mode, int reg);
int op_movem_store(uint16_t op);
int op_movem_load(uint16_t op);

#endif /* MOVEM_H */
