#!/bin/bash
# make-opencore.sh: build an OpenCore ESP folder for booting Mac OS X 10.6
# (installer or installed system) on QEMU q35 + OVMF + KVM.
#
# Usage: snowleopard/make-opencore.sh [OpenCore-x.y.z-{RELEASE,DEBUG}.zip [outdir]]
#   Without a zip, downloads OpenCore 1.0.8 (the tested version; OC_VARIANT
#   DEBUG, the default, or RELEASE) into work/ and checks its SHA-256.
#   outdir defaults to snowleopard/work/esp; sl-vm.sh uses outdir.img, a
#   FAT image of it, when present (written at the end).
#   SL_PICKER=1: show OpenCore's boot picker (needed to start the
#   installer); SL_VERBOSE=1: verbose kernel boot.
#
# No third-party kexts: SMC comes from QEMU's isa-applesmc, the display is
# either -vga std (installation) or the emulated rv530 (driver bring-up).
# The only injected kext is our codeless kexts/QemuTabletFix.kext (pointer
# scaling of QEMU's usb-tablet, see its Info.plist).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -n "${1:-}" ]; then
    ZIP=$(realpath "$1")
else
    V=${OC_VARIANT:-DEBUG}
    case "$V" in
    DEBUG)   SUM=3f5f42b85703cf2c08e8dcd9245a52fb4326c9f4ef335c75b49f516cbbe131c6 ;;
    RELEASE) SUM=2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b ;;
    *) echo "OC_VARIANT must be DEBUG or RELEASE" >&2; exit 1 ;;
    esac
    mkdir -p "$HERE/work"
    ZIP=$HERE/work/OpenCore-1.0.8-$V.zip
    [ -f "$ZIP" ] || curl -fL -o "$ZIP" \
        "https://github.com/acidanthera/OpenCorePkg/releases/download/1.0.8/OpenCore-1.0.8-$V.zip"
    # macOS's sha256sum (14 and later) is BSD's, without GNU's --quiet, and
    # earlier macOS has only shasum: compare the hash itself
    if [ "$(uname -s)" = Darwin ]; then HASH=$(shasum -a 256 "$ZIP"); else HASH=$(sha256sum "$ZIP"); fi
    [ "${HASH%% *}" = "$SUM" ] || { echo "$ZIP: SHA-256 mismatch (delete it to download again)" >&2; exit 1; }
fi
OUT=${2:-$HERE/work/esp}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" -d "$TMP"
rm -rf "$OUT"
mkdir -p "$OUT/EFI/BOOT" "$OUT/EFI/OC/Drivers" "$OUT/EFI/OC/Kexts" "$OUT/EFI/OC/ACPI" \
         "$OUT/EFI/OC/Tools" "$OUT/EFI/OC/Resources"
cp "$TMP/X64/EFI/BOOT/BOOTx64.efi" "$OUT/EFI/BOOT/"
# OpenCore lists its own disk as a boot entry; hide it (see Misc/Boot below)
[ "${SL_PICKER:-0}" = 1 ] || printf 'Disabled' > "$OUT/EFI/BOOT/.contentVisibility"
cp "$TMP/X64/EFI/OC/OpenCore.efi" "$OUT/EFI/OC/"
for d in OpenRuntime OpenHfsPlus OpenPartitionDxe ResetNvramEntry; do
    cp "$TMP/X64/EFI/OC/Drivers/$d.efi" "$OUT/EFI/OC/Drivers/"
done
cp "$TMP/X64/EFI/OC/Tools/OpenShell.efi" "$OUT/EFI/OC/Tools/"
cp -r "$HERE/kexts/QemuTabletFix.kext" "$OUT/EFI/OC/Kexts/"

# The guest's lock timeouts follow the accelerator sl-vm.sh will use
# (host.sh): KVM and HVF run it on the host's VT-x, TCG emulates it.
. "$HERE/host.sh"
case "$(host_accel)" in
kvm|hvf) export SL_SLTO=${SL_SLTO:-1000000} SL_TCG=${SL_TCG:-0} ;;
*)       export SL_SLTO=${SL_SLTO:-3000000} SL_TCG=${SL_TCG:-1} ;;
esac
python3 - "$TMP/Docs/Sample.plist" "$OUT/EFI/OC/config.plist" <<'EOF'
import os, plistlib, sys, uuid
p = plistlib.load(open(sys.argv[1], 'rb'))
for k in [k for k in p if k.startswith('#')]:
    del p[k]

# One ACPI patch, one codeless kext.  QEMU's RTC _CRS reserves 8 ports at
# 0x70 but only decodes 0x70/0x71.  AppleRTC sizes its CMOS banks from that
# range and reads the prior-boot sleep/wake trace from the upper bank
# (0xb0-0xb7 via 0x72/0x73); unassigned ports read 0xff, so PMStatusCode
# comes out all ones and configd's PowerManagement plugin indexes past
# PCITopLevel and segfaults at every boot (the network then reads offline
# until mDNSResponder restarts).  Two ports = one bank, trace reads 0.
p['ACPI']['Add'] = []
p['ACPI']['Delete'] = []
p['ACPI']['Patch'] = [dict(Base='\\_SB.PCI0.SF8.RTC', BaseSkip=0, Comment='RTC: 2 ports (one CMOS bank)',
                           Count=1, Enabled=True, Find=bytes.fromhex('4701700070000108'), Limit=0,
                           Mask=b'', OemTableId=bytes(8), Replace=bytes.fromhex('4701700070000102'),
                           ReplaceMask=b'', Skip=0, TableLength=0, TableSignature=b'DSDT')]
