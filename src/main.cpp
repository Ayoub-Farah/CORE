/*
 * Copyright (c) 2026-present LAAS-CNRS
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1
 */

/**
 * @brief  This example deploys the open-loop control of a MMC arm integrating a Capacitor Voltage Balancing algorithm using duty cycle ramping to reduce high-frequency oscillations from low-side LC filter. 
 *         This research was funded in whole by the French National Research Agency (ANR) under the project CARROTS "ANR-24-CE05-0920-01".
 *
 * @author Ayoub Farah Hassan <ayoub.farah-hassan@laas.fr>
 * @author Ana Luiza Haas Bezerra <ana-luiza.haas-bezerra@centralesupelec.fr>
 * @author Zaid Jabbar <zaid.jabbar@grenoble-inp.fr>
 * @author Luiz Villa <luiz.villa@laas.fr>
 * @author Jean Alinei <jean.alinei@owntech.org>
 * @author Noemi Lanciotti <noemi.lanciotti@centralesupelec.fr>
 * @author Loïc Quéval <loic.queval@centralesupelec.fr>
 */

/* --------------OWNTECH APIs---------------------------------- */
#include "SpinAPI.h"
#include "TaskAPI.h"
#include "ShieldAPI.h"
#include "CommunicationAPI.h"

/*--------------OWNTECH Libraries----------------------------- */
#include "filters.h"
#include "trigo.h"
#include "pid.h"
#include "pr.h"
#include "arm_math_types.h"
#include <ScopeMimicry.h>
#include <math.h>

/*-- Zephyr includes --*/
#include "zephyr/console/console.h"


#define MMC_LEAD 0
#define MMC_SM1 1
#define MMC_SM2 2
#define MMC_SM3 3
#define MMC_SM4 4
#define MMC_SM5 5
#define MMC_SM6 6
#define MMC_SM7 7
#define MMC_SM8 8
#define MMC_SM9 9
#define MMC_SM10 10

#define IDLE 0
#define POWER 1
#define LEAD_ERROR 2
#define OVER_VOLTAGE 3
#define UNDER_VOLTAGE 4
#define OVER_CURRENT 5

constexpr uint8_t MMC_SM_COUNT = 10;
constexpr uint8_t MMC_SM_FIRST = MMC_SM1;
constexpr uint8_t MMC_SM_LAST = MMC_SM10;

/* -------------- GENERAL MMC DEFINITIONS -------------------- */
/* --------------- To be changed by user --------------------- */

static const float f0 = 50.F; //[Hz] Output frequency used to generate the sinusoidal reference for open-loop control
static const uint8_t total_number_of_modules_arm = 5; //[-] Number of modules per arm
constexpr float32_t Vcap_expected = 80.0F; //[V] Capacitor DC voltage expected during the test (used to set voltage measurement scale for 12 bits)
constexpr float32_t i_expected = 10.0F; //[A] Expected current amplitude during test (used to set current measurement scale for 12 bits)
constexpr float32_t overvoltage_tolerance = 80.0F; //[V] Set overvoltage tolerance (default max TWIST voltage)
constexpr float32_t overcurrent_tolerance = 8.0F; //[A] Set overcurrent tolerance (default max TWIST current)

/* -------------- BOARD IDENTIFICATION ----------------------- */
/* --------------- To be changed by user --------------------- */

constexpr uint32_t UID_MMC_LEAD_BOARD = 0x002B002A;
constexpr uint32_t UID_MMC_SM1_BOARD = 0x0033004C;
constexpr uint32_t UID_MMC_SM2_BOARD = 0x0031001B;
constexpr uint32_t UID_MMC_SM3_BOARD = 0x00330049;
constexpr uint32_t UID_MMC_SM4_BOARD = 0x0033004B;
constexpr uint32_t UID_MMC_SM5_BOARD = 0x00330054;
constexpr uint32_t UID_MMC_SM6_BOARD = 0x0032003F;
constexpr uint32_t UID_MMC_SM7_BOARD = 0x004E0048;
constexpr uint32_t UID_MMC_SM8_BOARD = 0x00470026;
constexpr uint32_t UID_MMC_SM9_BOARD = 0x004C001D;
constexpr uint32_t UID_MMC_SM10_BOARD = 0x0047002B;

/* --------- BOARD IDENTIFICATION functions ------------------ */
static uint32_t read_board_uid()
{
    static volatile uint32_t *const uid0 =
        reinterpret_cast<volatile uint32_t *>(0x1FFF7590UL);
    return *uid0;
}

static uint8_t detect_module_id()
{
    switch (read_board_uid())
    {
    case UID_MMC_LEAD_BOARD:
        return MMC_LEAD;
    case UID_MMC_SM1_BOARD:
        return MMC_SM1;
    case UID_MMC_SM2_BOARD:
        return MMC_SM2;
    case UID_MMC_SM3_BOARD:
        return MMC_SM3;
    case UID_MMC_SM4_BOARD:
        return MMC_SM4;
    case UID_MMC_SM5_BOARD:
        return MMC_SM5;
    case UID_MMC_SM6_BOARD:
        return MMC_SM6;
    case UID_MMC_SM7_BOARD:
        return MMC_SM7;
    case UID_MMC_SM8_BOARD:
        return MMC_SM8;
    case UID_MMC_SM9_BOARD:
        return MMC_SM9;
    case UID_MMC_SM10_BOARD:
        return MMC_SM10;
    default:
        return MMC_SM1;
    }
}

/* -------------- DATA PACKING HELPERS ----------------------- */

