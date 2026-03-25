# Linux-bare_metal_hypervizor-amd
VMAware with sudo 0/89


Özellikleri:


​ASID Kullanımı: TLB flush işlemlerini minimize ederek hem performansı bare-metal seviyesine çektim hem de önbellek analizlerini (cache analysis) boşa çıkardım.

​Dinamik TSC Offsetleme: Sadece sabit bir zaman kaydırması yapmıyorum; Cache-miss durumlarını algılayıp, VMEXIT sırasında oluşan gecikmeyi nanosaniye hassasiyetinde dinamik olarak kompanse ediyorum.
​Threshold Yönetimi: VMCALL, UD2 (geçersiz opcode) ve MSR erişimlerinde işlemcinin gerçek tepki sürelerini (cycle count) taklit ederek, sentetik bare-metal testlerini gecebiliyorum.

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


