// minimal: write to stdout + a non-zero exit code (exercises write + exit_group).
#include <unistd.h>
int main(void) { write(1, "hi\n", 3); return 42; }