constexpr float32_t Cap_voltage_SCALE = Vcap_expected*2; //[V] Scale to transform voltage measurements sent to 1 byte (256 values)
constexpr float32_t Arm_current_SCALE = i_expected*2; //[A] Scale to transform current measurements sent to 1 byte (256 values)
constexpr float32_t Arm_current_OFFSET = i_expected; //[A] Offset to transform current measurements sent to 1 byte, used to allow positive and negative values with expected amplitude

/**
 * @brief Encode an capacitor voltage into the 12-bit transport format.
 *
 * @param current Physical capacitor voltage in volts.
 * @return 12-bit encoded voltage suitable for MMC frames.
 */
static inline uint16_t mmc_encode_voltage(float32_t voltage)
{
    int32_t raw = static_cast<int32_t>((voltage * 4095.0F) / Cap_voltage_SCALE);
    if (raw < 0)
    {
        raw = 0;
    }
    if (raw > 0x0FFF)
    {
        raw = 0x0FFF;
    }
    return static_cast<uint16_t>(raw);
}

/**
 * @brief Decode a raw capacitor voltage value from an MMC frame.
 *
 * @param raw 12-bit encoded capacitor voltage.
 * @return Physical capacitor voltage in volts.
 */
static inline float32_t mmc_decode_voltage(uint16_t raw)
{
    return (Cap_voltage_SCALE * static_cast<float32_t>(raw & 0x0FFF)) / 4095.0F;
}

/**
 * @brief Encode an arm current into the 12-bit transport format.
 *
 * @param current Physical arm current in amperes.
 * @return 12-bit encoded current suitable for MMC frames.
 */
static inline uint16_t mmc_encode_current(float32_t current)
{
    float32_t shifted = current + Arm_current_OFFSET;
    int32_t raw = static_cast<int32_t>((shifted * 4095.0F) / Arm_current_SCALE);
    if (raw < 0)
    {
        raw = 0;
    }
    if (raw > 0x0FFF)
    {
        raw = 0x0FFF;
    }
    return static_cast<uint16_t>(raw);
}

/**
 * @brief Decode a raw arm current value from an MMC frame.
 *
 * @param raw 12-bit encoded arm current.
 * @return Physical arm current in amperes.
 */
static inline float32_t mmc_decode_current(uint16_t raw)
{
    return ((Arm_current_SCALE * static_cast<float32_t>(raw & 0x0FFF)) / 4095.0F) - Arm_current_OFFSET;
}




/* --------------SETUP FUNCTIONS DECLARATION------------------- */

/* Setups the hardware and software of the system */
void setup_routine();

/* --------------LOOP FUNCTIONS DECLARATION-------------------- */

/* Code to be executed in the background task - only sets up boards LEDs */
void loop_background_task();
/* Code to be executed in real time in the critical task - executes all LEAD and MODULES control logics */
void loop_critical_task();
/* Code to be executed in the communication task - serves to send command to board via PC using USB-C cable */
void loop_communication_task();

/* --------------USER VARIABLES DECLARATIONS------------------- */

/* Auto-detected module ID (uses dummy UIDs for now). */
uint8_t module_ID = detect_module_id(); // The ID of the module, can be set to MMC_LEAD or any other SMx

static uint8_t module_comand; // The command the followers needs to apply
static uint8_t module_command_past; // The command the followers applied in t-1 (last critical task)
static bool change_state_command = false; // Flag to change the state of the command
static bool send_idle = false;            // Flag to send idle command from master to followers

constexpr uint8_t MMC_STATUS_CODE_BITS = 3;
constexpr uint32_t MMC_STATUS_CODE_MASK = (1UL << MMC_STATUS_CODE_BITS) - 1U;
constexpr uint32_t MMC_STATUS_UPPER_ARM_MASK = (1UL << MMC_STATUS_CODE_BITS);

/**
 * @brief Frame exchanged over the RS485 communication bus.
 *
 * Structure overview:
 * - `sm_insertion`: bit-packed insertion flags for each submodule.
 * - `capacitor_voltage_raw`: 12-bit encoded capacitor voltage.
 * - `arm_current_raw`: 12-bit encoded arm current.
 * - `status`: 3-bit global status level plus the arm selection flag.
 * - `sm_id`: identifier of the sender (lead or submodule index).
 */
struct MMC_frame
{
    union
    {
        uint16_t raw;
        struct
        {
            uint16_t sm1_inserted : 1;
            uint16_t sm2_inserted : 1;
            uint16_t sm3_inserted : 1;
            uint16_t sm4_inserted : 1;
            uint16_t sm5_inserted : 1;
            uint16_t sm6_inserted : 1;
            uint16_t sm7_inserted : 1;
            uint16_t sm8_inserted : 1;
            uint16_t sm9_inserted : 1;
            uint16_t sm10_inserted : 1;
        } bits;
    } sm_insertion;
    uint16_t capacitor_voltage_raw : 12;
    uint16_t arm_current_raw : 12;
    union
    {
        uint8_t raw;
        struct
        {
            uint8_t status_code : MMC_STATUS_CODE_BITS;
            uint8_t upper_arm_frame : 1;
        } bits;
    } status;
    uint8_t sm_id;
} __packed;

typedef MMC_frame MMC_frame_t;

/**
 * @brief Store an encoded capacitor voltage value inside an MMC frame.
 *
 * @param frame Frame that will carry the voltage information.
 * @param raw 12-bit raw voltage to write into the frame.
 */
static inline void mmc_frame_set_voltage_raw(MMC_frame_t &frame, uint16_t raw)
{
    frame.capacitor_voltage_raw = static_cast<uint16_t>(raw & 0x0FFFU);
}

/**
 * @brief Get the encoded capacitor voltage contained in an MMC frame.
 *
 * @param frame Frame that carries the voltage information.
 * @return 12-bit raw capacitor voltage.
 */
