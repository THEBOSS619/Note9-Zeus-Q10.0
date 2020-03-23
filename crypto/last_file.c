__attribute__ ((section(".rodata"), used))
const unsigned char last_crypto_rodata = 0x20;

__attribute__ ((section(".text"), used))
void last_crypto_text(void){}

__attribute__ ((section(".init.text"), optimize("-O0"), used))
static void last_crypto_init(void){};
