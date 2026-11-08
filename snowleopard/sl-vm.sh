#!/bin/bash
# sl-vm.sh: run Mac OS X 10.6 on q35 + OVMF + OpenCore + KVM, HVF or TCG.
#
# Linux x86_64 hosts use KVM, and Intel Macs Hypervisor.framework (HVF).
# Elsewhere (macOS on Apple silicon, or neither available; host.sh decides)
# the guest runs under multi-threaded TCG, which needs three
# things 10.6 cares about: an explicit core topology (left alone TCG
# reports hyperthreads and 10.6 hangs in sched_startup), a TSC above 1 GHz
# (at TCG's 1 GHz 10.6 sets kSlow and panics in commpage_populate; needs
# qemu/patches/0005) and a longer spinlock timeout (make-opencore.sh).
#
#   DISPLAY_MODE=std  sl-vm.sh       # -vga std (installation, reference)
#   DISPLAY_MODE=rv530 sl-vm.sh      # emulated Radeon X1600 as the only GPU
#   DISPLAY_MODE=both sl-vm.sh       # -vga std for the firmware + rv530
#   DISPLAY_MODE=hd5770 sl-vm.sh     # emulated Radeon HD 5770 (evergreen device,
#                                    # experimental): ROM5770 is the option ROM
#                                    # (default: the synthetic AtomBIOS of
#                                    # scripts/atombios/mkrom-evergreen.py),
#                                    # EGOPTS extra device options, and an ESP
#                                    # built with SL_FBNAME=Hoolock
#                                    # SL_GPUMODEL="ATI Radeon HD 5770"
#
# Environment: OSK (the Apple SMC key, required), WORK (the VM's own
# directory for the disk, NVRAM, sockets and logs; default work/, e.g.
# work/lion for a second VM), SYSDISK (default
# $WORK/snowleopard.qcow2, created at 40G if missing), INSTALLER (raw HFS+
# image of the install DVD, attached only if INSTALL=1), ESP (OpenCore
# folder from make-opencore.sh), ROM (VBIOS for the rv530; default the
# synthetic AtomBIOS from scripts/atombios/mkrom.py, built into
# build-rom/ on first use; a real dump also works), QMP (socket
# path), QDISPLAY (QEMU -display backend, default none; gtk for a window),
# VNC (display, default :2), SERIAL (default file:work/serial.log),
# AUDIO (none or usb, with AUDIO_BACKEND / AUDIO_WAV), REFRESH (Hz of
# the EDID's preferred mode, default 60, up to 240; lowered per size to what
# fits dual-link DVI's 330 MHz, e.g. 81 Hz at 2560x1440),
# DND (1: host -> guest drag and drop channel), CLIPBOARD (1: text
# clipboard sync, needs QDISPLAY=gtk or cocoa on the host side), AUTORESIZE
# (1: the guest resolution follows the QEMU window; set it only when you
# want it, an empty value counts as off; View > Resize Guest with Window
# switches it at run time), ACCEL (kvm, hvf, tcg, ...; default from
# host.sh: kvm on x86_64 Linux with /dev/kvm, hvf on an Intel Mac, else
# tcg), TSC_HZ (TCG only, default 1200000000), EXTRA (extra QEMU args).
# The QEMU log goes to work/qemu.log.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${WORK:-$HERE/work}
mkdir -p "$W"
# QEMU from scripts/setup-qemu.sh, else from a release package (bin/)
if [ -z "${QEMU:-}" ]; then
    QEMU=$HERE/../build-qemu/qemu-11.1.3/build/qemu-system-x86_64
    [ -x "$QEMU" ] || [ ! -x "$HERE/../bin/qemu-system-x86_64" ] || QEMU=$HERE/../bin/qemu-system-x86_64