static inline uint16_t mmc_frame_get_voltage_raw(const MMC_frame_t &frame)
{
    return static_cast<uint16_t>(frame.capacitor_voltage_raw & 0x0FFFU);
}

/**
 * @brief Store an encoded arm current value inside an MMC frame.
 *
 * @param frame Frame that will carry the current information.
 * @param raw 12-bit raw current to write into the frame.
 */
static inline void mmc_frame_set_current_raw(MMC_frame_t &frame, uint16_t raw)
{
    frame.arm_current_raw = static_cast<uint16_t>(raw & 0x0FFFU);
}

/**
 * @brief Get the encoded arm current contained in an MMC frame.
 *
 * @param frame Frame that carries the current information.
 * @return 12-bit raw arm current.
 */
static inline uint16_t mmc_frame_get_current_raw(const MMC_frame_t &frame)
{
    return static_cast<uint16_t>(frame.arm_current_raw & 0x0FFFU);
}

/**
 * @brief Set the submodule identifier associated with an MMC frame.
 *
 * @param frame Frame to update.
 * @param id Identifier of the sender (lead or submodule).
 */
static inline void mmc_frame_set_sm_identifier(MMC_frame_t &frame, uint8_t id)
{
    frame.sm_id = id;
}

/**
 * @brief Read the submodule identifier stored inside an MMC frame.
 *
 * @param frame Frame to inspect.
 * @return Sender identifier extracted from the frame.
 */
static inline uint8_t mmc_frame_get_sm_identifier(const MMC_frame_t &frame)
{
    return frame.sm_id;
}

/**
 * @brief Update the insertion flag for a given submodule in an MMC frame.
 *
 * @param frame Frame to modify.
 * @param sm_index Submodule identifier to update.
 * @param inserted Set to true if the submodule is inserted.
 */
static inline void mmc_frame_set_sm_inserted(MMC_frame_t &frame, uint8_t sm_index, bool inserted)
{
    if (sm_index < MMC_SM_FIRST || sm_index > MMC_SM_LAST)
    {
        return;
    }
    uint8_t shift = static_cast<uint8_t>(sm_index - MMC_SM_FIRST);
    uint16_t mask = static_cast<uint16_t>(1U << shift);
    if (inserted)
    {
        frame.sm_insertion.raw |= mask;
    }
    else
    {
        frame.sm_insertion.raw &= static_cast<uint16_t>(~mask);
    }
}

/**
 * @brief Check whether a submodule is marked as inserted in an MMC frame.
 *
 * @param frame Frame to inspect.
 * @param sm_index Submodule identifier to check.
 * @return True when the insertion flag is set, false otherwise.
 */
static inline bool mmc_frame_get_sm_inserted(const MMC_frame_t &frame, uint8_t sm_index)
{
    if (sm_index < MMC_SM_FIRST || sm_index > MMC_SM_LAST)
    {
        return false;
    }
    uint8_t shift = static_cast<uint8_t>(sm_index - MMC_SM_FIRST);
    uint16_t mask = static_cast<uint16_t>(1U << shift);
    return (frame.sm_insertion.raw & mask) != 0U;
}

/**
 * @brief Set the global status level encoded inside an MMC frame.
 *
 * @param frame Frame to modify.
 * @param status_code 3-bit status value (IDLE, POWER, error levels).
 */
static inline void mmc_frame_set_status_code(MMC_frame_t &frame, uint8_t status_code)
{
    frame.status.raw &= ~MMC_STATUS_CODE_MASK;
    frame.status.raw |= static_cast<uint32_t>(status_code & MMC_STATUS_CODE_MASK);
}

/**
 * @brief Retrieve the global status level encoded inside an MMC frame.
 *
 * @param frame Frame to inspect.
 * @return 3-bit status value (IDLE, POWER, error levels).
 */
static inline uint8_t mmc_frame_get_status_code(const MMC_frame_t &frame)
{
    return static_cast<uint8_t>(frame.status.raw & MMC_STATUS_CODE_MASK);
}

/**
 * @brief Mark whether the frame data describes the upper arm.
 *
 * @param frame Frame to update.
 * @param is_upper_arm True when the frame belongs to the upper arm.
 */
static inline void mmc_frame_set_upper_arm_flag(MMC_frame_t &frame, bool is_upper_arm)
{
    if (is_upper_arm)
    {
        frame.status.raw |= MMC_STATUS_UPPER_ARM_MASK;
    }
    else
    {
        frame.status.raw &= ~MMC_STATUS_UPPER_ARM_MASK;
    }
}

/**
 * @brief Determine whether the MMC frame is associated with the upper arm.
 *
 * @param frame Frame to inspect.
 * @return True when the upper arm flag is set, false otherwise.
 */
static inline bool mmc_frame_is_upper_arm(const MMC_frame_t &frame)
{
    return (frame.status.raw & MMC_STATUS_UPPER_ARM_MASK) != 0U;
}

/**
 * @brief Determine if a module identifier corresponds to the upper arm.
 *
 * @param id Module identifier under test.
 * @return True when the module belongs to the upper arm side.
 */
static inline bool mmc_is_upper_arm_module(uint8_t id)
{
    if (id == MMC_LEAD)
    {
        return true;
    }
    if (id < MMC_SM_FIRST || id > MMC_SM_LAST)
    {
        return false;
    }
    uint8_t offset = static_cast<uint8_t>(id - MMC_SM_FIRST);
    return offset < (MMC_SM_COUNT / 2);
}

static MMC_frame_t dataTX_mmc;
static MMC_frame_t dataRX_mmc;

float32_t MMC_capacitor_voltage[MMC_SM_COUNT];
float32_t MMC_arm_current[MMC_SM_COUNT];

constexpr size_t MMC_FRAME_SIZE = sizeof(MMC_frame_t);

