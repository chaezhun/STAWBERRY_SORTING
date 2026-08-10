#if (MCU_BSP_SUPPORT_CAN_DEMO == 1)

#include <app_cfg.h>
#include "bsp.h"
#include "gic.h"
#include "gpio.h"
#include "debug.h"
#include "pdm.h"
#include "i2c.h"
#include "stdio.h"
#include "can_vcp_ctrl.h"

#define MIN_DUTY (35)

static PDMModeConfig_t cfgA, cfgB;

// --- forward declarations ---
static void lcd_send(uint8 mode, uint8 data);
static void lcd_cmd(uint8 cmd);
static void lcd_data(uint8 dat);
static void lcd_init(void);
static void lcd_print(const char *str);

static void pin_out(uint32 p);
static void pin_hi(uint32 p);
static void pin_lo(uint32 p);

static int pdm_apply(uint32 ch, PDMModeConfig_t* cfg);
static void MotorPDM_Init(void);
static void MotorA_Set(uint32 duty_pct, uint32 forward);
static void MotorB_Set(uint32 duty_pct, uint32 forward);
static uint32 duty_pct_to_ns(uint32 pct, uint32 period_ns);
static sint8 duty_from_speed(sint8 speed);
static void ConfigureServoPDM(uint32 channel, uint32 port, uint32 angle_deg);

static void ControlLED(uint8 led_id, uint8 action);
static void ControlServo(uint8 servo_num, uint8 action);
static void ControlConveyor(uint8 action, uint8 speed);
static void ControlLCDStats(uint8 total, uint8 fresh, uint8 rotten, uint8 unripe);


// ============================================
// initialisation
// ============================================
boolean InitSensorControls(void) {
    MotorPDM_Init();
    I2C_Init();
    if (I2C_Open(I2C_CH, I2C_PORT, I2C_SPEED, NULL_PTR, NULL_PTR) != SAL_RET_SUCCESS) {
        mcu_printf("[LCD] I2C_Open failed (ch=%d, port=%d, speed=%d)\r\n",
                   (int)I2C_CH, (int)I2C_PORT, (int)I2C_SPEED);
        return FALSE;
    }
    I2C_ScanSlave(I2C_CH);
    lcd_init();

    // LED pins
    GPIO_Config(LED_RED_PIN, GPIO_FUNC(0) | GPIO_OUTPUT | GPIO_NOPULL | GPIO_DS(3) | GPIO_INPUTBUF_DIS);
    GPIO_Config(LED_YELLOW_PIN, GPIO_FUNC(0) | GPIO_OUTPUT | GPIO_NOPULL | GPIO_DS(3) | GPIO_INPUTBUF_DIS);
    GPIO_Config(LED_BLUE_PIN, GPIO_FUNC(0) | GPIO_OUTPUT | GPIO_NOPULL | GPIO_DS(3) | GPIO_INPUTBUF_DIS);
    GPIO_Config(LED_GREEN_PIN, GPIO_FUNC(0) | GPIO_OUTPUT | GPIO_NOPULL | GPIO_DS(3) | GPIO_INPUTBUF_DIS);
    GPIO_Set(LED_RED_PIN, 0);
    GPIO_Set(LED_YELLOW_PIN, 0);
    GPIO_Set(LED_BLUE_PIN, 0);
    GPIO_Set(LED_GREEN_PIN, 0);

    // conveyor enable pin, driving a relay; off at boot
    GPIO_Config(CONVEYOR_EN_PIN, GPIO_FUNC(0) | GPIO_OUTPUT | GPIO_NOPULL | GPIO_DS(3) | GPIO_INPUTBUF_DIS);
    GPIO_Set(CONVEYOR_EN_PIN, 0);

    // servo to its home angle, which is the closed-gate position
    ControlServo(1, SERVO_HOME_DEG);
    ControlServo(2, SERVO_HOME_DEG);

    // LCD splash
    lcd_cmd((uint8)(0x80 | 0x00));
    lcd_print("Strawberry Sort");
    lcd_cmd((uint8)(0x80 | 0x40));
    lcd_print("Ready...");

    mcu_printf("[STRAWBERRY] InitSensorControls done\r\n");
    return TRUE;
}


