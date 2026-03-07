#ifndef ALU_H
#define ALU_H

#include <stdint.h>

void op_moveq(uint16_t op);
void dispatch_9xxx(uint16_t op);
void dispatch_Bxxx(uint16_t op);
void dispatch_add(uint16_t op);

#endif /* ALU_H */
