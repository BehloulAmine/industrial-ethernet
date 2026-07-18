#ifndef APP_EIP_H_
#define APP_EIP_H_

#include <stdint.h>

#define APP_EIP_INPUT_ASSEMBLY 100U
#define APP_EIP_OUTPUT_ASSEMBLY 101U
#define APP_EIP_ASSEMBLY_WORD_COUNT 10U

int app_eip_start(void);
uint16_t app_eip_connection_count(void);

#endif /* APP_EIP_H_ */
