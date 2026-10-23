# UEFI-Dali — Mu-Silicium for REDMI K80 Ultra

This repository owns the dali / MT6991 **UEFI firmware port**. See
[dali build and bring-up notes](DALI.md) for the current state and build steps.

The project follows UniXNU's repository separation:

| Repository | Responsibility |
| --- | --- |
| [UEFI-Dali](https://github.com/localhosts-A/UEFI-Dali) | Firmware, BootShim, MT6991 platform drivers, diagnostics and boot-v4 packaging |
| [UniXNU-Dali](https://github.com/localhosts-A/UniXNU-Dali) | `board.toml`, device payload configuration and device build entry points |
| [UniXNU](https://github.com/localhosts-A/UniXNU) | Generic boot manager, build driver, DarwinBoot and shared modules; tracks [upstream](https://github.com/UniXNU/UniXNU) |

Device backups, runtime DTBs, logs, built images and private firmware inputs are
kept outside Git. This port is under active bring-up; see DALI.md for tested
capabilities and remaining work.

The UEFI test milestone passed on hardware: UniXNU menu, HelloPayload,
return to firmware, all three physical keys and original Android recovery.
The source-built XNU path is now in bring-up and still requires its own build,
QEMU and hardware validation. Physical storage and USB transport remain future work.

## Upstream Project Silicium

<!-- ![Banner](https://github.com/Project-Silicium/Mu-Silicium/blob/main/Resources/Pictures/Banner.png) -->

## Description

This Project focuses on Making ARM-Based Devices Boot Operating Systems like Windows or Linux using Custom UEFI Firmware.

## Resources

- [Status](Status.md)
- [Building](Building.md)
- [Guides](https://github.com/Project-Silicium/Guides/blob/main/README.md)
- [Discord Server](https://discord.gg/Dx2QgMx7Sv)

## Frequently asked Questions

### **Is my Device Supported in this Project?**
- To Check if your Device is Supported, Take a Look at the Status Page Referenced in the Section Above.

### **What do I do if my Device isn't Supported in this Project?**
- We made some Guides that Explain How to Port your Device that are Referenced in the Section Above.

## Credits

[Microsoft](https://github.com/microsoft/) for their [Project Mu](https://microsoft.github.io/mu/) which Powers this Project. <br>
[WOA-Project](https://github.com/WOA-Project/) for their [mu_andromeda_platforms](https://github.com/WOA-Project/mu_andromeda_platforms/) which is used as Base for this Project. <br>
[Project Aloha](https://github.com/Project-Aloha/) for their [mu_aloha_platforms](https://github.com/Project-Aloha/mu_aloha_platforms/) which is used as Reference for some SoCs. <br>
[Open Surface RT](https://openrt.gitbook.io/open-surfacert/) for their Help on Tegra Related Stuff.

[sonic011gamer](https://github.com/sonic011gamer/) and [halal-beef](https://github.com/halal-beef/) for their Amazing Work on Exynos Related Stuff.

## License

All code except drivers in `GPLDrivers` & `GPLApplications` & `GPLLibrary` directories are licensed under BSD-2-Clause. <br />
GPL Drivers are licensed under `GPLv2` or later license.
