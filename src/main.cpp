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
 * @brief  Distributed MMC voltage balancing. SM1 broadcasts the sine reference.
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
#include "trigo.h"
#include "arm_math_types.h"
#include <ScopeMimicry.h>
#include <math.h>

/*-- Zephyr includes --*/
#include "zephyr/console/console.h"


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
#define OVER_VOLTAGE 3
#define UNDER_VOLTAGE 4
#define OVER_CURRENT 5
#define COMMUNICATION_ERROR 6

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
        return 0; // An unknown board must never become a second SM1.
    }
}

/* -------------- DATA PACKING HELPERS ----------------------- */

constexpr float32_t Cap_voltage_SCALE = Vcap_expected*2; //[V] Scale for 12-bit voltage measurements
constexpr float32_t Arm_current_SCALE = i_expected*2; //[A] Scale for 12-bit current measurements
constexpr float32_t Arm_current_OFFSET = i_expected; //[A] Offset for signed current measurements

// Signed transport of sin(angle), without the modulation amplitude m.
// -32768 is reserved as invalid; all boards decode the same quantized reference.
constexpr int16_t MMC_SINE_REFERENCE_MAX = 32767;
constexpr int16_t MMC_SINE_REFERENCE_INVALID = -32768;

static inline int16_t mmc_encode_sine_reference(float32_t sine)
{
    if (!isfinite(sine)) return MMC_SINE_REFERENCE_INVALID;
    if (sine >= 1.0F) return MMC_SINE_REFERENCE_MAX;
    if (sine <= -1.0F) return -MMC_SINE_REFERENCE_MAX;
    return static_cast<int16_t>(roundf(sine * MMC_SINE_REFERENCE_MAX));
}

static inline float32_t mmc_decode_sine_reference(int16_t raw)
{
    return static_cast<float32_t>(raw) / MMC_SINE_REFERENCE_MAX;
}

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
/* Code to be executed in real time in the critical task - executes the control of each module */
void loop_critical_task();
/* Code to be executed in the communication task - serves to send command to board via PC using USB-C cable */
void loop_communication_task();

/* --------------USER VARIABLES DECLARATIONS------------------- */

// Only known boards can participate; SM1 generates the common sine reference.
uint8_t module_ID = detect_module_id();
static uint8_t module_command = 0;
static bool power_requested = false; // SM1: a new 'p' is required after every fault.
static volatile char requested_command = 0; // Console publishes; control consumes.

// One frame per module. Only SM1's sine reference is used as a command.
// The first two bytes replace the old insertion counts: update all boards together.
struct MMC_frame_t
{
    int16_t sine_reference_raw;
    uint16_t cycle_id;
    uint16_t capacitor_voltage_raw : 12;
    uint16_t arm_current_raw : 12;
    uint8_t status;
    uint8_t sm_id;
} __packed;

static bool mmc_is_upper_arm_module(uint8_t id)
{
    return id >= MMC_SM1 && id <= MMC_SM5;
}

static MMC_frame_t cycle_command;
static MMC_frame_t next_command; // Prepared by SM1 for the next communication window.
static bool cycle_started = false;
static bool measurement_received[MMC_SM_COUNT];
static uint8_t received_module_count = 0;
static volatile uint8_t communication_fault = 0;

// Communication is assumed to finish between control tasks, without overlap.
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


/* --------------- LIST OF POSSIBLE BOARD MODES ------------------*/
enum serial_interface_menu_mode
{
    IDLEMODE = 0, // Related to blocked state
    POWERMODE = 1, // Related to connected/disconnected state
};

serial_interface_menu_mode mode = IDLEMODE;

/* --------------- Firmware CVB variables ------------------*/

/* [us] period of the control task (=critical task) */
static constexpr uint32_t control_task_period = 200; // us
static float32_t Ts = control_task_period * 1e-6F; // s
/* [bool] state of the PWM (ctrl task) */
static bool pwm_enable = false;

static uint32_t critical_task_timer = 0; 

/* Scope variables */
static bool enable_acq; // Sets trigger moment if true
static const uint16_t NB_DATAS = 1028; // Number of data acquired
static ScopeMimicry scope(NB_DATAS, 14); // Scope configuration with 14 channels
static bool is_downloading; // Records data if true
static uint32_t scope_timer = 0;
static uint32_t scope_period = 1; // scope acquire data every t = scope_period * critical_task_period (200 µs) s;

/* CVB variables */

static float32_t number_of_connected_submodules_upper_arm; // Stores number of modules connected in the upper arm (NLM output)
static float32_t number_of_connected_submodules_lower_arm; // Stores number of modules connected in the lower arm (NLM output)
static float32_t i_upper_arm= 1.0F; // Upper arm current - will be updated with physical current measure during test execution
static float32_t i_lower_arm= -1.0F; // Lower arm current - will be updated with physical current measure during test execution