fi
# qemu-img from the same build, so the host needs no qemu-utils package
QEMU_IMG=${QEMU_IMG:-$(dirname "$QEMU")/qemu-img}
[ -x "$QEMU_IMG" ] || QEMU_IMG=qemu-img
SYSDISK=${SYSDISK:-$W/snowleopard.qcow2}
INSTALLER=${INSTALLER:-$W/installer.img}
ESP=${ESP:-$W/esp}
ROM=${ROM:-$HERE/../build-rom/rv530-atombios.rom}
if [ ! -f "$ROM" ] && [ "$ROM" = "$HERE/../build-rom/rv530-atombios.rom" ]; then
    mkdir -p "$HERE/../build-rom"
    python3 "$HERE/../scripts/atombios/mkrom.py" "$ROM" >/dev/null
fi
ROM5770=${ROM5770:-$HERE/../build-rom/hd5770-atombios.rom}
if [ ! -f "$ROM5770" ] && [ "$ROM5770" = "$HERE/../build-rom/hd5770-atombios.rom" ]; then
    mkdir -p "$HERE/../build-rom"
    python3 "$HERE/../scripts/atombios/mkrom-evergreen.py" "$ROM5770" >/dev/null
fi
QMP=${QMP:-$W/qmp.sock}
VNC=${VNC:-:2}
SERIAL=${SERIAL:-file:$W/serial.log}
DISPLAY_MODE=${DISPLAY_MODE:-std}
# OVMF from build-ovmf.sh (with the rv530 GOP driver) when it has been built,
# else a release package's (firmware/), otherwise the distribution's (enough
# for DISPLAY_MODE=std)
if [ -f "$W/fw/OVMF_CODE.fd" ]; then
    OVMF_CODE=${OVMF_CODE:-$W/fw/OVMF_CODE.fd}
    OVMF_VARS_TEMPLATE=${OVMF_VARS_TEMPLATE:-$W/fw/OVMF_VARS.fd}
elif [ -f "$HERE/../firmware/OVMF_CODE.fd" ]; then
    OVMF_CODE=${OVMF_CODE:-$HERE/../firmware/OVMF_CODE.fd}
    OVMF_VARS_TEMPLATE=${OVMF_VARS_TEMPLATE:-$HERE/../firmware/OVMF_VARS.fd}
fi
for d in /usr/share/OVMF/OVMF_%s_4M.fd /usr/share/edk2/x64/OVMF_%s.4m.fd \
         /usr/share/edk2-ovmf/x64/OVMF_%s.4m.fd; do
    if [ -z "${OVMF_CODE:-}" ] && [ -f "$(printf "$d" CODE CODE CODE)" ]; then
        OVMF_CODE=$(printf "$d" CODE CODE CODE)
        OVMF_VARS_TEMPLATE=${OVMF_VARS_TEMPLATE:-$(printf "$d" VARS VARS VARS)}
    fi
done
: "${OVMF_CODE:?set OVMF_CODE (run build-ovmf.sh, or install OVMF)}"
: "${OSK:?set OSK to the SMC key}"
[ -f "$W/OVMF_VARS.fd" ] || cp "$OVMF_VARS_TEMPLATE" "$W/OVMF_VARS.fd"
[ -f "$SYSDISK" ] || "$QEMU_IMG" create -f qcow2 "$SYSDISK" 40G >/dev/null
. "$HERE/host.sh"
if [ -z "${ACCEL:-}" ]; then
    ACCEL=$(host_accel)
    [ "$ACCEL" = tcg ] && ACCEL=tcg,thread=multi,tb-size=512
fi
CPU=Penryn,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on
case "$ACCEL" in
hvf*)
    # Intel Mac: the guest runs on the host's VT-x as under KVM.  kvm= and
    # vmware-cpuid-freq= are KVM's (HVF builds CPUID without its leaves).
    CPU=Penryn,vendor=GenuineIntel,+invtsc ;;
tcg*)
    # see the header: topology, TSC frequency (no invtsc under TCG).
    # 2 GB keeps QEMU out of swap on 8 GB hosts: a vCPU stalled in a
    # page-in for seconds is what 10.6's lock timeouts trip over.
    MEM=${MEM:-2048}
    SMP=${SMP:-4}
    case "$SMP" in *=*) ;; *) SMP="$SMP,sockets=1,cores=$SMP,threads=1" ;; esac
    CPU=Penryn,vendor=GenuineIntel,tsc-frequency=${TSC_HZ:-1200000000} ;;
