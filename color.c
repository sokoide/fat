#include "color.h"

void cl(int code) { printf("\x1b[%dm", code); }

void clbg(int code) { cl(code + CL_BG2); }

void clcl() {
    cl(CL_CLEAR);
    /* printf("\x1b[m"); */
    /* printf("\x1b[39m"); */
    /* printf("\x1b[49m"); */
}

void cl_test() {
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 10; j++) {
            printf("\x1b[%dm %03d \x1b[0m", i * 10 + j, i * 10 + j);
        }
        printf("\n");
    }
}
