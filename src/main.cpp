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
 * @brief  Distributed MMC voltage balancing. SM1 broadcasts arm insertion counts.
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
#if defined(CONFIG_OWNTECH_OTA)
#include "OtaService.h"
#include <zephyr/kernel.h>
#endif


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

static const float32_t f0 = 50.F; //[Hz] Output frequency used to generate the sinusoidal reference for open-loop control
static const uint8_t total_number_of_modules_arm = 5; //[-] Number of modules per arm
constexpr float32_t Vcap_expected = 80.0F; //[V] Capacitor DC voltage expected during the test (used to set voltage measurement scale for 12 bits)
constexpr float32_t i_expected = 10.0F; //[A] Expected current amplitude during test (used to set current measurement scale for 12 bits)
constexpr float32_t overvoltage_tolerance = 80.0F; //[V] Set overvoltage tolerance (default max TWIST voltage)
constexpr float32_t overcurrent_tolerance = 8.0F; //[A] Set overcurrent tolerance (default max TWIST current)

/* -------------- BOARD IDENTIFICATION ----------------------- */
/* --------------- To be changed by user --------------------- */

constexpr uint32_t UID_MMC_SM1_BOARD = 0x0029004C;
constexpr uint32_t UID_MMC_SM2_BOARD = 0x00290049;
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

/**
 * @brief Encode an capacitor voltage into the 12-bit transport format.
 *
 * @param current Physical capacitor voltage in volts.
 * @return 12-bit encoded voltage suitable for MMC frames.
 */