esac

# The GPU sits behind a conventional PCI bridge at 00:01.0 (the OpenCore
# DeviceProperties path is PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)).  A
# pcie-root-port works too, but Snow Leopard never services the root port's
# own INTx, which shares the GPU's line: the ATI handler then runs ~80k
# times a second and the single vCPU spends most of its time there.
RP=${RP:-pci-bridge,id=rp1,bus=pcie.0,addr=0x1,chassis_nr=1,shpc=off}
# CLIPBOARD=1: text clipboard sync with the guest agent.  The device
# is a QEMU clipboard peer on the work/clip.sock chardev; the agent's
# connection to 10.0.2.100:7651 is relayed to it; the GTK UI (with
# clipboard=on) or the Cocoa UI (always) syncs the host side.
CLIP=()
if [ "${CLIPBOARD:-0}" = 1 ]; then
    rm -f "$W/clip.sock"
    CLIP=(-chardev "socket,id=clip,path=$W/clip.sock,server=on,wait=off")
    DNDFWD="${DNDFWD:-},guestfwd=tcp:10.0.2.100:7651-cmd:python3 '$HERE/../scripts/dnd/relay.py' '$W/clip.sock'"
    RVOPTS="clipboard=clip${RVOPTS:+,$RVOPTS}"
    case "${QDISPLAY:-none}" in gtk*) QDISPLAY="$QDISPLAY,clipboard=on" ;; esac
fi
case "$DISPLAY_MODE" in
std)   DISP=(-vga std) ;;
rv530) DISP=(-vga none -device $RP
             -device rv530,id=gpu,bus=rp1,romfile="$ROM",xres=${XRES:-1440},yres=${YRES:-900},xmax=${XMAX:-2560},ymax=${YMAX:-1600}${REFRESH:+,refresh=$REFRESH}${AUTORESIZE:+,resize=on}${RVOPTS:+,$RVOPTS}) ;;
both)  DISP=(-vga std -device $RP
             -device rv530,id=gpu,bus=rp1,romfile="$ROM",xres=${XRES:-1440},yres=${YRES:-900},xmax=${XMAX:-2560},ymax=${YMAX:-1600}${REFRESH:+,refresh=$REFRESH}${AUTORESIZE:+,resize=on}${RVOPTS:+,$RVOPTS}) ;;
hd5770) DISP=(-vga none -device $RP
             -device radeon-hd5770,id=gpu,bus=rp1,romfile="$ROM5770",xres=${XRES:-1440},yres=${YRES:-900}${EGOPTS:+,$EGOPTS}) ;;
*) echo "DISPLAY_MODE must be std, rv530, both or hd5770" >&2; exit 1 ;;
esac
# AUTORESIZE=1 with the Cocoa window: it can only be resized with Zoom To Fit
if [ -n "${AUTORESIZE:-}" ]; then
    case "${QDISPLAY:-none}" in
    cocoa*zoom-to-fit*) ;;
    cocoa*) QDISPLAY="$QDISPLAY,zoom-to-fit=on" ;;
    esac
fi
# (The guestfwd commands are split like a shell would, by libslirp: the
# paths are quoted for a checkout path with spaces.)
# DND=1: drag and drop from the host.  The guest agent
# (snowleopard/agent) connects to 10.0.2.100:7650; slirp runs dnd-host.py
# for it, which takes drops from scripts/dnd/dnd-send.py on work/dnd.sock.
if [ "${DND:-0}" = 1 ]; then
    DNDFWD="${DNDFWD:-},guestfwd=tcp:10.0.2.100:7650-cmd:python3 '$HERE/../scripts/dnd/dnd-host.py' '$W/dnd.sock'"
    # the GTK or Cocoa window hands drops to dnd-send.py (qemu/patches/0003,
    # 0006)
    export RV530_DND_SEND=$HERE/../scripts/dnd/dnd-send.py RV530_DND_SOCK=$W/dnd.sock
