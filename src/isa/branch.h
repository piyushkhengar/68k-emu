#ifndef BRANCH_H
#define BRANCH_H

#include <stdint.h>

int branch_condition_met(uint8_t cond);
int op_bcc(uint16_t op);

#endif /* BRANCH_H */