p['Kernel']['Add'] = [dict(Arch='Any', BundlePath='QemuTabletFix.kext', Comment='usb-tablet scaling',
                           Enabled=True, ExecutablePath='', MaxKernel='', MinKernel='',
                           PlistPath='Contents/Info.plist')]
p['Kernel']['Block'] = []
p['Kernel']['Patch'] = []
# SL_TCG=1 (default where host.sh finds neither KVM nor HVF, so the guest
# runs under TCG):
# an emulated vCPU can be held up for seconds (host scheduling, swapping),
# which 10.6 takes for a deadlock and panics ("Spinlock acquisition timed
# out", "TLB invalidation IPI timeout").  Make both waits unbounded: the
# hw_lock_to loop's "time left" jb becomes jmp, and pmap_flush_tlbs's
# "deadline not reached" jae becomes nop+jmp.  Patterns are unique in the
# 10.6.x kernels.
if os.environ.get('SL_TCG') == '1':
    for comment, find, repl in (
            ('TCG: hw_lock_to never times out',
             '4839f072d64831c04159c3', '4839f0ebd64831c04159c3'),
            ('TCG: TLB shootdown never times out',
             '4939c60f838d000000', '4939c690e98d000000')):
        p['Kernel']['Patch'].append(dict(
            Arch='x86_64', Base='', Comment=comment, Count=1, Enabled=True,
            Find=bytes.fromhex(find), Identifier='kernel', Limit=0, Mask=b'',
            MaxKernel='10.99.99', MinKernel='10.0.0', Replace=bytes.fromhex(repl),
            ReplaceMask=b'', Skip=0))
p['Kernel']['Force'] = []
p['Kernel']['Scheme']['KernelArch'] = 'x86_64'   # X1000 kexts are universal
p['Kernel']['Scheme']['KernelCache'] = os.environ.get('SL_KCACHE', 'Mkext')
p['Kernel']['Quirks']['ProvideCurrentCpuInfo'] = True
p['Kernel']['Quirks']['DisableLinkeditJettison'] = True
p['Kernel']['Emulate']['DummyPowerManagement'] = True

p['Booter']['MmioWhitelist'] = []
p['Booter']['Patch'] = []
q = p['Booter']['Quirks']
q.update(AvoidRuntimeDefrag=True, EnableSafeModeSlide=True, EnableWriteUnprotector=True,
         ProvideCustomSlide=True, SetupVirtualMap=True, RebuildAppleMemoryMap=True,
         DevirtualiseMmio=False, ProtectUefiServices=False,   # devirt of the OVMF flash breaks runtime SetVariable (bless panic)
         AllowRelocationBlock=True)   # 10.6 has no KASLR: kernel needs low memory

# Real Macs get the ATI framebuffer personality from EFI device properties:
# ATISupport creates one IOFBStub per "@N," prefix and ATIFramebuffer matches
# on its name.  The rv530 sits behind the q35 root port at 00:01.0.
fb = 'ATY,' + os.environ.get('SL_FBNAME', 'Wormy')
gpu = {'model': 'ATI Radeon X1600', 'name': fb + 'Parent', 'device_type': fb + 'Parent',
       'VRAM,totalsize': (256 << 20).to_bytes(4, 'little'),
       'ATY,Copyright': b'Copyright AMD Inc. All Rights Reserved. 2005-2009',
       'ATY,PlatformInfo': bytes([int(os.environ.get('SL_FBCFG', '0'))]) + bytes(127),   # [0] = connector config index
       'ATY,EFIVersion': b'01.00.068', 'ATY,Rom#': b'113-xxxxxx-xxx', 'ATY,Card#': b'109-A52027-00'}
for i in (0, 1):
    gpu.update({f'@{i},name': fb, f'@{i},compatible': fb, f'@{i},device_type': 'display'})
p['DeviceProperties']['Add'] = {os.environ.get('SL_GPUPATH', 'PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)'): gpu}
p['DeviceProperties']['Delete'] = {}

m = p['Misc']
# Straight to the Apple logo: no picker (hold Option at power-on for it, or
# build with SL_PICKER=1), tools hidden, and the ESP's own BOOTx64.efi entry
# hidden by EFI/BOOT/.contentVisibility, so the one macOS entry boots.
picker = os.environ.get('SL_PICKER') == '1'
m['Boot'].update(Timeout=0, HideAuxiliary=not picker, PickerMode='Builtin',
                 PollAppleHotKeys=True, ShowPicker=picker)
