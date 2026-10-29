#!/bin/sh
# m0-gadget-setup.sh —— 在 Linux VM 内配置 f_uac2 gadget 并通过 usbip-vudc 导出
# 由 systemd oneshot（cloud-init 写入）在启动时调用。幂等。
set -x
exec > /var/log/m0-gadget.log 2>&1

modprobe usbip_core
modprobe usbip_vudc || modprobe usbip-vudc
modprobe libcomposite

UDC_NAME=$(ls /sys/class/udc | grep -i usbip | head -1)
[ -z "$UDC_NAME" ] && { echo "no usbip udc found"; exit 1; }
UDC_PATH=/sys/class/udc/$UDC_NAME

G=/sys/kernel/config/usb_gadget/g1
[ -d "$G" ] && exit 0            # 已配置

mkdir -p "$G"
cd "$G"
echo 0x1d6b > idVendor           # Linux Foundation
echo 0x0104 > idProduct          # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "M0-PROBE"     > strings/0x409/manufacturer
echo "UAC2 f_uac2"  > strings/0x409/product
echo "M0000000001"  > strings/0x409/serialnumber

mkdir -p configs/b.1/strings/0x409
echo "UAC2" > configs/b.1/strings/0x409/configuration
echo 500    > configs/b.1/MaxPower

mkdir -p functions/uac2.0
# 48kHz 立体声 16bit（f_uac2 属性；保持默认也行，这里显式化）
echo 2 > functions/uac2.0/c_ssize 2>/dev/null
echo 2 > functions/uac2.0/p_ssize 2>/dev/null
echo 0 > functions/uac2.0/c_mute  2>/dev/null
echo 0 > functions/uac2.0/p_mute  2>/dev/null

ln -sf functions/uac2.0 configs/b.1/uac2.0
echo "$UDC_NAME" > UDC

# usbip 服务端：导出 vudc 设备
USBIPD=$(ls /usr/lib/linux-tools/*/usbip /usr/lib/linux-tools/*/usbipd 2>/dev/null | head -2)
USBIPD_BIN=$(echo "$USBIPD" | grep usbipd$ | head -1)
[ -z "$USBIPD_BIN" ] && USBIPD_BIN=$(command -v usbipd)
"$USBIPD_BIN" -D --device "$UDC_PATH" || usbipd -D --device
echo "gadget exported on $UDC_PATH"
