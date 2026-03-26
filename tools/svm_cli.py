#!/usr/bin/env python3
"""
SVM CLI - Unified Ring-1 Snapshot Tool (V5.0)
Replaces manual /proc/echo interactions with a single clean interface.
"""

import sys
import os
import time
import struct
import argparse

PROC_DIR = "/proc/svm_dump"

# Extraction definitions
HEADER_FMT = "<4sIiQQQQQQI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

def xor_checksum(data):
    seed = 0x5356444D48414B41
    cksum = seed
    for i in range(0, len(data) - 7, 8):
        word = struct.unpack_from("<Q", data, i)[0]
        cksum ^= word
    return cksum & 0xFFFFFFFFFFFFFFFF

def verify_checksum(raw_data, total_size, stored_checksum):
    cksum_offset = 52 
    modified = bytearray(raw_data[:total_size])
    if len(modified) < cksum_offset + 8: return False, 0
    for i in range(8): modified[cksum_offset + i] = 0
    computed = xor_checksum(bytes(modified))
    return computed == stored_checksum, computed

def extract_dump(output_file):
    input_file = f"{PROC_DIR}/output"
    print("[*] RAM okunuyor (Ring -1 Belleği)...")
    try:
        with open(input_file, "rb") as f:
            raw_data = f.read()
    except Exception as e:
        print(f"[!] Veri okunamadı: {e}")
        return

    if len(raw_data) < HEADER_SIZE:
        print("[!] Geçersiz snapshot boyutu.")
        return

    magic, version, pid, flags, ts, cr3, vma_count, map_count, total_size, checksum = struct.unpack_from(HEADER_FMT, raw_data, 0)
    
    if magic != b'SVMD':
        print("[!] HATA: Büyülü imza bulunamadı (SVMD).")
        return

    print(f"\n[+] Snapshot Bilgileri:")
    print(f"    - Modül Versiyon: {version}")
    print(f"    - Hedef PID     : {pid}")
    print(f"    - Kernel CR3    : 0x{cr3:x}")
    print(f"    - VMA Sayısı    : {vma_count}")
    print(f"    - Toplam Alan   : {total_size / (1024*1024):.2f} MB")

    ok, comp = verify_checksum(raw_data, total_size, checksum)
    if ok:
        print("    - Integrity   : [OK] Sağlam")
    else:
        print(f"    - Integrity   : [FAIL] Checksum Uyuşmazlığı! Istenen: {checksum:x}, Hesaplanan: {comp:x}")

    try:
        with open(output_file, "wb") as f:
            f.write(raw_data[:total_size])
        print(f"\n[+] Snapshot başarıyla diske kaydedildi: {output_file}")
    except Exception as e:
        print(f"[!] Dosya yazılamadı: {e}")


def write_proc(name, val):
    try:
        with open(os.path.join(PROC_DIR, name), "w") as f:
            f.write(str(val))
    except Exception as e:
        print(f"[!] {name} dosyasına yazılamadı: {e}")
        sys.exit(1)

def read_proc(name):
    try:
        with open(os.path.join(PROC_DIR, name), "r") as f:
            return f.read().strip()
    except Exception as e:
        return ""

def is_ready():
    status = read_proc("status")
    for line in status.split("\n"):
        if line.startswith("Ready:"):
            return "YES" in line
    return False

def cmd_dump(args):
    print(f"[*] Hedef PID {args.pid} için Snapshot tetikleniyor...")
    write_proc("target_pid", args.pid)
    write_proc("full_dump", "1")
    
    print("[*] Kernel hipervizörünün hafızayı dondurup kopyalaması bekleniyor", end="")
    sys.stdout.flush()
    
    # 30 saniyeye kadar bekleme (büyük oyunların RAM'ini çekmek birkaç saniye sürebilir)
    for _ in range(30):
        if is_ready():
            print("\n")
            extract_dump(args.out)
            return
        time.sleep(1)
        sys.stdout.write(".")
        sys.stdout.flush()
        
    print("\n[!] İşlem 30 saniye içinde tamamlanamadı (Zaman Aşımı veya Kernel reddetti).")

