/**
 * Test fixture for ARMv5TE instruction execution and ELF loader verification.
 * Compiled with: clang -target armv5te-linux-androideabi -fuse-ld=lld -shared -nostdlib
 */

typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;

// External host function to test PLT / jump slot bridging
extern int host_bridge_multiply(int x, int factor);

// Global data in .data and .rodata to test data relocations
uint32_t g_magic_number = 0xCAFEBABE;
const char g_greeting_str[] = "Hello ARM32 Execution!";

// 1. Arithmetic & Bitwise
int test_arithmetic(int a, int b) {
    int sum = a + b;
    int diff = a - b;
    int product = sum * diff; // (a+b)*(a-b) = a^2 - b^2
    int bit_ops = (a ^ b) | (a & 0x0F);
    return product + (bit_ops >> 1);
}

// 2. Conditionals, Loops & CPSR flags (EQ, NE, GT, LT, GE, LE, CS, CC)
int test_conditionals_and_loops(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        if ((i & 1) == 0) {
            acc += (i * 2);
        } else {
            acc -= i;
        }
    }
    return acc;
}

// 3. Memory Loads & Stores (Byte, Halfword, Word, Stack frames)
int test_memory_load_store(int initial_val) {
    volatile uint8_t b_arr[4];
    b_arr[0] = (uint8_t)(initial_val & 0xFF);
    b_arr[1] = (uint8_t)((initial_val >> 8) & 0xFF);
    b_arr[2] = (uint8_t)((initial_val >> 16) & 0xFF);
    b_arr[3] = (uint8_t)((initial_val >> 24) & 0xFF);

    uint32_t reconstructed = ((uint32_t)b_arr[0]) |
                             (((uint32_t)b_arr[1]) << 8) |
                             (((uint32_t)b_arr[2]) << 16) |
                             (((uint32_t)b_arr[3]) << 24);
    return (int)reconstructed;
}

// 4. Global Data Access (.data and .rodata relocations)
uint32_t test_global_data(uint32_t addend) {
    uint32_t val = g_magic_number;
    uint32_t first_char = (uint32_t)g_greeting_str[0]; // 'H' = 72
    return val + addend + first_char;
}

// 5. Indirect Function Calls (Function pointers)
typedef int (*math_func_t)(int, int);

static int helper_add(int x, int y) { return x + y; }
static int helper_sub(int x, int y) { return x - y; }

int test_indirect_call(int selector, int x, int y) {
    math_func_t fn = (selector == 1) ? helper_add : helper_sub;
    return fn(x, y);
}

// 6. External Host Function Bridge via PLT
int test_host_bridge(int value) {
    return host_bridge_multiply(value, 3);
}