uint8_t buffer_tx[MMC_FRAME_SIZE];
uint8_t buffer_rx[MMC_FRAME_SIZE];

float32_t Cap_voltage = 0.0f;
static float32_t Arm_current = 0.0f;

uint32_t counter_timer = 0;
uint32_t counter_receive = 0;

uint8_t received_serial_char; // Variable to store the received character from the serial interface
int8_t CommTask_num;

static bool master = false;

/* --------------- LIST OF POSSIBLE BOARD MODES ------------------*/
enum serial_interface_menu_mode
{
    IDLEMODE = 0, // Related to blocked state
    POWERMODE = 1, // Related to connected/disconnected state
};

serial_interface_menu_mode mode = IDLEMODE;

/* --------------- Firmware CVB variables ------------------*/

/* [us] period of the control task (=critical task) */
static constexpr uint32_t control_task_period = 200; // µs
static float32_t Ts = control_task_period * 1e-6F; // s
/* [bool] state of the PWM (ctrl task) */
static bool pwm_enable = false;

static uint32_t critical_task_timer = 0; 

/* Measure variables */

static float32_t V1_low_value;
static float32_t V2_low_value;
static float32_t I1_low_value;
static float32_t I2_low_value;
static float32_t I_high;
static float32_t V_high;

static float32_t temp_1_value;
static float32_t temp_2_value;

/* Temporary storage for measured value (ctrl task) */
static float meas_data;

/* Scope variables */
static bool enable_acq; // Sets trigger moment if true
static const uint16_t NB_DATAS = 1028; // Number of data acquired
static ScopeMimicry scope(NB_DATAS, 14); // Scope configuration with 5 channels
static bool is_downloading; // Records data if true
static uint32_t scope_timer = 0;
static uint32_t scope_period = 1; // scope acquire data every t = scope_period * critical_task_period (100 µs) s;

/* CVB variables */

static float32_t number_of_connected_submodules_upper_arm; // Stores number of modules connected in the upper arm (NLM output)
static float32_t number_of_connected_submodules_lower_arm; // Stores number of modules connected in the lower arm (NLM output)
static float32_t i_upper_arm= 1.0F; // Upper arm current - will be updated with physical current measure during test execution
static float32_t i_lower_arm= -1.0F; // Lower arm current - will be updated with physical current measure during test execution

/* Gate logic */
uint8_t g_u[total_number_of_modules_arm]; // Gate signals to send to the upper modules
uint8_t g_l[total_number_of_modules_arm]; // Gate signals to send to the lower modules
static float32_t g_u_1; // Gate signal M1 - Used for gate signal acquisition by scopemimicry
static float32_t g_u_2; // Gate signal M2 - Used for gate signal acquisition by scopemimicry
static float32_t g_u_3; // Gate signal M3 - Used for gate signal acquisition by scopemimicry
static float32_t g_u_4; // Gate signal M4 - Used for gate signal acquisition by scopemimicry
static float32_t g_u_5; // Gate signal M5 - Used for gate signal acquisition by scopemimicry
static float32_t g_l_1; // Gate signal M6 - Used for gate signal acquisition by scopemimicry
static float32_t g_l_2; // Gate signal M7 - Used for gate signal acquisition by scopemimicry
static float32_t g_l_3; // Gate signal M8 - Used for gate signal acquisition by scopemimicry
static float32_t g_l_4; // Gate signal M9 - Used for gate signal acquisition by scopemimicry
static float32_t g_l_5; // Gate signal M10 - Used for gate signal acquisition by scopemimicry

/* NLM */
static float32_t m = 1; // Modulation amplitude
static float32_t a = 1; // Modulation dc part
static float32_t angle;
static const float w0 = 2 * PI * f0; // Angular frequency

static float32_t modulation_signal_upper; //[pu] Modulation output upper voltage
static float32_t modulation_signal_lower; //[pu] Modulation output lower voltage

/* Current measurement filter */

LowPassFirstOrderFilter i_low_filter(Ts, 160e-6F); // Lowpass filter with tau = 180µs -> fc = 880 Hz
static float32_t i_lowfilter_value;

/* Oscillations treatment with duty cycle ramping */
static float32_t duty_cycle = 0.0F; // Applied duty cycle
static constexpr uint32_t duty_cycle_ramp_size = 4U; // How many intermediate steps
static float32_t duty_cycle_ramp_up[duty_cycle_ramp_size] = {0.25F, 0.5F, 0.75F, 0.95F}; // Duty cycle ramp in 4 levels to reduce oscillations
static float32_t duty_cycle_ramp_down[duty_cycle_ramp_size] = {0.75F, 0.5F, 0.25F, 0.0F}; // Duty cycle ramp in 4 levels to reduce oscillations
static constexpr uint32_t duty_cycle_ramp_step_period_us = 500U; // Intermediate steps period
static constexpr uint32_t duty_cycle_ramp_step_ticks =
    (duty_cycle_ramp_step_period_us + control_task_period - 1U) / control_task_period; // Time where the steps are going to be applied
uint32_t duty_cycle_counter = 0;
uint32_t duty_cycle_step_counter = 0;

/* Ramping functions */
static inline void duty_cycle_ramp_reset() // Resets duty cycle ramp
{
    duty_cycle_counter = 0U;
    duty_cycle_step_counter = 0U;
}

