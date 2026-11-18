#pragma once

// Configures the button GPIOs as inputs.
void input_output_init(void);

// Reads an integer from NVS, returning def if it has not been stored yet.
int storage_read_int(char *name, int def);

// Writes an integer to NVS and commits it.
void storage_write_int(char *name, int val);
