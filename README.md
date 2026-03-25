# Linux-bare_metal_hypervizor-amd
VMAware with sudo 0/89

Eklenecekler: Önceden bellek ayırma,
LBR izleme, Snapshot mantığını sadece değişen sayfalar için ayarlamak /proc da daha iyi python kodu ile sha veya aes key yakalama özelliği

sudo pacman -S base-devel linux-headers

git clone https://github.com/Hakan4178/Linux-bare_metal_hypervizor-amd.git
cd Linux-bare_metal_hypervizor-amd

make

sudo insmod ring_minus_one.ko

lsmod | grep ring

Not: Çakışma olursa: sudo modprobe -r kvm_amd kvm

Kaldırmak için: sudo rmmod ring_minus_one