// ============================================
// LED control
// ============================================
static void ControlLED(uint8 led_id, uint8 action) {
    // id 0 turns everything off.
    // Any other id changes only that LED and leaves the rest alone, so several
    // colours can be lit at once when different classes are detected together.
    if (led_id == VCP_LED_ALL_OFF) {
        GPIO_Set(LED_RED_PIN, 0);
        GPIO_Set(LED_YELLOW_PIN, 0);
        GPIO_Set(LED_BLUE_PIN, 0);
        GPIO_Set(LED_GREEN_PIN, 0);
        mcu_printf("[LED] ALL OFF\r\n");
        return;
    }

    uint32 pin;
    switch (led_id) {
        case VCP_LED_RED:    pin = LED_RED_PIN;    break;
        case VCP_LED_YELLOW: pin = LED_YELLOW_PIN; break;
        case VCP_LED_BLUE:   pin = LED_BLUE_PIN;   break;
        case VCP_LED_GREEN:  pin = LED_GREEN_PIN;  break;
        default:
            mcu_printf("[LED] Unknown ID: 0x%02X\r\n", led_id);
            return;
    }
    GPIO_Set(pin, (action == VCP_IO_ACTION_ON) ? 1U : 0U);
    mcu_printf("[LED] id=%d %s\r\n", (int)led_id, (action == VCP_IO_ACTION_ON) ? "ON" : "OFF");
}


// ============================================
// servo control, the sorting gate
// ============================================
static void ControlServo(uint8 servo_num, uint8 angle) {
    uint32 channel, port;

    // the pin map lives in can_vcp_ctrl.h
    switch (servo_num) {
        case 1:  // rotten gate
            channel = SERVO1_CH;  port = SERVO1_PORT;  break;
        case 2:  // unripe gate
            channel = SERVO2_CH;  port = SERVO2_PORT;  break;
        case 3:  // spare gate
            channel = SERVO3_CH;  port = SERVO3_PORT;  break;
        case 4:  // reserved gate
            channel = SERVO4_CH;  port = SERVO4_PORT;  break;
        default:
            mcu_printf("[SERVO] Unknown servo: %d\r\n", servo_num);
            return;
    }

    // Position-controlled servo: byte 0 of the frame is the target angle.
    if (angle > 180U) angle = 180U;
    mcu_printf("[SERVO%d] angle = %d deg\r\n", servo_num, angle);
    ConfigureServoPDM(channel, port, (uint32)angle);
}


// ============================================
// conveyor enable, a hybrid arrangement
//   Speed is set by a knob on the external PWM module; the board only switches
//   its power. The speed byte is ignored, kept only for compatibility.
// ============================================
static void ControlConveyor(uint8 action, uint8 speed) {
    (void)speed;
    /* The old direct-PWM path is unused now; suppress the warning. */
    (void)MotorA_Set; (void)MotorB_Set; (void)duty_from_speed;
    if (action == VCP_IO_ACTION_ON) {
        GPIO_Set(CONVEYOR_EN_PIN, 1);   // relay on, module powered, belt runs
        mcu_printf("[CONVEYOR] ON (enable HIGH)\r\n");
    } else {
        GPIO_Set(CONVEYOR_EN_PIN, 0);   // off, belt stops
        mcu_printf("[CONVEYOR] OFF (enable LOW)\r\n");
    }
}


// ============================================
// LCD totals
// ============================================
static void ControlLCDStats(uint8 total, uint8 fresh, uint8 rotten, uint8 unripe) {
    char line1[17], line2[17];

    // first row: total and fresh; second row: rotten and unripe
    (void)snprintf(line1, sizeof(line1), "T:%3u F:%3u", (unsigned)total, (unsigned)fresh);
    (void)snprintf(line2, sizeof(line2), "R:%3u U:%3u", (unsigned)rotten, (unsigned)unripe);

    lcd_cmd((uint8)(0x80 | 0x00));
    lcd_print(line1);
    lcd_cmd((uint8)(0x80 | 0x40));
    lcd_print(line2);

    mcu_printf("[LCD] T:%u F:%u R:%u U:%u\r\n", total, fresh, rotten, unripe);
}


// ============================================
// LCD driver, from the board SDK
// ============================================
void lcd_send(uint8 mode, uint8 data) {
    uint8 high = data & 0xF0;
    uint8 low = (data << 4) & 0xF0;
    uint8 buf[6];

    buf[0] = high | 0x08 | (mode ? 0x01 : 0x00);
    buf[1] = high | 0x0C | (mode ? 0x01 : 0x00);
    buf[2] = high | 0x08 | (mode ? 0x01 : 0x00);
    buf[3] = low  | 0x08 | (mode ? 0x01 : 0x00);
    buf[4] = low  | 0x0C | (mode ? 0x01 : 0x00);
    buf[5] = low  | 0x08 | (mode ? 0x01 : 0x00);

    I2CXfer_t xfer = {
        .xCmdLen = 0,
        .xOutLen = 6,
        .xOutBuf = buf,
        .xInLen = 0,
        .xInBuf = NULL,
        .xCmdBuf = NULL,
        .xOpt = 0
    };
    (void)I2C_Xfer(I2C_CH, (uint8)(LCD_ADDR << 1), xfer, 0);
    SAL_TaskSleep(2);
}