/* Gate logic */
uint8_t g_u[total_number_of_modules_arm]; // Gate signals to send to the upper modules
uint8_t g_l[total_number_of_modules_arm]; // Gate signals to send to the lower modules
/* NLM */
static float32_t m = 1; // Modulation amplitude: identical on every board
static float32_t a = 1; // Modulation dc part: identical on every board
static float32_t angle;
static const float w0 = 2 * PI * f0; // Angular frequency

static float32_t modulation_signal_upper; //[pu] Modulation output upper voltage
static float32_t modulation_signal_lower; //[pu] Modulation output lower voltage

// NLM runs on every board, including SM1, using the completed round's reference.
static bool mmc_compute_insertion_counts(int16_t sine_reference_raw,
                                         uint8_t &n_insert_upper,
                                         uint8_t &n_insert_lower)
{
    if (sine_reference_raw == MMC_SINE_REFERENCE_INVALID) return false;

    const float32_t sine = mmc_decode_sine_reference(sine_reference_raw);
    modulation_signal_upper = (a + m * sine) / 2.0F;
    modulation_signal_lower = (a - m * sine) / 2.0F;
    const float32_t upper = roundf(total_number_of_modules_arm * modulation_signal_upper);
    const float32_t lower = roundf(total_number_of_modules_arm * modulation_signal_lower);
    // Preserve the insertion-count bounds check after moving NLM off the sender.
    // This form also rejects non-finite results before converting them to uint8_t.
    if (!(upper >= 0.0F && upper <= total_number_of_modules_arm &&
          lower >= 0.0F && lower <= total_number_of_modules_arm))
        return false;

    n_insert_upper = static_cast<uint8_t>(upper);
    n_insert_lower = static_cast<uint8_t>(lower);
    return true;
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
        Cap_voltage = latest;
    }

    latest = shield.sensors.getLatestValue(I1_LOW);
    if (latest != NO_VALUE)
    {
        Arm_current = -latest;
    }
}

// The previous round was consumed by control before this window opened.
static bool begin_cycle(const MMC_frame_t &frame)
{
    if (received_module_count != 0)
    {
        if (frame.cycle_id != cycle_command.cycle_id)
            communication_fault = COMMUNICATION_ERROR;
        return false; // Duplicate SM1 frame, or two rounds in one window.
    }
    if (cycle_started)
    {
        const uint16_t advance = static_cast<uint16_t>(frame.cycle_id - cycle_command.cycle_id);
        if (advance == 0 || advance >= 32768U) return false;
        if (frame.status == POWER && advance != 1)
            communication_fault = COMMUNICATION_ERROR;
    }
    if (frame.status == IDLE)
        communication_fault = 0; // SM1 has cancelled POWER; prepare a fresh acquisition.
    if (frame.status > POWER || frame.sine_reference_raw == MMC_SINE_REFERENCE_INVALID)
        communication_fault = COMMUNICATION_ERROR;

    cycle_command = frame;
    cycle_started = true;
    return true;
}

// Also used before transmission, so a module does not depend on its own echo.
static bool store_module_measurements(const MMC_frame_t &frame)
{
    if (!cycle_started || frame.sm_id < MMC_SM_FIRST || frame.sm_id > MMC_SM_LAST)
        return false;
    if (frame.cycle_id != cycle_command.cycle_id)
    {
        communication_fault = COMMUNICATION_ERROR;
        return false;
    }
    const uint8_t index = frame.sm_id - MMC_SM_FIRST;
    if (measurement_received[index]) return false;
    if (frame.sm_id != MMC_SM1 && !measurement_received[0]) return false;

    if (frame.status > POWER) communication_fault = frame.status;
    else if (frame.status != cycle_command.status) communication_fault = COMMUNICATION_ERROR;
    MMC_capacitor_voltage[index] = mmc_decode_voltage(frame.capacitor_voltage_raw);
    MMC_arm_current[index] = mmc_decode_current(frame.arm_current_raw);
    measurement_received[index] = true;
    received_module_count++;
    return true;
}

static void send_own_measurements()
{
    MMC_frame_t frame = cycle_command;
    frame.sm_id = module_ID;
    frame.capacitor_voltage_raw = mmc_encode_voltage(Cap_voltage);
    frame.arm_current_raw = mmc_encode_current(Arm_current);
    if (communication_fault) frame.status = communication_fault;
    store_module_measurements(frame);
    memcpy(buffer_tx, &frame, sizeof(frame));
    communication.rs485.startTransmission();
}

