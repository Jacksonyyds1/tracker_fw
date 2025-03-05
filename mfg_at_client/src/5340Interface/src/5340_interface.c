
#include "5340_interface.h"
#include "spis_interface.h"
#include "uart_interface.h"

int init_5340_interface() 
{

#if (USE_UART_TO_NRF5340)
    // Initialize UART
    init_uart_interface();
#else
    // Initialize SPI
    init_spis_interface();
#endif
    return 0;
}