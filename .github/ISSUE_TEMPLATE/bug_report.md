---
name: Bug report
about: Something crashes, renders incorrectly, or does not work
labels: bug
---

**What occurred, and what did you expect?**


**Steps to make it occur again** (does it occur each time, or only sometimes?)

1.

**Host**

- OS and version (x86_64 Linux, Intel Mac, or Apple silicon Mac):
- CPU and GPU:
- Accelerator (KVM, Hypervisor.framework, or TCG):

**Guest**

- Mac OS X version (or Linux guest):
- Number of vCPUs, and memory:
- Command line or `sl-vm.sh` settings:

**Logs and screenshots**

Useful items: `snowleopard/work/qemu.log`, the panic report of the guest, and a
screenshot. First, remove all private data, for example your Apple SMC key,
serial numbers, and the paths of your home folder.

**Did you find the cause?** (optional)

Does it still occur with `vulkan=off` or with `SMP=1`?