def cmd_watch(args):
    print(f"[*] İzleniyor: '{args.name}'\n[*] Kernel Auto-Watch modu aktif edildi, uygulama bekleniyor...")
    
    # Eskiyi temizle
    write_proc("target_pid", "0")
    write_proc("watch_name", args.name)
    write_proc("full_dump", "1")
    write_proc("auto_watch", "1")
    
    try:
        while True:
            if is_ready():
                print("\n[+] Süreç tespit edildi, Ring -1 Snapshot başarıyla yakalandı!")
                extract_dump(args.out)
                write_proc("auto_watch", "0") # Oto modu kapat
                break
            time.sleep(0.5)
            sys.stdout.write(".")
            sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n[*] İptal edildi. Auto-Watch kapatılıyor...")
        write_proc("auto_watch", "0")

def cmd_list(args):
    print("\n=== SVM DUMP AKTİF SÜREÇ LİSTESİ ===")
    print(read_proc("process_list"))
    print("===================================\n")

import math
from collections import deque

def calculate_entropy(data):
    if not data:
        return 0.0
    entropy = 0
    for x in range(256):
        p_x = data.count(x) / len(data)
        if p_x > 0:
            entropy += - p_x * math.log2(p_x)
    return entropy

def read_exactly(f, size):
    buf = bytearray()
    while len(buf) < size:
        chunk = f.read(size - len(buf))
        if not chunk:
            time.sleep(0.01)
            continue
        buf.extend(chunk)
    return bytes(buf)

def cmd_trace(args):
    """
    Sürekli (Continuous) Ring Buffer izleme modu (Phase 4).
    LBR (Code tracing) ve NPT NPF (Data tracing) eventlerini /proc/svm_trace üzerinden okur.
    LBR geçmişi ile bellek mutasyonlarını eşleştirir ve entropi hesaplar.
    """
    print("[*] Ring -1 Continuous Trace Engine başlatılıyor... (Çıkmak için Ctrl+C)")
    trace_file = "/proc/svm_trace"
    
    if not os.path.exists(trace_file):
        print(f"[!] HATA: {trace_file} bulunamadı! V6.0 modülü yüklü mü?")
        sys.exit(1)
        
    if args.out_dir and not os.path.exists(args.out_dir):
        os.makedirs(args.out_dir)
        
    ENTRY_FMT = "<QQIIQQQII32Q"
    ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
    
    # Son 16 dallanmayı hafızada tutarak mutasyonla eşleştirme
    recent_branches = deque(maxlen=16)
    
    try:
        with open(trace_file, "rb") as f:
            while True:
                header_data = read_exactly(f, ENTRY_SIZE)
                
                unpacked = struct.unpack(ENTRY_FMT, header_data)
                magic = unpacked[0]
                tsc = unpacked[1]
                ev_type = unpacked[2]
                lbr_count = unpacked[3]
                cr3 = unpacked[4]
                rip = unpacked[5]
                gpa = unpacked[6]
                data_size = unpacked[7]
                lbr_data = unpacked[9:]
                
                if magic != 0x5356545200000000:
                    print(f"[!] HATA: Ring Buffer senkronizasyon kaybı: 0x{magic:016x}")
                    continue
                
                if ev_type == 1: # TRACE_EVT_LBR_SAMPLE
                    for i in range(lbr_count):
                        frm = lbr_data[i*2]
                        to = lbr_data[i*2 + 1]
                        if frm != 0 and to != 0:
                            recent_branches.append((frm, to))
                            if not args.quiet:
                                print(f"[LBR] TSC: {tsc} | 0x{frm:016x} -> 0x{to:016x}")
                                
                elif ev_type == 2: # TRACE_EVT_NPF_DIRTY
                    payload = read_exactly(f, data_size)
                    ent = calculate_entropy(payload)
                    
                    print(f"\n[DIRTY MUTATION] TSC: {tsc} | CR3: 0x{cr3:016x} | Fault_RIP: 0x{rip:016x} | GPA: 0x{gpa:016x}")
                    
                    if ent > 7.5:
                        print(f"      [!!!] DİKKAT: Yüksek Entropi Tespit Edildi (Entropy: {ent:.2f})")
                        print(f"      [!!!] Bu sayfa şifrelenmiş payload, AES/RSA Anahtarı veya Packed Code olabilir!")
                    else:
                        print(f"      [*] Normal Entropi (Entropy: {ent:.2f}) - Standart Veri/Kod")
                        
                    print("      [*] Mutasyona Yol Açan Son LBR Dallanmaları (Chronological):")
                    if not recent_branches:
                        print("          (Geçmiş dal yok)")
                    else:
                        for b_idx, (b_from, b_to) in enumerate(recent_branches):
                            print(f"          ├─ [-{len(recent_branches)-b_idx}] 0x{b_from:016x} -> 0x{b_to:016x}")
                    
                    if args.out_dir:
                        out_path = os.path.join(args.out_dir, f"dirty_0x{gpa:x}_{tsc}.bin")
                        with open(out_path, "wb") as outf:
                            outf.write(payload)
                        print(f"      └─ Kaydedildi: {out_path}\n")
                        
    except KeyboardInterrupt:
        print("\n[*] Trace Engine başarıyla durduruldu.")

