"""Run MMC inhibition/rearm paths; OTA readiness belongs to the core tests."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

STUBS = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "OtaService.h"
#ifdef _WIN32
extern "C" { int _fltused = 0; }
#endif
#define CONFIG_OWNTECH_OTA 1
#define __packed __attribute__((packed))
#define PRIX32 "X"
#define PI 3.14159265F
#define NO_VALUE -9999.0F
using float32_t = float;
static unsigned irq_lock() { return 0; }
static void irq_unlock(unsigned) {}
static bool inhibited;
static uint32_t test_board_uid;
extern "C" bool ota_safety_inhibited() { return inhibited; }
struct Hardware { struct { unsigned OENR; } sCommonRegs; } hardware;
#define HRTIM1 (&hardware)
enum { ALL, LEG1, LEG2, V_HIGH, I2_LOW, V1_LOW, V2_LOW, I1_LOW, I_HIGH,
       GPIOA, LL_GPIO_PIN_5, LL_GPIO_MODE_OUTPUT, LL_GPIO_SPEED_FREQ_VERY_HIGH,
       LL_GPIO_OUTPUT_PUSHPULL, LL_GPIO_PULL_NO, SPEED_20M };
static void LL_GPIO_SetPinMode(int,int,int) {}
static void LL_GPIO_SetPinSpeed(int,int,int) {}
static void LL_GPIO_SetPinOutputType(int,int,int) {}
static void LL_GPIO_SetPinPull(int,int,int) {}
static void LL_GPIO_ResetOutputPin(int,int) {}
static void LL_GPIO_SetOutputPin(int,int) {}
static int printk(const char *, ...) { return 0; }
static int console_char;
static int console_getchar() { return console_char; }
static float fabsf(float x) { return x < 0 ? -x : x; }
static float roundf(float x) { return (float)(int)(x + 0.5F); }
static float ot_modulo_2pi(float x) { return x; }
static float ot_sin(float) { return 0; }
struct Power {
    void initBuck(int) {} void disconnectCapacitor(int) {}
    void setDutyCycleMax(int,float) {} void setDutyCycleMin(int,float) {}
    void setDutyCycle(int,float) {} void setPhaseShift(int,int) {}
    void stop(int) { hardware.sCommonRegs.OENR = 0; }
    void start(int) { if (!inhibited) hardware.sCommonRegs.OENR = 1; }
};
struct Sensors {
    float getLatestValue(int) { return NO_VALUE; }
    void enableDefaultTwistSensors() {}
    void setConversionParametersLinear(int,float,float) {}
};
static struct { Power power; Sensors sensors; } shield;
struct Led { void turnOff() {} void toggle() {} };
static struct { Led led; } spin;
struct Tasks {
    unsigned backgrounds, criticals;
    int8_t createBackground(void (*)()) { ++backgrounds; return 0; }
    int8_t createCritical(void (*)(), uint32_t) { ++criticals; return 0; }
    void startBackground(int) {} void startCritical() {}
    void suspendBackgroundMs(int) {} void suspendBackgroundUs(int) {}
} task;
struct Rs485 {
    void configure(uint8_t *,uint8_t *,size_t,void (*)(),int) {}
    void startTransmission() {}
};
struct Sync {
    unsigned masters, slaves;
    void initMaster() { ++masters; } void initSlave() { ++slaves; }
};
static struct { Rs485 rs485; Sync sync; } communication;
struct ScopeMimicry {
    ScopeMimicry(int,int) {}
    uint8_t *get_buffer() { return nullptr; }
    int get_buffer_size() { return 0; } int get_nb_channel() { return 0; }
    const char *get_channel_name(int) { return ""; } int get_final_idx() { return 0; }
    void connectChannel(float &,const char *) {} void set_trigger(bool (*)()) {}
    void set_delay(float) {} void start() {} void acquire() {}
};
'''

CHECKS = r'''
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static void reset_mmc() {
    module_ID = MMC_SM1; inhibited = true;
    mmc_wait_for_idle = true;
    mode = IDLEMODE; pwm_enable = false; power_requested = false;
    requested_command = 0; module_command = 0; communication_fault = 0;
    cycle_started = false; received_module_count = 0; cycle_command = {};
    counter_timer = counter_receive = 0; console_char = 0;
    task.backgrounds = task.criticals = 0;
    communication.sync.masters = communication.sync.slaves = 0;
    hardware.sCommonRegs.OENR = 0;
    for (auto &received : measurement_received) received = false;
}
static void tick() { loop_critical_task(); }
static void core_inhibit() {
    // Model the already completed core-owned stop; MMC has no OTA callback.
    inhibited = true;
    hardware.sCommonRegs.OENR = 0;
}
extern "C" int mmc_application_test_run() {
    test_board_uid = 0x0029004C;
    CHECK(detect_module_id() == MMC_SM1);
    test_board_uid = 0x00290049;
    CHECK(detect_module_id() == MMC_SM2);
    test_board_uid = 0xDEADBEEF;
    CHECK(detect_module_id() == 0);
    // Unknown MMC identity prevents application control only. No OTA service
    // health/confirmation function is supplied by this application's main.
    reset_mmc(); module_ID = 0; setup_routine();
    CHECK(!task.backgrounds && !task.criticals && !hardware.sCommonRegs.OENR);

    // A follower may finish setup and run background tasks without any SYNC.
    reset_mmc(); module_ID = MMC_SM2; setup_routine(); loop_background_task();
    CHECK(task.backgrounds == 2 && task.criticals == 1);
    CHECK(communication.sync.slaves == 1 && !communication.sync.masters);
    CHECK(!counter_timer && mmc_wait_for_idle && !hardware.sCommonRegs.OENR);
    inhibited = false;
    console_char = 'p'; loop_communication_task(); // service release cannot power a follower
    CHECK(!requested_command && !power_requested && !hardware.sCommonRegs.OENR);

    reset_mmc(); inhibited = false; mmc_wait_for_idle = false;
    mode = POWERMODE; pwm_enable = true; power_requested = true;
    requested_command = 'p'; module_command = 1; hardware.sCommonRegs.OENR = 1;
    core_inhibit(); loop_background_task();
    CHECK(mode == IDLEMODE && !pwm_enable && !power_requested);
    CHECK(!requested_command && module_command == 0 && !hardware.sCommonRegs.OENR);
    CHECK(mmc_wait_for_idle && !counter_timer);
    console_char = 'p'; loop_communication_task(); CHECK(!requested_command);
    requested_command = 'p'; tick(); // even a queued command cannot restart
    CHECK(!power_requested && mode == IDLEMODE && next_command.status == IDLE);
    CHECK(!hardware.sCommonRegs.OENR && counter_timer == 1);
    inhibited = false; tick(); // establish fresh IDLE after release
    CHECK(!power_requested && !mmc_wait_for_idle && mode == IDLEMODE);
    // SM1 requires a fresh request and then a full POWER measurement round.
    received_module_count = MMC_SM_COUNT;
    console_char = 'p'; loop_communication_task(); tick();
    CHECK(power_requested && mode == IDLEMODE && next_command.status == POWER);
    received_module_count = MMC_SM_COUNT; tick();
    CHECK(mode == POWERMODE && pwm_enable && hardware.sCommonRegs.OENR);

    // A running follower also discards its stale state from the background
    // task when its critical task receives no SYNC during maintenance.
    reset_mmc(); module_ID = MMC_SM2; inhibited = false; mmc_wait_for_idle = false;
    mode = POWERMODE; pwm_enable = true; module_command = 1; hardware.sCommonRegs.OENR = 1;
    core_inhibit(); loop_background_task();
    CHECK(mmc_wait_for_idle && mode == IDLEMODE && !pwm_enable && !counter_timer);
    CHECK(!module_command && !hardware.sCommonRegs.OENR);
    MMC_frame_t during_ota{}; during_ota.sm_id = MMC_SM1;
    CHECK(begin_cycle(during_ota) && mmc_wait_for_idle); // IDLE while inhibited cannot rearm
    inhibited = false; loop_background_task();
    CHECK(mmc_wait_for_idle); // service release alone cannot rearm MMC
    cycle_command.status = POWER; cycle_started = true;
    received_module_count = MMC_SM_COUNT;
    tick(); CHECK(mode == IDLEMODE && !hardware.sCommonRegs.OENR);
    cycle_command.status = POWER; send_own_measurements();
    MMC_frame_t sent; memcpy(&sent, buffer_tx, sizeof(sent));
    CHECK(sent.status == COMMUNICATION_ERROR); // report refusal to SM1
    received_module_count = 0; cycle_started = false;
    MMC_frame_t idle{}; idle.sm_id = MMC_SM1;
    CHECK(begin_cycle(idle) && !mmc_wait_for_idle);
    cycle_command.status = POWER; received_module_count = MMC_SM_COUNT;
    tick(); CHECK(mode == POWERMODE && pwm_enable && hardware.sCommonRegs.OENR);
    return 0;
}
int main() { return mmc_application_test_run(); }
'''


class MmcApplicationTests(unittest.TestCase):
    def test_identity_inhibition_and_rearm_without_ota_callbacks(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++ compiler required")
        source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        # Compile all application logic. Only replace platform headers and the
        # reset-time physical UID read; explicit unknown-ID coverage is above.
        source = re.sub(r'^#include .*$', '', source, flags=re.MULTILINE)
        source = source.replace('return *uid0;', 'return test_board_uid;')
        source = source.replace('uint8_t module_ID = detect_module_id();',
                                'uint8_t module_ID = MMC_SM1;')
        source = source.replace('int main(void)', 'int mmc_firmware_main(void)')
        with tempfile.TemporaryDirectory(prefix="mmc-ota-test-") as tmp:
            cpp = Path(tmp) / "mmc.cpp"
            cpp.write_text(STUBS + source + CHECKS, encoding="utf-8")
            output = Path(tmp) / ("mmc.dll" if os.name == "nt" else "mmc")
            args = [compiler, "-std=c++20", "-I" + str(ROOT / "zephyr/modules/owntech_ota/zephyr/public_api")]
            if os.name == "nt":
                args += ["-I" + str(ROOT / "tests/ota/core_shim"),
                         "-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe",
                         "-fno-exceptions", "-fno-rtti", "-nostdlib", "-shared", "-fuse-ld=lld",
                         "-Wl,/noentry", "-Wl,/export:mmc_application_test_run",
                         str(ROOT / "tests/ota/core_shim.cpp")]
            built = subprocess.run(args + [str(cpp), "-o", str(output)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            if os.name == "nt":
                command = [os.sys.executable, "-c",
                           "import ctypes,sys; rc=ctypes.CDLL(sys.argv[1]).mmc_application_test_run();"
                           "print(rc);sys.exit(bool(rc))", str(output)]
            else:
                command = [str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
