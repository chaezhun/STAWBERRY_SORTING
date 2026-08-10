#include <i2c.h>
#include <lcd.h>
#include "lcd1602.h"

void LCD1602_Run(void)
{
    uint32 addr;

    SAL_OsInitFuncs();
    I2C_Init(); // bring up I2C

    // the channel and port are configured in lcd.h
    if (I2C_Open(I2C_CH, I2C_PORT, I2C_SPEED, NULL, NULL) != SAL_RET_SUCCESS) {
        mcu_printf("I2C open failed\n");
        return; // could not open the bus
    }

    addr = I2C_ScanSlave(I2C_CH); // probe for the display
    mcu_printf("Detected I2C Slave Address: 0x%02X (I2C_CH=%d, I2C_PORT=%d)\n",
               addr, (int)I2C_CH, (int)I2C_PORT);

    lcd_init();
    lcd_cmd(0x80); // cursor home
    lcd_print("Hello TOPST");

    while (1) {
        SAL_TaskSleep(1000);
    }
}