void reception_function()
{
    MMC_frame_t frame;
    memcpy(&frame, buffer_rx, sizeof(frame));
    if (frame.sm_id == MMC_SM1)
    {
        if (module_ID == MMC_SM1 || !begin_cycle(frame)) return;
    }
    const bool accepted = store_module_measurements(frame);
    if (module_ID != MMC_SM1 && accepted && frame.sm_id == module_ID - 1)
        send_own_measurements();
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
    if (module_ID < MMC_SM_FIRST || module_ID > MMC_SM_LAST)
    {
        printk("Unknown board: control and communication disabled\n");
        return;
    }

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

    CommTask_num = task.createBackground(loop_communication_task);
    task.startBackground(CommTask_num);

    communication.rs485.configure(buffer_tx, buffer_rx, sizeof(buffer_rx),
                                  reception_function,
                                  SPEED_20M); // custom configuration for RS485
                                              /* Configure scope channels, what measurements do you want to acquire? */
    if (module_ID == MMC_SM1)
    {
        /* SM1 supplies the common control clock. */
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
        scope.set_trigger(&a_trigger);
        scope.set_delay(0.0F);
        scope.start();

    }
    else{
        /* Defines module as follower for communication synchorinization */
        communication.sync.initSlave();
    }
    task.startCritical();
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
        requested_command = 'i';
        printk("idle requested\n");
        break;
    case 'p':
        if (module_ID == MMC_SM1)
        {
            requested_command = 'p';
            printk("power requested: waiting for a complete measurement round\n");
        }
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
    if (module_ID == MMC_SM1)
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
 * It is executed every 200 micro-seconds defined in the setup_software
 * function.
 *
 * In the critical task, we implement the MMC control algorithms that will
 * run in Real Time.
 */
void loop_critical_task()
{
    update_measurements();

    // Every tick consumes the preceding window, including during IDLE.
    const bool round_complete = received_module_count == MMC_SM_COUNT;
    if (cycle_started && !round_complete) communication_fault = COMMUNICATION_ERROR;
    const char request = requested_command;
    requested_command = 0; // The console cannot preempt this interrupt.
    if (request == 'i')
    {
        power_requested = false;
        if (module_ID != MMC_SM1) communication_fault = COMMUNICATION_ERROR;
    }
    if (communication_fault) power_requested = false;
    else if (module_ID == MMC_SM1 && request == 'p') power_requested = true;

    // A POWER request first collects a round; PWM starts at the following tick.
    mode = round_complete && cycle_command.status == POWER && !communication_fault &&
           (module_ID != MMC_SM1 || power_requested) ? POWERMODE : IDLEMODE;
    uint8_t n_insert_upper = 0;
    uint8_t n_insert_lower = 0;
    if (mode == POWERMODE &&
        !mmc_compute_insertion_counts(cycle_command.sine_reference_raw,
                                     n_insert_upper, n_insert_lower))
    {
        communication_fault = COMMUNICATION_ERROR;
        power_requested = false;
        mode = IDLEMODE;
    }
    if (mode == POWERMODE)
    {
        i_upper_arm = MMC_arm_current[0] - 0.8F;
        i_lower_arm = MMC_arm_current[5] + 0.19F;
        number_of_connected_submodules_upper_arm = n_insert_upper;
        number_of_connected_submodules_lower_arm = n_insert_lower;
        if (mmc_is_upper_arm_module(module_ID))
        {
            assign_arm_gates_local_consensus(MMC_capacitor_voltage, i_upper_arm,
                n_insert_upper, g_u);
            module_command = g_u[module_ID - MMC_SM1];
        }
        else
        {
            assign_arm_gates_local_consensus(&MMC_capacitor_voltage[total_number_of_modules_arm],
                i_lower_arm, n_insert_lower, g_l);
            module_command = g_l[module_ID - MMC_SM6];
        }
        if (module_ID == MMC_SM1)
        {
            if (++scope_timer >= scope_period)
            {
                scope.acquire();
                scope_timer = 0;
            }
        }
        critical_task_timer++;
    }

    // Prepare the NEXT round. SM1 applies the same previous round as all other modules.
    if (module_ID == MMC_SM1)
    {
        next_command = {};
        next_command.sm_id = MMC_SM1;
        next_command.cycle_id = static_cast<uint16_t>(cycle_command.cycle_id + 1U);
        if (power_requested)
        {
            angle = ot_modulo_2pi(angle + w0 * Ts);
            next_command.sine_reference_raw = mmc_encode_sine_reference(ot_sin(angle));
            next_command.status = POWER;
        }
        else angle = 0.0F;
    }

    received_module_count = 0;
    for (uint8_t i = 0; i < MMC_SM_COUNT; ++i) measurement_received[i] = false;

    if (communication_fault)
    {
        power_requested = false;
        mode = IDLEMODE;
    }
    if (mode == POWERMODE)
    {
        shield.power.setDutyCycle(LEG1, module_command ? 1.0F : 0.0F);
        if (!pwm_enable) shield.power.start(LEG1);
        pwm_enable = true;
    }
    else
    {
        if (pwm_enable) shield.power.stop(ALL);
        pwm_enable = false;
        module_command = 0;
    }
    counter_timer++;
    // As in the original schedule, the exchange follows control and must finish
    // before the next tick on every board. No additional timer is used.
    if (module_ID == MMC_SM1)
    {
        if (communication_fault)
        {
            next_command.status = IDLE;
            next_command.sine_reference_raw = 0;
        }
        if (begin_cycle(next_command)) send_own_measurements();
    }
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
