#include "color.h"

void cl(int code) { printf("\x1b[%dm", code); }

void clcl(void) {
    cl(CL_CLEAR);
}