m['Debug'].update(AppleDebug=True, ApplePanic=True, DisableWatchDog=True,
                  Target=0x09, DisplayLevel=int(os.environ.get('SL_OCLEVEL', '0x80000042'), 16))   # enable + serial
m['Serial']['Init'] = True
m['Security'].update(SecureBootModel='Disabled', Vault='Optional', ScanPolicy=0,
                     DmgLoading='Any', AllowSetDefault=True, ExposeSensitiveData=6)
m['Entries'] = []
m['Tools'] = [dict(Arguments='', Auxiliary=True, Comment='UEFI shell',
                   Enabled=True, Flavour='OpenShell:UEFIShell:Shell', FullNvramAccess=False,
                   Name='UEFI Shell', Path='OpenShell.efi', RealPath=False,
                   TextMode=False)]

nv = p['NVRAM']
apple = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
# slto_us: xnu's LockTimeOut (spinlock acquisition and TLB shootdown IPI
# timeouts) defaults to 250 ms.  A vCPU descheduled on the host, or one held
# in QEMU by a long device stall, trips it and panics an SMP guest.  The
# spinlock check uses LockTimeOutTSC, a 32-bit count of TSC ticks, so the
# value must stay below 2^32 / TSC Hz: 2 s at 2.4 GHz wraps to ~225 ms.
# 1 s is safe up to a 4.29 GHz TSC.  Under TCG (any non-x86_64 host) vCPUs
# are slow enough to trip 1 s with 4 vCPUs; sl-vm.sh runs the TCG TSC at
# 1.2 GHz there, so SL_SLTO defaults to 3 s (3.6e9 ticks still fits).
# debug=: keep bit 0x4 (DB_NMI) off.  With it the power button (QEMU's
# system_powerdown) drops the kernel into the debugger, which waits for a
# remote KDP connection: the whole guest freezes and the debugger takes
# over the network card, so ssh dies too.
nv['Add'][apple] = {
    # SL_VERBOSE=1: the text boot log instead of the Apple logo
    'boot-args': os.environ.get('SL_BOOTARGS', ('-v ' if os.environ.get('SL_VERBOSE') == '1' else '') +
                                'keepsyms=1 debug=0x100 arch=x86_64 slto_us=' + os.environ['SL_SLTO']),
    'csr-active-config': b'\x00\x00\x00\x00',
    'prev-lang:kbd': b'en:0',
    'run-efi-updater': 'No',
}
nv['Delete'][apple] = ['boot-args', 'csr-active-config', 'prev-lang:kbd']
nv['WriteFlash'] = True

pi = p['PlatformInfo']
pi['Automatic'] = True
pi['Generic'].update(SystemProductName='iMac8,1',        # 10.6.0 boot.efi allows K64 only for iMac8,1+
                     SystemSerialNumber='W00000000001', MLB='M0000000000000001',
                     SystemUUID=os.environ.get('SL_UUID', str(uuid.uuid4()).upper()), ROM=b'\x52\x56\x35\x33\x30\x00',
                     SpoofVendor=True)

u = p['UEFI']
u['Drivers'] = [dict(Arguments='', Comment='', Enabled=True, HideVerbose=False, LoadEarly=False, Path=d)
                for d in ('OpenRuntime.efi', 'OpenHfsPlus.efi', 'OpenPartitionDxe.efi',
                          'ResetNvramEntry.efi')]
u['Quirks']['RequestBootVarRouting'] = True
u['Output'].update(ProvideConsoleGop=True, Resolution='1440x900', TextRenderer='BuiltinText',   # graphics mode at handoff makes 10.6 boot.efi Blt before GOP init
                   UgaPassThrough=True, InitialMode='Text')
plistlib.dump(p, open(sys.argv[2], 'wb'))
print('wrote', sys.argv[2])
EOF
case "$(uname -s)" in
Darwin) OCV=$TMP/Utilities/ocvalidate/ocvalidate ;;
*)      OCV=$TMP/Utilities/ocvalidate/ocvalidate.linux ;;
esac
if [ -f "$OCV" ]; then
    chmod +x "$OCV"
    "$OCV" "$OUT/EFI/OC/config.plist" | tail -4 || true
fi
echo "ESP ready: $OUT"

# Real FAT image too: QEMU's vvfat asserts when Mac OS X writes to the ESP.
IMG=${OUT%/}.img
rm -f "$IMG"
# macOS has no truncate(1); Homebrew's dosfstools (Apple silicon) and
# MacPorts' (Intel Macs) put mkfs.fat in sbin, and MacPorts' mtools (mcopy)
# is in /opt/local/bin, which a login shell may not have on PATH
PATH=$PATH:/opt/homebrew/sbin:/opt/local/sbin:/opt/local/bin:/usr/local/sbin
python3 -c 'import sys; open(sys.argv[1], "wb").truncate(256 << 20)' "$IMG"
mkfs.fat -F 32 -n OPENCORE "$IMG" >/dev/null
MTOOLS_SKIP_CHECK=1 mcopy -s -i "$IMG" "$OUT/EFI" ::/
echo "ESP image: $IMG"
