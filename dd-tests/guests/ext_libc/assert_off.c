// assert() compiled out under NDEBUG: a false assert is a no-op, execution continues. Portable verdict.
#define NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void) {
    int reached = 0;
    assert(0 && "compiled out under NDEBUG");
    reached = 1;
    assert(reached == 99); // also a no-op
    printf("assert_off reached=%d\n", reached);
    return 0;
}