void lcd_cmd(uint8 cmd) { lcd_send(LCD_CMD, cmd); }
void lcd_data(uint8 dat) { lcd_send(LCD_DATA, dat); }

void lcd_init(void) {
    SAL_TaskSleep(50);
    lcd_cmd(0x33);
    lcd_cmd(0x32);
    lcd_cmd(0x28);
    lcd_cmd(0x0C);
    lcd_cmd(0x06);
    lcd_cmd(0x01);
    SAL_TaskSleep(5);
}

void lcd_print(const char *str) {
    while (*str) lcd_data((uint8)*str++);
}


// ============================================
// motor driver, from the board SDK
// ============================================
static inline void pin_out(uint32 p) { GPIO_Config(p, GPIO_OUTPUT | GPIO_FUNC(0) | GPIO_DS(3)); }
static inline void pin_hi(uint32 p) { GPIO_Set(p, 1); }
static inline void pin_lo(uint32 p) { GPIO_Set(p, 0); }

static int pdm_apply(uint32 ch, PDMModeConfig_t* cfg) {
    (void)PDM_Disable(ch, PMM_OFF);
    uint32 wait = 0;
    while (PDM_GetChannelStatus(ch) && wait < 100) {
        SAL_TaskSleep(1);
        wait++;
    }
    if (PDM_SetConfig(ch, cfg) != SAL_RET_SUCCESS) return -1;
    if (PDM_Enable(ch, PMM_OFF) != SAL_RET_SUCCESS) return -2;
    return 0;
}

uint32 duty_pct_to_ns(uint32 pct, uint32 period_ns) {
    if (pct == 0) return 0;
    if (pct > 100) pct = 100;
    if (pct < MIN_DUTY && pct > 0) pct = MIN_DUTY;
    uint64 num = (uint64)period_ns * (uint64)pct + 50ULL;
    return (uint32)(num / 100ULL);
}

sint8 duty_from_speed(sint8 speed) {
    if (speed == 0) return 0;
    if (speed > 80) speed = 80;
    if (speed < 0) return 0;
    return 40 + ((speed - 1) * 80) / 79;
}

void MotorPDM_Init(void) {
    static boolean inited = FALSE;
    if (inited) return;

    pin_out(IN1); pin_out(IN2);
    pin_out(IN3); pin_out(IN4);
    pin_lo(IN1); pin_lo(IN2);
    pin_lo(IN3); pin_lo(IN4);

    PDM_Init();
    PDM_CfgSetWrPw();
    PDM_CfgSetWrLock(0);

    SAL_MemSet(&cfgA, 0, sizeof(cfgA));
    cfgA.mcPortNumber = ENA_PORT;
    cfgA.mcOperationMode = PDM_OUTPUT_MODE_PHASE_1;
    cfgA.mcPeriodNanoSec1 = PDM_PERIOD_NS;
    cfgA.mcDutyNanoSec1 = 0;
    (void)pdm_apply(ENA_SEL, &cfgA);

    SAL_MemSet(&cfgB, 0, sizeof(cfgB));
    cfgB.mcPortNumber = ENB_PORT;
    cfgB.mcOperationMode = PDM_OUTPUT_MODE_PHASE_1;
    cfgB.mcPeriodNanoSec1 = PDM_PERIOD_NS;
    cfgB.mcDutyNanoSec1 = 0;
    (void)pdm_apply(ENB_SEL, &cfgB);

    inited = TRUE;
}

void MotorA_Set(uint32 duty_pct, uint32 forward) {
    if (duty_pct == 0) {
        pin_lo(IN1); pin_lo(IN2);
    } else if (forward) {
        pin_lo(IN1); pin_hi(IN2);
    } else {
        pin_hi(IN1); pin_lo(IN2);
    }
    cfgA.mcDutyNanoSec1 = duty_pct_to_ns(duty_pct, cfgA.mcPeriodNanoSec1);
    (void)pdm_apply(ENA_SEL, &cfgA);
}

