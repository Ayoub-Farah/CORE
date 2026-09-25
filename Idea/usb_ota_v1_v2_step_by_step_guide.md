# OwnTech: use OTA v2 and switch back to USB

Use the buttons in **PlatformIO Project Tasks**. The assistant windows guide
board selection, building, installation and verification.

**Hardware qualification of this complete workflow is still pending.**

<a id="readiness-before-flashing"></a>
## Before you start

- Open the intended project in VS Code and check its branch and application.
  The buttons build the project currently open.
- Use a project already configured for your boards and safe OTA operation.
  These buttons do not prepare an application that lacks safe shutdown and startup.
- Keep the power stage stopped. Close serial monitors and Scope.
- Connect one board by USB at a time. Keep its label or USB serial available.
- Reserve a separate board as the **Lead**. It distributes updates; the other
  boards are **receivers** running your application.

Click the **PlatformIO icon** in the VS Code sidebar, then open
**Project Tasks → environment → Custom**. Refresh Project Tasks if needed.

| Environment | Use it for |
|---|---|
| **OTA** | Your application with OTA v2 reception |
| **USB_LEAD** | The separate Lead board |
| **USB** | Your ordinary USB application |

| Button | Available under |
|---|---|
| **Initialize over USB** | OTA, USB_LEAD |
| **Update CAN receiver boards** | USB_LEAD |
| **Check connected board** | USB, OTA, USB_LEAD |
| **Finish previous CAN update** | USB_LEAD |
| **Switch to USB** | USB, OTA, USB_LEAD |
| **Return to OTA V2** | OTA, USB_LEAD |

<a id="case-a--ordinary-usb-application-to-ota-v2"></a>
## 1. Start with USB boards and use OTA v2

### Prepare each receiver

1. Connect a receiver by USB.
2. Open **OTA → Custom → Initialize over USB**.
3. Select the connected board if asked and follow the assistant windows.
4. Wait for the installation and verification to finish.
5. Use **Check connected board** to check the result.
6. Repeat for each receiver.

A receiver can report that it is waiting for a CAN peer until the network is
connected. Its local installation must still pass verification.

If automatic USB bootloader entry fails before any firmware is sent, the assistant
asks you to use **BOOT + RESET**, then click **OK**. It checks the same board again
and continues with the saved firmware. This also applies when preparing the Lead.

### Prepare the Lead

1. Connect the board reserved for the Lead by USB.
2. Open **USB_LEAD → Custom → Initialize over USB**.
3. Follow the assistant windows and wait for verification.
4. Use **Check connected board** to verify its Lead role.

Installing the Lead replaces that board's application with the coordinator.

### Send an update over CAN

1. Connect and power the Lead and receivers on the prepared CAN network.
2. Leave the Lead connected to the PC by USB.
3. Open **USB_LEAD → Custom → Update CAN receiver boards**.
4. Enter the number of receivers when asked. **Do not count the Lead.**
5. Confirm the update in the assistant window.
6. Keep USB, CAN and board power connected until the task reports success.

The task builds the receiver application and sends it through the Lead. Success
includes the receivers restarting, confirming their images and leaving update
mode. Reaching 100% transfer alone is insufficient.

If an update was interrupted, keep the same boards connected and use
**USB_LEAD → Custom → Finish previous CAN update** before starting another one.

<a id="case-c--ota-v2-back-to-usb-then-back-to-ota-v2"></a>
## 2. Switch OTA v2 boards to USB, then return to OTA v2

### Switch a board to ordinary USB

1. Finish any previous CAN update while the complete network is still connected.
   Use **Finish previous CAN update** on the Lead when needed.
2. Connect the board you want to change by USB.
3. Open **USB → Custom → Switch to USB**. The same button is also available
   under OTA and USB_LEAD.
4. Follow the assistant windows. It reads the board and finds its saved firmware
   and update history. If a file is missing, use the file picker to select the
   original saved build or successful update record.
5. Follow each **BOOT + RESET** prompt. The assistant temporarily installs a
   cleanup application, checks its result, installs your USB application and
   verifies it.
6. Wait for the final USB verification before using the application normally.

Choose **never used for a CAN update** only when that is the board's actual
history. It does not bypass missing or unfinished update records. A newly
rebuilt image cannot replace the saved image that is currently on the board.

The temporary cleanup application preserves calibration and the existing
bootloader. The assistant manages it automatically; you do not select its build
profile yourself. When converting the whole network, **convert the Lead last**.

### Use BOOT + RESET when prompted

For the supported OwnTech board, hold **BOOT**, press and release **RESET**,
keep BOOT held for about one second, then release it. Continue in the assistant
window after the board reconnects. Follow the board-specific instructions if
its buttons differ.

### Return to OTA v2

1. Open the project and application you want to install.
2. Connect the USB board.
3. Choose **OTA → Custom → Return to OTA V2** for a receiver, or
   **USB_LEAD → Custom → Return to OTA V2** for the Lead.
4. Select the board's saved operation folder if asked. If you installed further
   USB builds, select the saved build matching the application now on the board.
5. Follow the build, BOOT + RESET and installation prompts.
6. Wait for OTA verification, then reconnect CAN and use **Check connected board**.

You may return with a newer or different application. Keep the project's normal
OTA readiness checks satisfied before sending another CAN update.

## Saved files, cancellation and problems

The assistant saves the operation automatically under **ota-artifacts/operations**.
Keep that folder: it contains the firmware, update evidence and verification
results needed to finish or resume the operation.

**Cancel** stops subsequent steps and keeps the saved files. To continue an
interrupted switch, launch **Switch to USB** again and select its existing
operation folder when offered. If the assistant refuses a board or cannot
verify a step, keep its report and resolve the reported problem before retrying.
Do not repeatedly reset the board to force progress.

If a task fails, read the specific error in the assistant window or the PlatformIO
task output. A copy of the output is saved under **ota-artifacts/workflow-logs**;
the task displays its exact location. Keep this log with the operation folder
when asking for help.

For another switch after a later OTA update, start a new operation using that
board's current firmware and completed update history.

[Advanced workflow reference](../docs/ota-usb-transition.md)