static inline void duty_cycle_ramp_apply(bool module_inserted) // Applied duty cycle ramp
{
    const float32_t *ramp = module_inserted ? duty_cycle_ramp_up : duty_cycle_ramp_down; // Ramp up if module is inserted - Ramp down if module is disconnected
    const float32_t target_final = ramp[duty_cycle_ramp_size - 1U];

    if (duty_cycle_counter < duty_cycle_ramp_size)
    {
        if (duty_cycle_step_counter == 0U)
        {
            duty_cycle = ramp[duty_cycle_counter];
            duty_cycle_counter++;
        }

        duty_cycle_step_counter++;
        if (duty_cycle_step_counter >= duty_cycle_ramp_step_ticks)
        {
            duty_cycle_step_counter = 0U;
        }
    }
    else
    {
        duty_cycle = target_final;
    }

    shield.power.setDutyCycle(LEG1, duty_cycle);
}


/* --------------SETUP FUNCTIONS------------------------------- */

/* Function to control the LEDs in the low level */
void config_led_LL()
{
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT);
    LL_GPIO_SetPinSpeed(GPIOA, LL_GPIO_PIN_5, LL_GPIO_SPEED_FREQ_VERY_HIGH);
    LL_GPIO_SetPinOutputType(GPIOA, LL_GPIO_PIN_5, LL_GPIO_OUTPUT_PUSHPULL);
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_5, LL_GPIO_PULL_NO);
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
}

inline void Led_turnON_LL()
{
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_5);
}

inline void Led_turnOFF_LL()
{
    LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_5);
}

/* Trigger function for scope manager */
bool a_trigger()
{
    return enable_acq;
}

/* Records scope data */
void dump_scope_datas(ScopeMimicry &scope)
{
    uint8_t *buffer = scope.get_buffer();
    /* We divide by 4 (4 bytes per float data) */
    uint16_t buffer_size = scope.get_buffer_size() >> 2;
    printk("begin record\n");
    printk("#");
    for (uint16_t k = 0; k < scope.get_nb_channel(); k++)
    {
        printk("%s,", scope.get_channel_name(k));
    }
    printk("\n");
    printk("# %d\n", scope.get_final_idx());
    for (uint16_t k = 0; k < buffer_size; k++)
    {
        printk("%08x\n", *((uint32_t *)buffer + k));
        task.suspendBackgroundUs(100);
    }
    printk("end record\n");
}

static void update_measurements(void)
{
    float32_t latest = shield.sensors.getLatestValue(V_HIGH);
    if (latest != NO_VALUE)
    {
        V_high = latest;
        Cap_voltage = V_high;
    }

    latest = shield.sensors.getLatestValue(I1_LOW);
    if (latest != NO_VALUE)
    {
        I1_low_value = latest;
        Arm_current = -I1_low_value;
    }
}

void reception_function(void)
{
    dataRX_mmc = *(MMC_frame_t *)buffer_rx;
    uint8_t sender_id = mmc_frame_get_sm_identifier(dataRX_mmc);
    uint8_t status_code = mmc_frame_get_status_code(dataRX_mmc);

    if (module_ID == MMC_LEAD)
    {
        if ((sender_id >= MMC_SM_FIRST) && (sender_id <= MMC_SM_LAST))
        {
            const uint8_t index = sender_id - MMC_SM_FIRST;
            MMC_capacitor_voltage[index] =
                mmc_decode_voltage(mmc_frame_get_voltage_raw(dataRX_mmc));
            MMC_arm_current[index] =
                mmc_decode_current(mmc_frame_get_current_raw(dataRX_mmc));

            if ((status_code >= LEAD_ERROR) && (mode != IDLEMODE))
            {
                mode = IDLEMODE;
                send_idle = false;
            }
        }
    }

    else
    {
        if (sender_id == MMC_LEAD)
        {
            /* retrieving command from lead message*/
            module_comand = static_cast<uint8_t>(
                mmc_frame_get_sm_inserted(dataRX_mmc, module_ID));

            /* retrieving status */
            if (status_code == POWER)
            {
                mode = POWERMODE;
            }
            else
            {
                mode = IDLEMODE;
            }
        }

        /* The board following the ID of the one who sent will start sending
            the next message */
        if (sender_id == static_cast<uint8_t>(module_ID - 1))
        {
            dataTX_mmc = dataRX_mmc; // Copy the received data to the transmission data
            mmc_frame_set_sm_identifier(dataTX_mmc, module_ID);
            mmc_frame_set_upper_arm_flag(dataTX_mmc, mmc_is_upper_arm_module(module_ID));
            mmc_frame_set_voltage_raw(dataTX_mmc,
                                      mmc_encode_voltage(Cap_voltage));
            mmc_frame_set_current_raw(dataTX_mmc,
                                      mmc_encode_current(Arm_current));
            
            /* Verifies overvoltage protection criteria */
            if(Cap_voltage > overvoltage_tolerance)
            {
                // mmc_frame_set_status_code(dataTX_mmc, OVER_VOLTAGE);
                mmc_frame_set_status_code(dataTX_mmc, POWER);
            }
            /* Verifies overcurrent protection criteria */
            else if(Arm_current > overcurrent_tolerance)
            {
                // mmc_frame_set_status_code(dataTX_mmc, OVER_CURRENT);
                mmc_frame_set_status_code(dataTX_mmc, POWER);
            }
            else{
                mmc_frame_set_status_code(dataTX_mmc, POWER);
            }
            memcpy(buffer_tx, &dataTX_mmc, sizeof(dataTX_mmc));

            communication.rs485.startTransmission();
            
        }
    }
    counter_receive++;
}


/**
 * This is the setup routine.
 * It is used to call functions that will initialize your spin, power shields
 * and tasks.
 */
