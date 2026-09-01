# Trust FF380 RaceMaster - Linux driver

Linux kernel USB driver for the **Trust FF380 RaceMaster** steering wheel with pedals (USB ID `06D6:005F`). 

Supports:
- Steering axis
- All 8 buttons and D-pad
- Gas and brake pedals (separate axes)
- Force feedback

Force feedback is handled entirely by the kernel's built-in **ff-memless**
layer.  All effect types (constant, spring, damper, friction, inertia,
periodic, ramp) are evaluated and mixed by the kernel.  The driver
receives the final force, converts it to correct force byte and sends it to the wheel.

For more infomation on hardware aspect of the Trust FF380 RaceMaster steering wheel see [device-info.md](./device-info.md).

## Driver files

| File | Purpose |
|---|---|
| `ff380.c` | Driver source code |
| `Kbuild` | Kernel build system descriptor |
| `Makefile` | Out-of-tree build wrapper |
| `99-ff380.rules` | udev rules (auto-load + non-root access) |

## Build & install

Driver was tested only on `Linux Mint 22.3 - Cinnamon 64-bit`, kernel: `6.17.0-22-generic`.

```bash
# Install kernel headers (Linux Mint / Ubuntu / Debian)
sudo apt install linux-headers-$(uname -r) build-essential

# Move to folder where ff380.c file is

# Build from source code
make

# Install the module (copies to /lib/modules/… and runs depmod)
sudo make install

# Tell usbhid to ignore this wheel so this driver can claim it
echo "options usbhid quirks=0x06D6:0x005F:0x0004" | sudo tee /etc/modprobe.d/ff380.conf

# Embed the quirk into initramfs so it applies at boot
sudo update-initramfs -u

# Install udev rule for auto-load and non-root access
sudo cp 99-ff380.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules

# Add yourself to plugdev group if not already
sudo usermod -aG plugdev $USER
```

Then reboot computer. After reboot plug in the wheel and it loads automatically.

You can check if the wheel was detected by the driver with command `dmesg | grep ff380`. If it works, you’ll see `Trust FF380 RaceMaster connected`.

## Uninstall
```bash
sudo rmmod ff380
sudo rm /etc/modprobe.d/ff380.conf
sudo rm /etc/udev/rules.d/99-ff380.rules
sudo rm /lib/modules/$(uname -r)/updates/ff380.ko
sudo depmod -a
sudo update-initramfs -u
```

Then reboot computer.

---

*The driver was made by me with the help of AI.*
