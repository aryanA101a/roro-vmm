# Build the initramfs

Run from the Roro project root on Linux:

```sh
project_dir="$(pwd)"
mkdir -p "$HOME/roro-build"
cd "$HOME/roro-build"

curl -fLO https://buildroot.org/downloads/buildroot-2026.08.tar.xz
tar -xf buildroot-2026.08.tar.xz
make -C buildroot-2026.08 O="$HOME/roro-build/output" \
    BR2_DEFCONFIG="$project_dir/guest/buildroot_defconfig" defconfig
make -C buildroot-2026.08 O="$HOME/roro-build/output"

```

Output:

```text
~/roro-build/output/images/rootfs.cpio.gz
```
