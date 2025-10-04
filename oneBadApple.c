/**********************************************************
 *
 * oneBadApple.c
 * CIS 451 Project 1 (F25)
 *
 * Gerrit Mitchell + Shah Kamali
 *************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


int main(int argc, char *argv[]) {
    /* Do stuff */
    return 0;
}

void interrupthandler(int sig_num) {
        if (sig_num == SIGINT) {
                sleep(1);
                exit(0);
                return;
        }
}
