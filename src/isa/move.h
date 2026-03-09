#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>

int dispatch_move_b(uint16_t op);
int dispatch_move_w(uint16_t op);
int dispatch_move_l(uint16_t op);

#endif /* MOVE_H */
