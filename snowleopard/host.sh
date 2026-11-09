# host.sh: sourced by sl-vm.sh and make-opencore.sh.  host_accel prints the
# accelerator the guest runs under on this host:
#   kvm  x86_64 Linux with a usable /dev/kvm (Arm64 Linux has one too, for
#        Arm guests, and cannot run this x86 guest on it)
#   hvf  an Intel Mac (Hypervisor.framework; kern.hv_support is 1)
#   tcg  anything else: Apple silicon, Windows, or an Intel Mac or Linux host
#        without hardware virtualization
# Apple silicon is told apart by hw.optional.arm64, not uname -m, which says
# x86_64 in a shell running under Rosetta.
# Windows: MSYS2 or Git for Windows bash
is_windows() {
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) return 0 ;; *) return 1 ;; esac
}

host_accel() {
    if [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] && [ -w /dev/kvm ]; then
        echo kvm
    elif [ "$(uname -s)" = Darwin ] &&
         [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != 1 ] &&
         [ "$(sysctl -n kern.hv_support 2>/dev/null || echo 0)" = 1 ]; then
        echo hvf
    else
        echo tcg
    fi
}
