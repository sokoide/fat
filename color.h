#include <stdio.h>

#define CL_H "\x1b["
#define CL_CLEAR 0
#define CL_RED 31
#define CL_GREEN 32
#define CL_YELLOW 33
#define CL_BLUE 34
#define CL_PURPLE 35
#define CL_CYAN 36
#define CL_GRAY 37
#define CL_BG1 10
#define CL_BG2 70

void cl_test();
void cl(int code);
void clbg(int code);
void clcl();
