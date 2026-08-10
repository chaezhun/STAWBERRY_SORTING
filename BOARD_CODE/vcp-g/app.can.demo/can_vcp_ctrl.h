#ifndef MCU_BSP_CAN_VCP_CTRL_HEADER
#define MCU_BSP_CAN_VCP_CTRL_HEADER

#if (MCU_BSP_SUPPORT_CAN_DEMO == 1)

// ============================================
// Strawberry sorting - VCP-G control header
// ============================================

// LED pins. Pin 17 is unusable on this board, so yellow moved to pin 18.
#define LED_RED_PIN         GPIO_GPA(6)   // red, rotten
#define LED_YELLOW_PIN      GPIO_GPA(28)  // yellow, unripe (pin 18, not 17)
#define LED_GREEN_PIN       GPIO_GPA(29)  // green, fresh
#define LED_BLUE_PIN        GPIO_GPA(7)   // unused, sits on the dead pin

// Legacy direct-PWM pins, unused now that the conveyor has its own module
#define IN1 GPIO_GPA(22)
#define IN2 GPIO_GPA(21)
#define IN3 GPIO_GPA(20)
#define IN4 GPIO_GPA(19)

// Conveyor enable pin
//   Speed is a manual knob on the module; the board only starts and stops it
//   by switching the module power through a relay.
//   High runs, low stops.
#define CONVEYOR_EN_PIN  GPIO_GPB(2)

// ============================================
// PWM output pin map, following the SDK port table
//   The physical pin is the channel number combined with the peripheral port,
//   which selects the GPIO bank.
//
//   function       channel  port    GPIO
//   conveyor A       0     CH0     GPA(10)
//   conveyor B       4     CH1     GPB(10)
//   servo 1 rotten   1     CH1     GPB(7)
//   servo 2 unripe   2     CH2     GPC(8)
//   servo 3 spare    5     CH3     GPK(13)
//   servo 4 spare    3     CH3     GPK(11)
// ============================================

// conveyor PWM
#define ENA_SEL  0
#define ENA_PORT GPIO_PERICH_CH0   // MotorA → GPA(10)
#define ENB_SEL  4
#define ENB_PORT GPIO_PERICH_CH1   // MotorB → GPB(10)

// Servo PWM. Only the first two are used; the others are reserved.
#define SERVO1_CH    1
#define SERVO1_PORT  GPIO_PERICH_CH1   // rotten
#define SERVO2_CH    2
#define SERVO2_PORT  GPIO_PERICH_CH2   // unripe
#define SERVO3_CH    5
#define SERVO3_PORT  GPIO_PERICH_CH3   // reserved
#define SERVO4_CH    3
#define SERVO4_PORT  GPIO_PERICH_CH3   // reserved

// Position-controlled servo, 0 to 180 degrees.
//   Byte 0 of the CAN frame is the target angle, converted to a pulse width:
//     0 degrees is 0.5 ms, 90 is 1.5 ms, 180 is 2.5 ms, at 50 Hz.
//   The gate motion is driven entirely from the A72 side by sending angles:
//     home, tilt, hold, back to home.
//   Angles and hold times are Python constants, so tuning needs no reflash.
//   A continuous-rotation servo was tried first, but it can only be commanded by speed and time, which drifted; a position servo replaced it.
#define SERVO_HOME_DEG   90            // neutral angle at boot and at rest

// PWM constants
#define PDM_PERIOD_NS 250000
#define SPEED_MAX     80
#define DUTY_MAX      80
#define MIN_ON_NS     15000

// I2C and LCD
// Channel 3, port 7 maps to GPC6 and GPC7, the board SDA and SCL header
#define I2C_CH      3
#define I2C_PORT    7
#define LCD_ADDR    0x27
#define I2C_SPEED   100
#define LCD_CMD     0
#define LCD_DATA    1

// ============================================
// CAN message identifiers
// ============================================
enum VCP_IO_TYPE {
    VCP_IO_SERVO_ROTTEN  = 0x111,   // rotten gate servo
    VCP_IO_SERVO_UNRIPE  = 0x112,   // unripe gate servo
    VCP_IO_SERVO_OTHER   = 0x113,   // spare servo
    VCP_IO_LED_CONTROL   = 0x114,   // LED control
    VCP_IO_CONVEYOR      = 0x115,   // conveyor
    VCP_IO_LCD_STATS     = 0x116,   // LCD totals
    VCP_IO_SERVO_4       = 0x117    // reserved
};

// LED identifiers
enum VCP_LED_ID {
    VCP_LED_ALL_OFF = 0x00,
    VCP_LED_RED     = 0x01,
    VCP_LED_YELLOW  = 0x02,
    VCP_LED_BLUE    = 0x03,
    VCP_LED_GREEN   = 0x04
};

// servo actions, from the continuous-rotation era
enum VCP_IO_ACTION {
    VCP_IO_ACTION_OFF = 0x00,   // stop
    VCP_IO_ACTION_ON  = 0x01,   // forward
    VCP_IO_ACTION_REV = 0x02    // reverse
};

// public interface
void ControlBreadBoardSensors(uint32 mId, uint8 nDataLength, sint8* pucData);
boolean InitSensorControls(void);

#endif
#endif