static inline uint16_t mmc_encode_voltage(float32_t voltage)
{
    if (voltage <= 0.0F) return 0;
    if (voltage >= Cap_voltage_SCALE) return 0x0FFF;
    return static_cast<uint16_t>((voltage * 4095.0F) / Cap_voltage_SCALE);
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
    if (current <= -Arm_current_OFFSET) return 0;
    if (current >= Arm_current_SCALE - Arm_current_OFFSET) return 0x0FFF;
    return static_cast<uint16_t>(((current + Arm_current_OFFSET) * 4095.0F) / Arm_current_SCALE);
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

// Only known boards can participate; SM1 generates the arm insertion counts.
uint8_t module_ID = detect_module_id();
static float32_t module_command = 0.0F; // 1 = inserted, 0 = bypassed.
static bool power_requested = false; // SM1: a new 'p' is required after every fault.
static volatile char requested_command = 0; // Console publishes; control consumes.

// SM1 supplies the two global insertion counts.
// Protocol change from sine-reference PWM: update all boards together.
struct MMC_frame_t
{
    uint8_t upper_insert_count;
    uint8_t lower_insert_count;
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

// Exact transmitted samples, including our own, ensure identical local ranks.
static uint16_t capacitor_voltage_raw[MMC_SM_COUNT] = {};
static bool measurement_received[MMC_SM_COUNT] = {};
static uint8_t received_module_count = 0;
// SM1 supplies the upper-arm current; SM6 supplies the lower-arm current.
static float32_t upper_arm_current_reference = 0.0F;
static float32_t lower_arm_current_reference = 0.0F;
static float32_t upper_insert_count_scope = 0.0F;
static float32_t lower_insert_count_scope = 0.0F;

// Scheduling assumption: RS485 reception and control never overlap.
// Each exchange finishes before the next control tick; no interrupt locking.
static MMC_frame_t cycle_command;
static MMC_frame_t next_command; // Prepared by SM1 for the next communication window.
static bool cycle_started = false;
static volatile uint8_t communication_fault = 0;

constexpr size_t MMC_FRAME_SIZE = sizeof(MMC_frame_t);
uint8_t buffer_tx[MMC_FRAME_SIZE];
uint8_t buffer_rx[MMC_FRAME_SIZE];

// Finite sensor/control values are assumed. Keep the last value on NO_VALUE.
float32_t Cap_voltage = 0.0F;
static float32_t Arm_current = 0.0F;

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

/* --------------- Distributed insertion variables ------------------*/

/* [us] period of the control task (=critical task) */
static constexpr uint32_t control_task_period = 200; // us
static float32_t Ts = control_task_period * 1e-6F; // s
/* [bool] state of the PWM (ctrl task) */
static bool pwm_enable = false;

#if defined(CONFIG_OWNTECH_OTA)
// A follower must see a fresh IDLE after release before accepting POWER.
static bool mmc_wait_for_idle = true;

static bool mmc_ota_inhibited()
{
    // OTA owns the hardware stop and never calls into this application.
    // Observing it only discards stale MMC power requests and requires a new
    // IDLE exchange; neither application setup nor SYNC gates the OTA service.
    // A completed campaign reboots and resets the latch. Local observation
    // covers maintenance while this application is still running.
    const unsigned int key = irq_lock();
    const bool inhibited = ota_safety_inhibited();
    if (inhibited)
    {
        power_requested = false;
        requested_command = 0;
        mode = IDLEMODE;
        pwm_enable = false;
        module_command = 0.0F;
        mmc_wait_for_idle = true;
    }
    irq_unlock(key);
    return inhibited;
}
static bool mmc_ota_power_blocked() { return mmc_ota_inhibited() || mmc_wait_for_idle; }
#else
static bool mmc_ota_inhibited() { return false; }
static bool mmc_ota_power_blocked() { return false; }
#endif

static uint32_t critical_task_timer = 0;

/* Scope variables */
static bool enable_acq; // Sets trigger moment if true
static const uint16_t NB_DATAS = 1028; // Number of data acquired
static ScopeMimicry scope(NB_DATAS, 5); // Voltage, arm counts, shared current and insertion
static bool is_downloading; // Records data if true
static uint32_t scope_timer = 0;
static uint32_t scope_period = 1; // scope acquire data every t = scope_period * critical_task_period (200 µs) s;

/* NLM modulation on SM1; preserve the firmware's upper/lower sine signs. */
static float32_t m = 1; // Modulation amplitude, used by SM1
static float32_t a = 1; // Modulation dc part, used by SM1
static float32_t angle;
static const float32_t w0 = 2 * PI * f0; // Angular frequency

static bool mmc_compute_insertion_counts(float32_t sine, MMC_frame_t &frame)
{
    const float32_t upper = 0.5F * (a + m * sine);
    const float32_t lower = 0.5F * (a - m * sine);
    if (!(upper >= 0.0F && upper <= 1.0F && lower >= 0.0F && lower <= 1.0F))
        return false;
    frame.upper_insert_count = static_cast<uint8_t>(roundf(total_number_of_modules_arm * upper));
    frame.lower_insert_count = static_cast<uint8_t>(roundf(total_number_of_modules_arm * lower));
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
    /* We divide by 4 (4 bytes per float32_t value) */
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
        Cap_voltage = latest;
    latest = shield.sensors.getLatestValue(I2_LOW);
    if (latest != NO_VALUE)
        Arm_current = -latest; // Preserve the arm-current sign convention on LEG2.
}

static void mmc_check_power_limits()
{
    if (fabsf(Arm_current) > overcurrent_tolerance) communication_fault = OVER_CURRENT;
    else if (Cap_voltage > overvoltage_tolerance) communication_fault = OVER_VOLTAGE;
}

// Called once per reception window, including on SM1.
static bool begin_cycle(const MMC_frame_t &frame)
{
    if (frame.sm_id != MMC_SM1) return false;
    if (received_module_count != 0)
    {
        if (frame.cycle_id != cycle_command.cycle_id)
            communication_fault = COMMUNICATION_ERROR;
        return false;
    }
    if (cycle_started)
    {
        const uint16_t advance = static_cast<uint16_t>(frame.cycle_id - cycle_command.cycle_id);
        if (advance == 0 || advance >= 32768U) return false;
        if (frame.status == POWER && advance != 1)
            communication_fault = COMMUNICATION_ERROR;
    }
    if (frame.status == IDLE)
        communication_fault = 0; // Recovery still requires usable measurements and a new 'p'.
    if (frame.status > POWER ||
        frame.upper_insert_count > total_number_of_modules_arm ||
        frame.lower_insert_count > total_number_of_modules_arm)
        communication_fault = COMMUNICATION_ERROR;

    cycle_command = frame;
    cycle_started = true;
#if defined(CONFIG_OWNTECH_OTA)
    if (mmc_ota_inhibited()) mmc_wait_for_idle = true;
    else if (frame.status == IDLE) mmc_wait_for_idle = false;
#endif
    return true;
}

static void store_cycle_sample(const MMC_frame_t &frame)
{
    capacitor_voltage_raw[frame.sm_id - MMC_SM_FIRST] = frame.capacitor_voltage_raw;
    const float32_t current = Arm_current_SCALE * static_cast<float32_t>(frame.arm_current_raw) / 4095.0F
        - Arm_current_OFFSET;
    if (frame.sm_id == MMC_SM1) upper_arm_current_reference = current;
    if (frame.sm_id == MMC_SM6) lower_arm_current_reference = current;
}

// Count accepted frames and keep measurements from this exchange.
static bool store_module_measurements(const MMC_frame_t &frame)
{
    if (!cycle_started || frame.sm_id < MMC_SM_FIRST || frame.sm_id > MMC_SM_LAST)
        return false;
    if (frame.sm_id != MMC_SM1 && !measurement_received[0]) return false;
    if (frame.cycle_id != cycle_command.cycle_id ||
        frame.upper_insert_count != cycle_command.upper_insert_count ||
        frame.lower_insert_count != cycle_command.lower_insert_count)
    {
        communication_fault = COMMUNICATION_ERROR;
        return false;
    }
    const uint8_t index = frame.sm_id - MMC_SM_FIRST;
    if (measurement_received[index]) return false;

    if (frame.status > POWER) communication_fault = frame.status;
    else if (frame.status != cycle_command.status) communication_fault = COMMUNICATION_ERROR;
    measurement_received[index] = true;
    received_module_count++;
    store_cycle_sample(frame);
    return true;
}

static void send_own_measurements()
{
    const uint8_t index = module_ID - MMC_SM_FIRST;
    if (measurement_received[index]) return;
    mmc_check_power_limits();
    MMC_frame_t frame = cycle_command;
    frame.sm_id = module_ID;
    frame.capacitor_voltage_raw = mmc_encode_voltage(Cap_voltage);
    frame.arm_current_raw = mmc_encode_current(Arm_current);
    if (communication_fault) frame.status = communication_fault;
    // A follower under maintenance must not echo a POWER acknowledgement.
    if (mmc_ota_power_blocked() && frame.status == POWER) frame.status = COMMUNICATION_ERROR;
    store_cycle_sample(frame);
    // Include our own slot in the round without waiting for an echo.
    measurement_received[index] = true;
    received_module_count++;
    memcpy(buffer_tx, &frame, sizeof(frame));
    communication.rs485.startTransmission();
}

void reception_function()
{
    MMC_frame_t frame;
    memcpy(&frame, buffer_rx, sizeof(frame));
    if (frame.sm_id == module_ID) return;
    if (frame.sm_id == MMC_SM1)
    {
        if (module_ID == MMC_SM1 || !begin_cycle(frame)) return;
    }
    const bool accepted = store_module_measurements(frame);
    // Bus order: SM1 -> SM2 -> ... -> SM10, across both arms.
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

    // PWMA remains the synchronization reference; PWMC drives LEG2.
    shield.power.initBuck(ALL);
    /* Declare task */
    int8_t background_task_number =
        task.createBackground(loop_background_task);

    const int8_t critical_result = task.createCritical(loop_critical_task, control_task_period);
    if (background_task_number < 0 || critical_result < 0) return;

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

    CommTask_num = task.createBackground(loop_communication_task);
    if (CommTask_num < 0) return;

    communication.rs485.configure(buffer_tx, buffer_rx, sizeof(buffer_rx),
                                  reception_function,
                                  SPEED_20M); // custom configuration for RS485
                                              /* Configure scope channels, what measurements do you want to acquire? */
    if (module_ID == MMC_SM1)
    {
        /* SM1 supplies the common control clock. */
        communication.sync.initMaster();

        /* Configures scopemimicry measured variables */
        scope.connectChannel(Cap_voltage, "vc_self");
        scope.connectChannel(upper_insert_count_scope, "N_upper");
        scope.connectChannel(lower_insert_count_scope, "N_lower");
        scope.connectChannel(upper_arm_current_reference, "i_upper_ref");
        scope.connectChannel(module_command, "inserted");
        scope.set_trigger(&a_trigger);
        scope.set_delay(0.0F);
        scope.start();

    }
    else{
        /* Defines module as follower for communication synchorinization */
        communication.sync.initSlave();
    }
    shield.power.stop(ALL);
    // Binary insertion uses the same switching phase on every module.
    shield.power.setPhaseShift(LEG2, 0);
    task.startBackground(background_task_number);
    task.startBackground(CommTask_num);
    task.startCritical();
}

/* --------------LOOP FUNCTIONS-------------------------------- */

/**
 * This is the communication task.
 * It is used to send to the board via the computer the desired mode
 * IDLE (i) = block all modules or POWER (p) = operate with distributed capacitor-voltage sorting.
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
        if (module_ID == MMC_SM1 && !mmc_ota_power_blocked())
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
    // Followers must also observe maintenance while external SYNC is absent.
    (void)mmc_ota_inhibited();
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

// Rank against shared quantized voltages; ties favor the smaller SM identifier.
static float32_t mmc_local_insertion()
{
    const bool upper = mmc_is_upper_arm_module(module_ID);
    const uint8_t first = upper ? MMC_SM1 : MMC_SM6;
    const uint8_t count = upper ? cycle_command.upper_insert_count : cycle_command.lower_insert_count;
    const float32_t current = upper ? upper_arm_current_reference : lower_arm_current_reference;
    const uint16_t own_voltage = capacitor_voltage_raw[module_ID - MMC_SM_FIRST];
    uint8_t rank = 0;
    for (uint8_t id = first; id < first + total_number_of_modules_arm; ++id)
    {
        const uint16_t voltage = capacitor_voltage_raw[id - MMC_SM_FIRST];
        const bool ahead = current >= 0.0F ? voltage < own_voltage : voltage > own_voltage;
        if (ahead || (voltage == own_voltage && id < module_ID)) ++rank;
    }
    return rank < count ? 1.0F : 0.0F;
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
    // The preceding exchange is finished before control starts.
    const bool round_complete = received_module_count == MMC_SM_COUNT;
    if (cycle_started && !round_complete)
        communication_fault = COMMUNICATION_ERROR;
    update_measurements();
    mmc_check_power_limits();

    const char request = requested_command;
    requested_command = 0;
    if (request == 'i')
    {
        power_requested = false;
        if (module_ID != MMC_SM1) communication_fault = COMMUNICATION_ERROR;
    }
    if (communication_fault) power_requested = false;
    else if (module_ID == MMC_SM1 && request == 'p') power_requested = true;

    const bool ota_inhibited = mmc_ota_inhibited();
    bool ota_rearm_pending = false;
    if (ota_inhibited) power_requested = false;
#if defined(CONFIG_OWNTECH_OTA)
    if (ota_inhibited) mmc_wait_for_idle = true;
    ota_rearm_pending = mmc_wait_for_idle;
#endif
    if (ota_rearm_pending) power_requested = false;

    // A request starts PWM only after the preceding POWER window is complete.
    mode = round_complete && cycle_command.status == POWER &&
        !communication_fault && !ota_inhibited && !ota_rearm_pending &&
        (module_ID != MMC_SM1 || power_requested) ? POWERMODE : IDLEMODE;
    module_command = 0.0F;
    if (mode == POWERMODE)
    {
        module_command = mmc_local_insertion();
    }
    upper_insert_count_scope = cycle_command.upper_insert_count;
    lower_insert_count_scope = cycle_command.lower_insert_count;
    if (mode == POWERMODE)
    {
        shield.power.setDutyCycle(LEG2, module_command);
        if (!pwm_enable) shield.power.start(LEG2);
        pwm_enable = true;
    }
    else
    {
        if (pwm_enable) shield.power.stop(ALL);
        pwm_enable = false;
        module_command = 0.0F;
    }

    if (module_ID == MMC_SM1 && mode == POWERMODE && ++scope_timer >= scope_period)
    {
        scope.acquire();
        scope_timer = 0;
    }
    if (mode == POWERMODE) critical_task_timer++;
    counter_timer++;

    // Open the next exchange only after consuming the previous window.
    received_module_count = 0;
    for (uint8_t i = 0; i < MMC_SM_COUNT; ++i) measurement_received[i] = false;
    if (module_ID == MMC_SM1)
    {
        next_command = {};
        next_command.sm_id = MMC_SM1;
        next_command.cycle_id = static_cast<uint16_t>(cycle_command.cycle_id + 1U);
        if (power_requested)
        {
            angle = ot_modulo_2pi(angle + w0 * Ts);
            if (mmc_compute_insertion_counts(ot_sin(angle), next_command))
                next_command.status = POWER;
            else
            {
                communication_fault = COMMUNICATION_ERROR;
                power_requested = false;
                next_command.status = COMMUNICATION_ERROR;
            }
        }
        else angle = 0.0F;
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
