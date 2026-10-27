# Roro VMM

Roro is a minimal hypervisor written in C using Linux KVM. It boots
an x86-64 Linux kernel and initramfs through the Linux 32-bit boot protocol, with
an interactive terminal console through an emulated UART.

The guest has one vCPU and 128 MiB of RAM. There is no BIOS, disk, networking,
or graphics support.

## Requirements

- An x86-64 Linux host with KVM enabled and permission to access `/dev/kvm`.
- A C compiler, Make, and Linux development headers.
- An x86-64 Linux `bzImage` and an initramfs containing `/init` and a serial console.

## Run

Guest images are distributed as GitHub Release assets, separately from the source.
Place `bzImage` and `rootfs.cpio.gz` in the project root, or build them using the
[kernel](guest/BUILD_KERNEL.md) and [initramfs](guest/BUILD_INITRAMFS.md) guides.

```sh
make run

# Optional overrides
make run KERNEL=/other/bzImage INITRAMFS=/other/rootfs.cpio.gz

# Print VMM diagnostics
make run VMM_ARGS=-v
```

`make run` builds the VMM automatically. To build and invoke it directly:

```sh
make
./vmm -k bzImage -i rootfs.cpio.gz
./vmm --help
```

Press **Ctrl+C** to exit the VMM.