def draw_dashboard(stats, branches, mutations):
    sys.stdout.write("\033[?25l") # Hide cursor
    sys.stdout.write("\033[H")    # Move to top-left
    
    lines = []
    lines.append("\033[96m╔════════════════════════════════════════════════════════════════════════════════╗\033[0m")
    lines.append("\033[96m║                SVM RING -1 LIVE MALWARE TRACE DASHBOARD (v6.0)                 ║\033[0m")
    lines.append("\033[96m╚════════════════════════════════════════════════════════════════════════════════╝\033[0m")
    lines.append(f" STATUS: Monitoring /proc/svm_trace    | LBRs: {stats['lbr']} | Mutations: {stats['dirty']} | High-Ent: {stats['high_ent']}")
    lines.append("\n\033[93m================ LBR EXECUTION FLOW (Live Last 15 Branches) ================\033[0m")
    
    br_list = list(branches)
    for i in range(15):
        if i < len(br_list):
            lines.append(f"  ├─ 0x{br_list[i][0]:016x} -> 0x{br_list[i][1]:016x}")
        else:
            lines.append("  |")
            
    lines.append("\n\033[93m================ RECENT MUTATIONS & ENTROPY ANALYSIS =======================\033[0m")
    mut_list = list(mutations)
    for i in range(10):
        if i < len(mut_list):
            m = mut_list[i]
            if m['ent'] > 7.5:
                prefix = "[!!!] HIGH ENTROPY"
                color = "\033[91m" # Red
            else:
                prefix = "[*] NORMAL PAGE   "
                color = "\033[92m" # Green
            reset = "\033[0m"
            
            lines.append(f" {color}{prefix}{reset} - GPA: 0x{m['gpa']:016x} | RIP: 0x{m['rip']:016x} | Ent: {m['ent']:05.2f}")
        else:
            lines.append(" " * 80)
            
    lines.append("\n [ Yüksek entropili (şifreli/paketlenmiş) sayfalar out-dir'e kaydediliyor ]   ")
    lines.append(" [ Çıkmak için Ctrl+C tuşuna basın.                                         ]   ")
    
    for _ in range(3):
        lines.append(" " * 80)
        
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()

