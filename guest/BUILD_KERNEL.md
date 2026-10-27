# Build the kernel

Run from the Linux source directory on an x86-64 Linux host:

```sh
make O=build-x86_64 ARCH=x86_64 x86_64_defconfig
make O=build-x86_64 ARCH=x86_64 -j"$(nproc)" bzImage
```

Output:

```text
build-x86_64/arch/x86/boot/bzImage
```
