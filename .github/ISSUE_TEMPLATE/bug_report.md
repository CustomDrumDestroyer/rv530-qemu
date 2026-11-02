---
name: Bug report
about: Something crashes, renders wrong, or doesn't work
labels: bug
---

**What happened, and what did you expect?**


**Steps to reproduce** (does it happen every time, or only sometimes?)

1.

**Host**

- OS and version (x86_64 Linux, Intel Mac, or Apple silicon Mac):
- CPU and GPU:
- Accelerator (KVM, Hypervisor.framework, or TCG):

**Guest**

- Mac OS X version (or Linux guest):
- vCPU count and memory:
- Command line or `sl-vm.sh` settings:

**Logs and screenshots**

Useful: `snowleopard/work/qemu.log`, the guest's panic report, and a screenshot.
Please remove anything private first, such as your Apple SMC key, serial
numbers, and home folder paths.

**Did you narrow it down?** (optional)

Does it still happen with `vulkan=off`, or with `SMP=1`?
