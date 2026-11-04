/* mf_quit: tells mvexflowd on the card to end its session and exit. */
#include <stdio.h>

#include "mf_link.h"

int main(void)
{
    struct mf_link l;
    if (mf_link_open(&l))
        return 1;
    l.ctl->session_end = 1;
    l.ctl->quit = 1;
    mf_link_close(&l);
    return 0;
}
