#ifndef GAME_H
#define GAME_H

#define NIM_PILES 5

/* Results of check_move.  The pile and quantity cases are kept apart because
   the protocol has a separate error code for each (32 and 33). */
#define MOVE_OK        0
#define MOVE_BAD_PILE  1
#define MOVE_BAD_QTY   2

void init_piles(int *p);
int  is_game_over(const int *p);
int  check_move(const int *p, int pile, int amount);
void move(int *p, int pile, int amount);

#endif
