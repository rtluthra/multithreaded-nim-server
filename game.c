#include "game.h"

/* The Nim rules: players alternate removing stones from a single pile, and
   whoever takes the last stone wins. */

/* Every game starts from the same board, as required by the write-up. */
void init_piles(int *p)
{
    p[0] = 1;
    p[1] = 3;
    p[2] = 5;
    p[3] = 7;
    p[4] = 9;
}

/* The game is over when there are no more stones in any of the piles. */
int is_game_over(const int *p)
{
    int i;
    for (i = 0; i < NIM_PILES; i++) {
        if (p[i] > 0)
            return 0;
    }
    return 1;
}

int check_move(const int *p, int pile, int amount)
{
    if (pile < 1 || pile > NIM_PILES)
        return MOVE_BAD_PILE;
    if (amount < 1 || amount > p[pile - 1])
        return MOVE_BAD_QTY;
    return MOVE_OK;
}

/* Callers must validate with check_move first. */
void move(int *p, int pile, int amount)
{
    p[pile - 1] -= amount;
}
