#ifndef ALU_H
#define ALU_H

#include <stdint.h>

int op_moveq(uint16_t op);
int dispatch_9xxx(uint16_t op);
int dispatch_Bxxx(uint16_t op);
int dispatch_add(uint16_t op);

#endif /* ALU_H */