def cmd_live(args):
    """Real-time Advanced Dashboard Mode for LBR-Memory Correlation"""
    trace_file = "/proc/svm_trace"
    if not os.path.exists(trace_file):
        print(f"[!] HATA: {trace_file} bulunamadı!")
        sys.exit(1)
        
    if args.out_dir and not os.path.exists(args.out_dir):
        os.makedirs(args.out_dir)
        
    ENTRY_FMT = "<QQIIQQQII32Q"
    ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
    
    recent_branches = deque(maxlen=15)
    recent_mutations = deque(maxlen=10)
    stats = {"lbr": 0, "dirty": 0, "high_ent": 0}
    session_log = []
    
    sys.stdout.write("\033[2J") # Clear entire screen once
    last_draw = 0
    
    try:
        with open(trace_file, "rb") as f:
            while True:
                header_data = read_exactly(f, ENTRY_SIZE)
                unpacked = struct.unpack(ENTRY_FMT, header_data)
                magic, tsc, ev_type, lbr_count, cr3, rip, gpa, data_size, _pad = unpacked[:9]
                lbr_data = unpacked[9:]
                
                if magic != 0x5356545200000000:
                    continue
                
                dirty_page_payload = None
                if ev_type == 2:
                    dirty_page_payload = read_exactly(f, data_size)
                    
                if ev_type == 1:
                    stats["lbr"] += lbr_count
                    for i in range(lbr_count):
                        frm = lbr_data[i*2]
                        to = lbr_data[i*2 + 1]
                        if frm != 0 and to != 0:
                            recent_branches.append((frm, to))
                            if args.log:
                                session_log.append(f"[LBR] TSC: {tsc} | 0x{frm:016x} -> 0x{to:016x}")
                            
                elif ev_type == 2:
                    stats["dirty"] += 1
                    ent = calculate_entropy(dirty_page_payload)
                    if ent > 7.5:
                        stats["high_ent"] += 1
                        if args.out_dir:
                            out_path = os.path.join(args.out_dir, f"live_0x{gpa:x}_{tsc}.bin")
                            with open(out_path, "wb") as outf:
                                outf.write(dirty_page_payload)
                                
                    recent_mutations.appendleft({
                        'gpa': gpa, 'rip': rip, 'ent': ent, 'tsc': tsc
                    })
                    
                    if args.log:
                        session_log.append(f"\n[MUTATION] TSC: {tsc} | CR3: 0x{cr3:016x} | Fault RIP: 0x{rip:016x} | GPA: 0x{gpa:016x} | Ent: {ent:.2f}")
                        if ent > 7.5:
                            session_log.append(f"  └─ [!!!] Yüksek Entropi Tespit Edildi (Şifreli Kod/Anahtar Olabilir)")
                
                # 30 FPS redraw limit
                now = time.time()
                if now - last_draw > 0.033:
                    draw_dashboard(stats, recent_branches, recent_mutations)
                    last_draw = now
                    
    except KeyboardInterrupt:
        sys.stdout.write("\033[?25h\n") # Show cursor
        sys.stdout.flush()
        print("\n[*] Live Dashboard başarıyla kapatıldı.")
        if args.log and session_log:
            print(f"[*] Bekleyin, tam kronolojik rapor {args.log} dosyasına yazılıyor...")
            with open(args.log, "w", encoding="utf-8") as lf:
                lf.write("\n".join(session_log))
            print(f"[+] Toplam {len(session_log)} kayıt {args.log} dosyasına başarıyla kaydedildi!")

def check_env():
    if os.geteuid() != 0:
        print("[!] Lütfen root yetkileriyle çalıştırın (sudo).")
        sys.exit(1)
    if not os.path.exists(PROC_DIR):
        print(f"[!] HATA: Kernel modülü yüklü değil! {PROC_DIR} bulunamadı.")
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="SVM CLI - Ring -1 RAM Snapshot & Trace Aracı", epilog="Mükemmel Gnu-Tarzı Gizli Hipervizör Yönetimi")
    subparsers = parser.add_subparsers(dest="command", required=True)
    
    p_dump = subparsers.add_parser("dump", help="Aktif PID'ye göre RAM snapshot al (Phase 0)")
    p_dump.add_argument("--pid", type=int, required=True, help="Hedefin PID'si")
    p_dump.add_argument("--out", "-o", type=str, required=True, help="Çıktı .bin dosyasının yolu")
    
    p_watch = subparsers.add_parser("watch", help="Uygulama açılana kadar bekle ve ilk baytta dondur (Phase 0)")
    p_watch.add_argument("--name", type=str, required=True, help="Programın tam adı (Örn: game.exe)")
    p_watch.add_argument("--out", "-o", type=str, required=True, help="Çıktı .bin dosyasının yolu")
    
    p_list = subparsers.add_parser("list", help="Mümkün süreçleri (process) listele")
    
    p_trace = subparsers.add_parser("trace", help="V6.0 Continuous Malware Trace (Linear Log)")
    p_trace.add_argument("--out-dir", type=str, help="Mutasyona uğramış fiziksel belleğin kaydedileceği klasör")
    p_trace.add_argument("--quiet", "-q", action="store_true", help="Standart LBR loglarını gizle, sadece mutasyonları göster")
    
    p_live = subparsers.add_parser("live", help="[GELİŞMİŞ] Sonsuz Döngü Dashboard - Canlı Analiz Ekranı")
    p_live.add_argument("--out-dir", type=str, help="Yüksek entropili sayfaların kaydedileceği klasör")
    p_live.add_argument("--log", "-l", type=str, default="live_trace_report.txt", help="Çıkışta (Ctrl+C) tüm işlem geçmişinin kaydedileceği TXT dosyası (ön tanımlı: live_trace_report.txt)")
    
    args = parser.parse_args()
    check_env()
    
    if args.command == "dump": cmd_dump(args)
    elif args.command == "watch": cmd_watch(args)
    elif args.command == "list": cmd_list(args)
    elif args.command == "trace": cmd_trace(args)
    elif args.command == "live": cmd_live(args)

if __name__ == "__main__":
    main()