void setup_routine()
{
    /* Informs the module ID in the terminal */
    const uint32_t board_uid = read_board_uid();
    printk("Board UID: 0x%08" PRIX32 "\n", board_uid);
    printk("Module ID : %u \n", module_ID);
    master = (module_ID == MMC_LEAD);

    config_led_LL(); // Configure the LED pin in Low Level

    shield.power.initBuck(ALL);
    /* Declare task */
    uint32_t background_task_number =
        task.createBackground(loop_background_task);

    task.createCritical(loop_critical_task, control_task_period);

    shield.sensors.enableDefaultTwistSensors();

    if(module_ID == MMC_SM1)
    {
        shield.sensors.setConversionParametersLinear(V_HIGH,0.0297809746442154,0.0816717736324648);
        shield.sensors.setConversionParametersLinear(V1_LOW,0.0447118735275233,-85.4652963581883);
        shield.sensors.setConversionParametersLinear(V2_LOW,0.044724349440928,-85.9790466977148);
        shield.sensors.setConversionParametersLinear(I1_LOW,0.00572228696154378,-12.9647582710024);
        shield.sensors.setConversionParametersLinear(I2_LOW,0.00573807392353278,-12.98795596087);
        shield.sensors.setConversionParametersLinear(I_HIGH,0.00505978197917605,-9.74527087864709);

    }
    if(module_ID == MMC_SM6)
    {
        shield.sensors.setConversionParametersLinear(V_HIGH,0.0297391039983399,0.323800681173033);
        shield.sensors.setConversionParametersLinear(V1_LOW,0.0449253808069436,-87.8192961435065);
        shield.sensors.setConversionParametersLinear(V2_LOW,0.0455433243875255,-87.419941665916);
        shield.sensors.setConversionParametersLinear(I1_LOW,0.00564375060446823,-12.3417089599044);
        shield.sensors.setConversionParametersLinear(I2_LOW,0.00473940070187198,-10.5860385444491);
        shield.sensors.setConversionParametersLinear(I_HIGH,0.00523739527489416,-10.2030551937385);
    }

    /* Disconnect electrolytical capacitors from low-side */
    shield.power.disconnectCapacitor(LEG1);
    shield.power.disconnectCapacitor(LEG2);

    /* Enable switch control with max and min duty cycle of 1 and 0 */
    shield.power.setDutyCycleMax(ALL,1.0);
    shield.power.setDutyCycleMin(ALL,0.0);

    /* Finally, start tasks */
    task.startBackground(background_task_number);

    task.startCritical();
    CommTask_num = task.createBackground(loop_communication_task);
    task.startBackground(CommTask_num);

    communication.rs485.configure(buffer_tx, buffer_rx, sizeof(buffer_rx),
                                  reception_function,
                                  SPEED_20M); // custom configuration for RS485
                                              /* Configure scope channels, what measurements do you want to acquire? */
    if (master == true)
    {
        /* Defines lead's clock as reference for communication synchorinization */
        communication.sync.initMaster();

        /* Configures scopemimicry measured variables */
        scope.connectChannel(number_of_connected_submodules_upper_arm, "N_u");
        scope.connectChannel(number_of_connected_submodules_lower_arm, "N_l");
        scope.connectChannel(MMC_capacitor_voltage[5], "v_c_6");
        scope.connectChannel(MMC_capacitor_voltage[6], "v_c_7");
        scope.connectChannel(MMC_capacitor_voltage[7], "v_c_8");
        scope.connectChannel(MMC_capacitor_voltage[8], "v_c_9");
        scope.connectChannel(MMC_capacitor_voltage[9], "v_c_10");
        scope.connectChannel(MMC_capacitor_voltage[0], "v_c_1");
        scope.connectChannel(MMC_capacitor_voltage[1], "v_c_2");
        scope.connectChannel(MMC_capacitor_voltage[2], "v_c_3");
        scope.connectChannel(MMC_capacitor_voltage[3], "v_c_4");
        scope.connectChannel(MMC_capacitor_voltage[4], "v_c_5");
        scope.connectChannel(i_upper_arm, "i_u");
        scope.connectChannel(i_lower_arm, "i_l");
        // scope.connectChannel(i_lowfilter_value, "i_u_filtered");
        scope.set_trigger(&a_trigger);
        scope.set_delay(0.0F);
        scope.start();

    }
    else{
        /* Defines module as follower for communication synchorinization */
        communication.sync.initSlave();
    }
}

/* --------------LOOP FUNCTIONS-------------------------------- */

/**
 * This is the communication task.
 * It is used to send to the board via the computer the desired mode
 * IDLE (i) = block all modules or POWER (p) = operate MMC arm with CVB.
 * 
 * It also sends data acquisition start command (a) and scope data retrieve commands (r).
 */
void loop_communication_task()
{
    received_serial_char = console_getchar();

    switch (received_serial_char)
    {
    case 'h':
        /*----------SERIAL INTERFACE MENU----------------------- */
        printk(" ________________________________________ \n"
               "|     ---- MENU buck voltage mode ----   |\n"
               "|     press i : idle mode                |\n"
               "|     press p : power mode               |\n"
               "|     press r : record data              |\n"
               "|     press a : toggle enable_acq var    |\n"
               "|________________________________________|\n\n");
        /*------------------------------------------------------ */
        break;
    case 'i':
        printk("idle mode\n");
        mode = IDLEMODE;
        break;
    case 'p':
        printk("power mode\n");
        mode = POWERMODE;
        send_idle = false; // Set the flag to send idle command to false 
        break;
    case 'r':
        is_downloading = true;
        break;
    case 'a':
        enable_acq = !(enable_acq);
        break;
    default:
        break;
    }
}

/**
 * This is the code loop of the background task
 * It runs perpetually. Here a `suspendBackgroundMs` is used to pause during
 * 2000ms between each LED toggles.
 * Hence we expect the LED to blink each 2 seconds.
 */
void loop_background_task()
{
    if (module_ID == MMC_LEAD)
    {
        if (mode == IDLEMODE)
        {
            spin.led.turnOff();
            if (is_downloading)
            {
                dump_scope_datas(scope);
                is_downloading = false;
            }
        }
        if (mode == POWERMODE)
        {
            spin.led.toggle();
        }
    }

    task.suspendBackgroundMs(2000);
}

