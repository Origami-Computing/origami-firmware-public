#pragma once
#define PROGMEM
#define pgm_read_byte(address) (*(const unsigned char*)(address))