void MotorB_Set(uint32 duty_pct, uint32 forward) {
    if (duty_pct == 0) {
        pin_lo(IN3); pin_lo(IN4);
    } else if (forward) {
        pin_hi(IN3); pin_lo(IN4);
    } else {
        pin_lo(IN3); pin_hi(IN4);
    }
    cfgB.mcDutyNanoSec1 = duty_pct_to_ns(duty_pct, cfgB.mcPeriodNanoSec1);
    (void)pdm_apply(ENB_SEL, &cfgB);
}

static void ConfigureServoPDM(uint32 channel, uint32 port, uint32 angle_deg) {
    PDMModeConfig_t pwm_cfg;
    uint32 duty_ns = 500000 + (angle_deg * (2000000 / 180));
    uint32 wait_cnt = 0;

    pwm_cfg.mcPortNumber = port;
    pwm_cfg.mcOperationMode = PDM_OUTPUT_MODE_PHASE_1;
    pwm_cfg.mcInversedSignal = 0;
    pwm_cfg.mcOutSignalInIdle = 0;
    pwm_cfg.mcLoopCount = 0;
    pwm_cfg.mcOutputCtrl = 0;
    pwm_cfg.mcPeriodNanoSec1 = 20000000;
    pwm_cfg.mcDutyNanoSec1 = duty_ns;
    pwm_cfg.mcPeriodNanoSec2 = 0;
    pwm_cfg.mcDutyNanoSec2 = 0;

    PDM_Disable(channel, PMM_ON);
    while (PDM_GetChannelStatus(channel)) {
        SAL_TaskSleep(1);
        if (++wait_cnt > 100) {
            mcu_printf("Timeout on channel %d\n", channel);
            return;
        }
    }
    if (PDM_SetConfig(channel, &pwm_cfg) != SAL_RET_SUCCESS) {
        mcu_printf("SetConfig fail (CH:%d)\n", channel);
        return;
    }
    if (PDM_Enable(channel, PMM_ON) != SAL_RET_SUCCESS) {
        mcu_printf("Enable fail (CH:%d)\n", channel);
        return;
    }
    mcu_printf("CH%d angle: %3d deg duty: %d ns\n", channel, angle_deg, duty_ns);
}


// ============================================
// CAN receive and dispatch
// ============================================
void ControlBreadBoardSensors(uint32 mId, uint8 nDataLength, sint8* pucData) {
    if (nDataLength == 0) return;

    switch (mId) {
        case VCP_IO_SERVO_ROTTEN:   // rotten gate servo
            ControlServo(1, (uint8)pucData[0]);
            mcu_printf("[CAN] SERVO_ROTTEN action=%d\r\n", pucData[0]);
            break;

        case VCP_IO_SERVO_UNRIPE:   // unripe gate servo
            ControlServo(2, (uint8)pucData[0]);
            mcu_printf("[CAN] SERVO_UNRIPE action=%d\r\n", pucData[0]);
            break;

        case VCP_IO_SERVO_OTHER:    // spare servo
            ControlServo(3, (uint8)pucData[0]);
            mcu_printf("[CAN] SERVO_OTHER action=%d\r\n", pucData[0]);
            break;

        case VCP_IO_SERVO_4:        // reserved servo
            ControlServo(4, (uint8)pucData[0]);
            mcu_printf("[CAN] SERVO_4 action=%d\r\n", pucData[0]);
            break;

        case VCP_IO_LED_CONTROL:    // LEDs
            ControlLED((uint8)pucData[0], (uint8)pucData[1]);
            mcu_printf("[CAN] LED id=%d action=%d\r\n", pucData[0], pucData[1]);
            break;

        case VCP_IO_CONVEYOR:       // conveyor
            ControlConveyor((uint8)pucData[0], (uint8)pucData[1]);
            mcu_printf("[CAN] CONVEYOR action=%d speed=%d\r\n", pucData[0], pucData[1]);
            break;

        case VCP_IO_LCD_STATS:      // LCD totals: total, fresh, rotten, unripe
            ControlLCDStats((uint8)pucData[0], (uint8)pucData[1],
                          (uint8)pucData[2], (uint8)pucData[3]);
            mcu_printf("[CAN] LCD total=%d fresh=%d rotten=%d unripe=%d\r\n",
                      pucData[0], pucData[1], pucData[2], pucData[3]);
            break;

        default:
            mcu_printf("[%s] undefined can id 0x%03X\r\n", __FUNCTION__, mId);
            break;
    }
}

#endif