# OwnTech Power API

This is the OwnTech Power API Core repository.

The experimental OTA environments build your current `src/main.cpp` with the
USB/CAN update service:

```sh
pio run -e OTA -t upload                 # Install the application on one board
pio run -e USB_LEAD -t lead_update       # Build and update the selected CAN fleet
```

Install OTA support on each board once, alone over USB if needed, then connect
the CAN bus and use `USB_LEAD` for subsequent
application updates. The same compatible firmware runs on the Lead and its
participants; the Lead role is selected at runtime. Build identity is generated
automatically from source and configuration changes. The ordinary `USB` and
`STLink` workflows remain available.

Select the USB board and expected fleet, and adapt the application's maintenance
and health callbacks before using power-control code. See the
[operator guide](docs/ota-client.md) and
[implementation and validation notes](docs/ota-implementation.md).

The Power API is designed to be used with VS Code and PlatformIO.
[Installing VS Code with PlatformIO](https://platformio.org/install/ide?install=vscode).

For information about Power API, check out its [Documentation](https://docs.owntech.org/#/renders/API/home).


## Downloading OwnTech Power API Core

You fisrt need to download the Power API Core repository using the following command:

`git clone https://github.com/owntech-foundation/Core.git owntech_power_api`

Then, open VS Code and, if not already done, install the PlatformIO plugin.

Finally, open the newly cloned folder `owntech_power_api` using menu `File > Open Folder...`


## Working with OwnTech Power API

While the project contains many folders and files, all your code goes to the `src` folder.
In the this folder, the file `main.cpp` is the entry point of the application.
Aditionally, some configuration can be done in the `platformio.ini` file.

Other folders and files are used to configure the underlying Zephyr OS and PlatformIO, and are hidden by default.


### Accessing OwnTech source code in VS Code (for advanced developers)

The full hierarchy of the project is as follows:

```
owntech_power_api
└─ owntech
|  └─ boards
|  └─ scripts
|  └─>pio_extra.ini
└─ src
|  └─>main.cpp
└─ zephyr
|  └─ boards
|  └─ dts
|  └─ modules
|  └─>CMakeLists.txt
|  └─>prj.conf
└─>LICENSE
└─>platformio.ini
└─>README.md
```

The `owntech` folder contains scripts and board description for PlatformIO, while the `zephyr` folder contains board decription and OwnTech's Zephyr modules.
By default, these folders (as well as VS Code and PlatformIO folders `.vscode` and `.pio`) are hidden when opening the project in VS Code.

If you need to access these in VS Code, open the project using your file explorer, then in the `.vscode` folder, rename file `settings.json`, e.g. to `settings.json.old`.

Advanced Zephyr configuration can be tweaked by editing `zephyr/prj.conf`.

The OwnTech API source code is located in `zephyr/modules`. If you need to tailor it to your needs, please checkout the [Zephyr documentation](https://docs.zephyrproject.org/3.4.0/).
