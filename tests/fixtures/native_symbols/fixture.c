// Original fixture for TraceRender's native PDB resolver. No Windows SDK or CRT needed.
__declspec(noinline) int allocate_buffer(int size) {
    volatile int bytes = size + 64;
    return bytes;
}

__declspec(noinline) int release_buffer(int value) {
    return value - 64;
}

void fixture_entry(void) {
    volatile int value = allocate_buffer(32);
    value = release_buffer(value);
}