fi
# AUDIO=usb: a USB Audio Class 1 output (QEMU usb-audio) that Apple's own
# AppleUSBAudio drives.  AUDIO_BACKEND picks the host side
# (pipewire, pa, alsa, sdl; wav writes AUDIO_WAV, for headless tests).
SND=()
case "${AUDIO:-none}" in
none) ;;
usb)  # the first driver this QEMU has and can load (a release package has
      # PulseAudio and ALSA, as modules that need the host's libraries)
      AB=${AUDIO_BACKEND:-}
      if [ -z "$AB" ]; then
          case "$(uname -s)" in Darwin) AB=coreaudio ;; *)
              DRV=$("$QEMU" -audiodev help)
              for d in pipewire pa alsa; do
                  if grep -qx "$d" <<<"$DRV"; then AB=$d; break; fi
              done
              [ -n "$AB" ] || { echo "AUDIO=usb: no PipeWire, PulseAudio, or ALSA driver; install libpulse or set AUDIO_BACKEND" >&2; exit 1; } ;;
          esac
      fi
      [ "$AB" = wav ] && AB="wav,path=${AUDIO_WAV:-$W/audio.wav}"
      SND=(-audiodev "$AB,id=snd0" -device usb-audio,audiodev=snd0,bus=uhci.0) ;;
*) echo "AUDIO must be none or usb" >&2; exit 1 ;;
esac
if [ -f "$ESP.img" ]; then
    ESPDRV=(-drive id=esp,if=none,format=raw,file="$ESP.img")
else
    ESPDRV=(-drive id=esp,if=none,format=raw,file=fat:rw:"$ESP")
fi
DISKS=("${ESPDRV[@]}" -device ide-hd,bus=ide.0,drive=esp
       -drive id=sys,if=none,format=qcow2,file="$SYSDISK" -device ide-hd,bus=ide.1,drive=sys)
if [ "${INSTALL:-0}" = 1 ]; then
    # INSTALLER may be the raw HFS+ volume (attached as a disk) or the original
    # DVD image (*.iso, attached as an AHCI DVD drive so its 2048-byte Apple
    # partition map is read correctly by both OpenCore and XNU)
    case "$INSTALLER" in
    *.iso) DISKS+=(-drive id=inst,if=none,format=raw,media=cdrom,readonly=on,file="$INSTALLER"
                   -device ide-cd,bus=ide.2,drive=inst) ;;
    *)     DISKS+=(-drive id=inst,if=none,format=raw,snapshot=on,file="$INSTALLER"
                   -device ide-hd,bus=ide.2,drive=inst) ;;
    esac
fi
rm -f "$QMP"
exec "$QEMU" -name snowleopard \
  -M q35,i8042=off -accel "$ACCEL" -m ${MEM:-4096} -smp ${SMP:-4} \
  -global ICH9-LPC.disable_s3=1 -global ICH9-LPC.disable_s4=1 \
  -cpu "$CPU" \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$W/OVMF_VARS.fd" \
  -smbios type=2 -device isa-applesmc,osk="$OSK" \
  "${DISP[@]}" "${DISKS[@]}" \
  -device piix3-usb-uhci,id=uhci -device usb-kbd,bus=uhci.0 -device usb-tablet,bus=uhci.0 \
  ${SND[@]+"${SND[@]}"} ${CLIP[@]+"${CLIP[@]}"} \
  -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:${SSHPORT:-2222}-:22${DNDFWD:-}" -device e1000-82545em,netdev=n0 \
  -serial "$SERIAL" -monitor none -qmp unix:"$QMP",server=on,wait=off \
  -display "${QDISPLAY:-none}" -vnc "$VNC" \
  -D "$W/qemu.log" -d ${QDEBUG:-guest_errors,unimp} \
  ${EXTRA:-}
