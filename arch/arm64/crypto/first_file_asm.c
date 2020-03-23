__attribute__ ((section(".rodata"), used))
const unsigned char first_crypto_asm_rodata = 0x10;

__attribute__ ((section(".text"), used))
void first_crypto_asm_text(void){}

__attribute__ ((section(".init.text"), optimize("-O0"), used))
static void first_crypto_asm_init(void){};