/* Consensus helpers adapted from Zaid_Code/software/src/mmc_local_consensus.hpp (MIT). */
/* Local Consensus / neighbor-consensus gains */
constexpr float MMC_CONSENSUS_K_V = 0.2F;
constexpr float MMC_CONSENSUS_K_NEIGHBOR_ORDER = 0.02F;
constexpr float MMC_VOLTAGE_DEADBAND_V = 0.1F;
constexpr float MMC_CURRENT_SCALE_A = 1.0F;


static inline float mmc_smooth_current_direction(float arm_current)
{
    /*
     * Normalize the arm current by MMC_CURRENT_SCALE_A and limit the result to
     * -1 or +1 when the normalized magnitude reaches 3. Between these limits,
     * x*(27 + x^2)/(27 + 9*x^2) approximates tanh(x) at a lower computation cost.
     * The result smoothly sets the direction and strength of voltage balancing.
     */
    constexpr float inv_current_scale = 1.0F / MMC_CURRENT_SCALE_A;
    const float x = arm_current * inv_current_scale;

    if (x >= 3.0F) return 1.0F;
    if (x <= -3.0F) return -1.0F;

    const float x2 = x * x;
    return x * (27.0F + x2) / (27.0F + 9.0F * x2);
}

static inline float mmc_consensus_neighbor_error(float vc_i, float vc_prev, float vc_next)
{
    float err = 0.5F * (vc_prev + vc_next) - vc_i;
    if (fabsf(err) < MMC_VOLTAGE_DEADBAND_V) err = 0.0F;
    return err;
}

static inline float mmc_local_consensus_priority_from_neighbors(float vc_i,
                                                                float vc_prev,
                                                                float vc_next,
                                                                float arm_current)
{
    const float err = mmc_consensus_neighbor_error(vc_i, vc_prev, vc_next);

    float neighbor_order_score = 0.0F;
    neighbor_order_score += (vc_i > vc_prev) ? 1.0F : 0.0F;
    neighbor_order_score += (vc_i > vc_next) ? 1.0F : 0.0F;

    const float neighbor_order_centered = neighbor_order_score - 1.0F;
    const float dir = mmc_smooth_current_direction(arm_current);

    return (MMC_CONSENSUS_K_V * err * dir)
           - (MMC_CONSENSUS_K_NEIGHBOR_ORDER * neighbor_order_centered * dir);
}

static inline float mmc_local_consensus_priority(uint8_t local_index,
                                                 const float *vc,
                                                 uint8_t n,
                                                 float arm_current)
{
    const uint8_t prev_index = static_cast<uint8_t>((local_index + n - 1U) % n);
    const uint8_t next_index = static_cast<uint8_t>((local_index + 1U) % n);
    return mmc_local_consensus_priority_from_neighbors(vc[local_index],
                                                       vc[prev_index],
                                                       vc[next_index],
                                                       arm_current);
}

static inline void mmc_clear_gates(uint8_t *gates, uint8_t n)
{
    for (uint8_t i = 0; i < n; ++i) gates[i] = 0U;
}

static inline void mmc_select_top_consensus_priorities(const float *priority,
                                                       uint8_t n,
                                                       uint8_t n_insert,
                                                       uint8_t *gates)
{
    mmc_clear_gates(gates, n);

    if (n_insert > n) n_insert = n;

    for (uint8_t selected = 0; selected < n_insert; ++selected)
    {
        float best_value = -1.0e30F;
        uint8_t best_index = 0U;

        for (uint8_t i = 0; i < n; ++i)
        {
            if ((gates[i] == 0U) && (priority[i] > best_value))
            {
                best_value = priority[i];
                best_index = i;
            }
        }

        gates[best_index] = 1U;
    }
}

/* Each arm forms its own neighbor ring, in physical module order. */
static void assign_arm_gates_local_consensus(const float32_t *voltages,
                                             float32_t arm_current,
                                             uint8_t number_to_insert,
                                             uint8_t *gates)
{
    float priorities[total_number_of_modules_arm];
    for (uint8_t index = 0; index < total_number_of_modules_arm; ++index)
    {
        priorities[index] = mmc_local_consensus_priority(
            index, voltages, total_number_of_modules_arm, arm_current);
    }
    mmc_select_top_consensus_priorities(
        priorities, total_number_of_modules_arm, number_to_insert, gates);
}

/**
 * This is the code loop of the critical task
 * It is executed every 100 micro-seconds defined in the setup_software
 * function.
 *
 * In the critical task, we implement the MMC control algorithms that will
 * run in Real Time.
 */
