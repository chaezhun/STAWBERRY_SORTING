// SPDX-License-Identifier: Apache-2.0

/*
***************************************************************************************************
*
*   FileName : lcd.h
*
*   Copyright (c) Telechips Inc.
*
*   Description :
*
*
***************************************************************************************************
*/


#ifndef I2C_LCD_H_
#define I2C_LCD_H_
// I2C_CH=3, I2C_PORT=7 → i2c.c i2cPortCfg[7] = GPC6(SDA)/GPC7(SCL), I2C3
// The board SCL/SDA header. The default 0/0 maps to pins the LCD is not wired to, which is why a scan returned 0x00.
#define I2C_CH     3
#define I2C_PORT   7
#define LCD_ADDR   0x27
#define I2C_SPEED  100
#define LCD_CMD    0
#define LCD_DATA   1

void lcd_send(uint8 mode, uint8 data);
void lcd_cmd(uint8 cmd);
void lcd_data(uint8 data);
void lcd_init(void);
void lcd_print(const char *str);

#endif