void loop_critical_task()
{
    update_measurements();

    if (mode == POWERMODE)
    {
        
        if (module_ID == MMC_LEAD) //CONTROL INSIDE LEAD - Modulation NLM + CVB algorithm execution
        {
            /* Modulation signal generation in open-loop */
            angle += w0 * Ts;
            angle = ot_modulo_2pi(angle);
            m = 1;
            modulation_signal_upper = (a + m * ot_sin(angle)) / (2.0);
            modulation_signal_lower = (a - m * ot_sin(angle)) / (2.0);

            /* Number of modules N_on to be connected on the arm according to modulation signal by Nearest Level Modulation (NLM)  */
            number_of_connected_submodules_upper_arm = round(total_number_of_modules_arm*modulation_signal_upper); 
            number_of_connected_submodules_lower_arm = round(total_number_of_modules_arm*modulation_signal_lower);

            /* Updating arm current measurements and filtering */
            // i_upper_arm = 1.0F; // We get the current from module 1
            // i_lower_arm = 1.0F; // We get the current from module 6
            i_upper_arm = MMC_arm_current[0]-0.8F; // We get the current from module 1
            i_lower_arm = MMC_arm_current[5]+0.19F; // We get the current from module 6
            // i_lowfilter_value = i_low_filter.calculateWithReturn(i_upper_arm); // filtered current value
            // i_upper_arm = i_lowfilter_value;
            // i_lowfilter_value = i_low_filter.calculateWithReturn(i_lower_arm); // filtered current value
            // i_lower_arm = i_lowfilter_value;

            /* Recompute consensus every cycle, even if the NLM level is unchanged. */
            assign_arm_gates_local_consensus(
                MMC_capacitor_voltage, i_upper_arm,
                static_cast<uint8_t>(number_of_connected_submodules_upper_arm), g_u);
            assign_arm_gates_local_consensus(
                &MMC_capacitor_voltage[total_number_of_modules_arm], i_lower_arm,
                static_cast<uint8_t>(number_of_connected_submodules_lower_arm), g_l);

            dataTX_mmc.sm_insertion.raw = 0U;

            /* Modules state assignment according to CVB output */
            for (uint8_t counter = 0; counter < total_number_of_modules_arm; counter++) {
                // mmc_frame_set_sm_inserted(trame_communication, module function (SM1,SM2,SM3,...), g_u associated to the module (SM1,SM2,SM3,...))
                // g_u[counter]: true = connected, false = disconnected
                mmc_frame_set_sm_inserted(dataTX_mmc, MMC_SM1 + counter, g_u[counter] != 0U);
                mmc_frame_set_sm_inserted(dataTX_mmc, MMC_SM6 + counter, g_l[counter] != 0U);
            }

            /* Fills all other communication trame spaces */
            dataTX_mmc.status.raw = 0U; // Reset status code

            mmc_frame_set_status_code(dataTX_mmc, POWER);
            mmc_frame_set_upper_arm_flag(dataTX_mmc, mmc_is_upper_arm_module(module_ID));
            mmc_frame_set_sm_identifier(dataTX_mmc, module_ID);
            mmc_frame_set_voltage_raw(dataTX_mmc, mmc_encode_voltage(Cap_voltage));
            mmc_frame_set_current_raw(dataTX_mmc, mmc_encode_current(Arm_current));
            memcpy(buffer_tx, &dataTX_mmc, sizeof(dataTX_mmc));

            /* LEAD communicates to MODULES */
            communication.rs485.startTransmission();

            /* Scope data acquisition */
            g_u_1 = (float)g_u[0];  // recuperate for scope acquisition
            g_u_2 = (float)g_u[1];  // recuperate for scope acquisition
            g_u_3 = (float)g_u[2];  // recuperate for scope acquisition
            g_u_4 = (float)g_u[3];  // recuperate for scope acquisition
            g_u_5 = (float)g_u[4];  // recuperate for scope acquisition

            if (scope_timer == scope_period)
            {
                scope.acquire();
                scope_timer = 0;
            }
            scope_timer++;
            critical_task_timer++;
        }
        else //CONTROL INSIDE MODULE - own switching only
        {
            /* Verifies if module received command changed */
            if (module_comand != module_command_past)
            {
                change_state_command = true; // Set the flag to change the state
            }

            //If command is 1, module changes to connected state
            if (module_comand)
            {
                if (change_state_command)
                {
                    change_state_command = false; // Reset the flag
                }
                shield.power.setDutyCycle(LEG1,1.0); // Duty cycle = 1.0 makes Q1 mostly closed and Q2 mostly open
                if (!pwm_enable)
                {
                    pwm_enable = true;
                    shield.power.start(LEG1);
                }
            }
            
            //if command is 2, module changes to blocked state (not used)
            else if (module_comand == 2)
            {
                if (change_state_command)
                {
                    change_state_command = false; // Reset the flag
                }
                if (pwm_enable == true)
                {
                    shield.power.stop(ALL); // Makes Q1 open and Q2 open
                }
                pwm_enable = false;
            }

            //if command is 0, module changes to disconnected state
            else
            {
                if (change_state_command)
                {
                    change_state_command = false; // Reset the flag
                }
                shield.power.setDutyCycle(LEG1,0.0); // Duty cycle = 0 makes Q1 open and Q2 closed
                if (!pwm_enable)
                {
                    pwm_enable = true;
                    shield.power.start(LEG1);
                }
            }
            critical_task_timer++;
        } 
        module_command_past = module_comand; // Update the past command

    }
    else if (mode == IDLEMODE)
    {
        /* Made  such that the LEAD send IDLE flag only once to all modules */
        if (!send_idle && module_ID == MMC_LEAD)
        {
            dataTX_mmc.sm_insertion.raw = 0U;
            dataTX_mmc.status.raw = 0U;
            mmc_frame_set_status_code(dataTX_mmc, IDLE);
            mmc_frame_set_upper_arm_flag(dataTX_mmc, mmc_is_upper_arm_module(module_ID));
            mmc_frame_set_sm_identifier(dataTX_mmc, module_ID);
            mmc_frame_set_voltage_raw(dataTX_mmc, mmc_encode_voltage(Cap_voltage));
            mmc_frame_set_current_raw(dataTX_mmc, mmc_encode_current(Arm_current));
            memcpy(buffer_tx, &dataTX_mmc, sizeof(dataTX_mmc));
            communication.rs485.startTransmission();
            send_idle = true; // Set the flag to send idle command
        }
        if (pwm_enable == true)
        {
            shield.power.stop(ALL);
        }
        pwm_enable = false;
    }
    counter_timer++;
}

/**
 * This is the main function of this example
 * This function is generic and does not need editing.
 */
int main(void)
{
    setup_routine();

    return 0;
}