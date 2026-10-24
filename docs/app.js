// MiniWallpaper Documentation & Landing Page Scripts
// Theme: Cyber Dark (Clean vector icons, Desktop app palette)

const translations = {
  id: {
    nav_features: "Fitur",
    nav_benchmarks: "Benchmark",
    nav_install: "Cara Pasang",
    nav_troubleshoot: "Troubleshooting",
    hero_badge: "Native C++20 & Direct3D 11 Engine",
    hero_title: "Wallpaper Video Hidup Tanpa Beban Resource",
    hero_desc: "Engine wallpaper video teringan di Windows. Memanfaatkan decoding GPU ASIC murni (D3D11VA), manajemen memori mimalloc zero-purge, dan penundaan DLL startup. Bebas Chromium, hemat baterai, dan nol kompromi untuk performa gaming.",
    btn_download: "Unduh MiniWallpaper v2.0.0",
    btn_github: "Lihat di GitHub",
    btn_guide: "Panduan Penggunaan",
    metric_ram: "RAM Task Manager",
    metric_cpu: "CPU Idle",
    metric_vram: "VRAM Dedicated",
    metric_runtime: "Zero Chromium / WPF",
    
    sec_features_badge: "Arsitektur & Inovasi",
    sec_features_title: "Fitur Unggulan MiniWallpaper",
    sec_features_subtitle: "Didesain dari baris pertama untuk efisiensi ekstrim tanpa membebani sistem saat bekerja maupun bermain game.",
    
    feat_hw_title: "Zero-Copy D3D11VA Decode",
    feat_hw_desc: "Stream video didekode langsung di chip hardware GPU (NVDEC / Intel QSV / AMD VCN) via D3D11 Texture2DArray SRV tanpa penyalinan CPU-ke-GPU.",
    
    feat_gpu_title: "Smart Dual-GPU Routing",
    feat_gpu_desc: "Secara otomatis memprioritaskan Integrated GPU (iGPU) hemat daya untuk rendering, membiarkan discrete GPU (NVIDIA/AMD) tetap berada pada status deep sleep 0%.",
    
    feat_gov_title: "Gaming Auto-Sleep & Governor",
    feat_gov_desc: "Otomatis pause instan saat game fullscreen aktif. Mengaktifkan Deep Sleep (0% CPU, 0 MB VRAM) saat RAM atau VRAM sistem mencapai ambang batas (>= 80%).",
    
    feat_boot_title: "Adaptive Boot Readiness Gate",
    feat_boot_desc: "Startup Windows tetap cepat berkat 9 DLL berat (FFmpeg & D3D11) yang berstatus delay-load. Wallpaper statis tampil detik ke-0, engine aktif saat CPU tenang.",
    
    feat_lock_title: "Sinkronisasi Layar Kunci",
    feat_lock_desc: "Ekstrak snapshot frame video secara presisi dan sinkronkan ke Lock Screen Windows serta companion screensaver native MiniWallpaper.scr.",
    
    feat_opt_title: "1080p Video Auto-Optimizer",
    feat_opt_desc: "Pre-scaler bawaan yang mengonversi video 4K/bitrate tinggi menjadi format hardware 1080p NV12 yang menghemat daya hingga 75%.",
    
    sec_bench_badge: "Pengujian Nyata",
    sec_bench_title: "Benchmark Windows Task Manager",
    sec_bench_subtitle: "Data pengujian resmi langsung dari Task Manager (Working Set - Private) pada video 1080p 60 FPS di Windows 11.",
    
    tb_metric: "Metrik Operasional",
    tb_miniwp: "MiniWallpaper v2.0.0",
    tb_other1: "Aplikasi Sejenis A (WPF / .NET)",
    tb_other2: "Aplikasi Sejenis B (CEF / Web)",
    tb_status: "Status Pengujian",
    
    tb_ram_bg: "RAM (Background Daemon)",
    tb_ram_bg_mini: "0.36 MB",
    tb_ram_bg_other1: "65.0 - 120.0 MB",
    tb_ram_bg_other2: "50.0 - 95.0 MB",
    tb_ram_bg_stat: "Hemat hingga 99% memori",
    
    tb_ram_boot: "RAM saat Windows Logon",
    tb_ram_boot_mini: "0.21 MB (38 modul)",
    tb_ram_boot_other1: "80.0 - 150.0 MB",
    tb_ram_boot_other2: "60.0 - 110.0 MB",
    tb_ram_boot_stat: "0.00 MB Disk I/O saat boot",
    
    tb_cpu: "Beban CPU Rata-Rata",
    tb_cpu_mini: "0.00% - 0.20%",
    tb_cpu_other1: "1.20% - 4.50%",
    tb_cpu_other2: "0.80% - 2.50%",
    tb_cpu_stat: "SetWinEventHook tanpa polling",
    
    tb_threads: "Jumlah Thread Aktif",
    tb_threads_mini: "23 threads",
    tb_threads_other1: "48 - 72 threads",
    tb_threads_other2: "35 - 55 threads",
    tb_threads_stat: "Arsitektur multi-thread efisien",
    
    tb_stack: "Teknologi Runtime",
    tb_stack_mini: "Native C++20 / Direct3D 11",
    tb_stack_other1: ".NET 8 / WPF / Chromium",
    tb_stack_other2: "Custom Engine / CEF Web",
    tb_stack_stat: "Zero runtime dependencies",
    
    sec_install_badge: "Instalasi Cepat",
    sec_install_title: "Cara Pasang & Konfigurasi",
    sec_install_subtitle: "Tidak membutuhkan instalasi rumit. Sepenuhnya portabel dan siap dijalankan dalam hitungan detik.",
    
    tab_portable: "1. Portable Quickstart",
    tab_autostart: "2. Menjalankan Otomatis",
    tab_screensaver: "3. Mode Screensaver",
    
    inst_step1_title: "Unduh & Ekstrak Berkas",
    inst_step1_desc: "Unduh arsip portable terbaru (MiniWallpaper-Portable-...-win64.zip) dari halaman GitHub Releases. Ekstrak seluruh isi file ZIP ke folder permanen pilihan Anda (contoh: D:\\MiniWallpaper\\).",
    inst_step1_note: "Penting: Jangan memindahkan file MiniWallpaper.exe sendirian keluar dari foldernya tanpa file DLL pendamping.",
    
    inst_step2_title: "Jalankan MiniWallpaper.exe",
    inst_step2_desc: "Klik dua kali MiniWallpaper.exe. Ikon aplikasi akan langsung aktif di System Tray taskbar (samping jam).",
    inst_step2_note: "Bila muncul dialog Windows SmartScreen (Windows protected your PC), klik 'More info' lalu pilih 'Run anyway' karena binary open-source belum menggunakan sertifikat berbayar.",
    
    inst_step3_title: "Tambahkan Video Favorit Anda",
    inst_step3_desc: "Klik kanan ikon tray -> Control Panel, atau buka jendela aplikasi. Anda cukup drag & drop berkas video (MP4, WebM, MKV, MOV) ke galeri untuk langsung menjadikannya wallpaper hidup.",
    
    inst_auto_title: "Konfigurasi Jalankan saat Windows Menyala",
    inst_auto_desc: "Buka Control Panel -> Tab General (Pengaturan Umum), lalu aktifkan switch 'Jalankan Otomatis saat Startup'. MiniWallpaper secara otomatis mendaftarkan autostart native Explorer tanpa menulis override registri yang mengganggu kestabilan sistem.",
    
    inst_scr_title: "Pasang Screensaver Bawaan (MiniWallpaper.scr)",
    inst_scr_desc: "Di dalam folder aplikasi terdapat file bernama MiniWallpaper.scr. Klik kanan berkas tersebut lalu pilih 'Install'. Dialog pengaturan Screensaver Windows akan terbuka dan otomatis memilih MiniWallpaper sebagai screensaver utama.",
    
    sec_trouble_badge: "Bantuan Teknis",
    sec_trouble_title: "Troubleshooting & Masalah Umum",
    sec_trouble_subtitle: "Solusi cepat untuk kendala umum yang sering ditemui pengguna.",
    
    faq_q1: "Layar hitam atau video wallpaper tidak muncul di desktop?",
    faq_a1: "Pastikan opsi 'Show desktop icons' dalam keadaan aktif (klik kanan desktop -> View -> centang 'Show desktop icons'). Windows WorkerW membutuhkan ikon desktop aktif untuk menempatkan jendela wallpaper di belakangnya. Pastikan juga file video tidak rusak dan menggunakan format standar (H.264/HEVC/AV1/VP9).",
    
    faq_q2: "Peringatan 'Windows Protected Your PC' (SmartScreen) muncul saat dijalankan?",
    faq_a2: "Hal ini lumrah terjadi pada software open-source baru yang didistribusikan gratis tanpa sertifikat EV berbayar Microsoft. Klik 'More info' lalu klik 'Run anyway'. Seluruh kode sumber MiniWallpaper 100% terbuka dan dapat diaudit secara publik di GitHub.",
    
    faq_q3: "Video terasa berat, patah-patah, atau membebani kartu grafis?",
    faq_a3: "Gunakan fitur bawaan 'Optimize Video (4K -> 1080p)' pada tab Wallpaper Gallery. Video 4K mendekode piksel 4x lipat lebih banyak dibandingkan 1080p. Dengan optimasi hardware NV12 bawaan, beban GPU akan turun drastis hingga 75%.",
    
    faq_q4: "Wallpaper otomatis berhenti saat bermain game atau menjalankan aplikasi berat?",
    faq_a4: "Ini adalah fitur proteksi sistem bernama 'Auto-Pause on Fullscreen' dan 'Gaming Auto-Sleep'. MiniWallpaper secara cerdas menghentikan dekoder dan membebaskan VRAM ke 0 MB saat game aktif agar FPS game Anda tetap 100% maksimal. Begitu game ditutup, wallpaper akan otomatis resume kembali.",
    
    faq_q5: "Suara video wallpaper tidak terdengar sama sekali?",
    faq_a5: "Secara default wallpaper dimulai dalam kondisi bisu (muted) demi kenyamanan. Buka Control Panel -> Tab General, aktifkan tombol Audio dan sesuaikan volume sesuai keinginan Anda.",
    
    faq_q6: "Wallpaper hilang atau tidak menyesuaikan saat menyambung monitor eksternal?",
    faq_a6: "MiniWallpaper memantau event sistem WM_DISPLAYCHANGE secara real-time. Jika Anda baru saja mencolok monitor baru atau mengubah orientasi, klik tombol Refresh di rail menu galeri atau pilih monitor target pada banner multi-monitor di galeri.",
    
    faq_q7: "Bagaimana cara mereset seluruh konfigurasi ke kondisi awal?",
    faq_a7: "Buka Windows Run (Win + R), ketik %APPDATA%\\MiniWallpaper lalu tekan Enter. Hapus berkas config.json kemudian jalankan ulang MiniWallpaper.exe. Aplikasi akan membuat konfigurasi default baru yang bersih.",
    
    footer_text: "MiniWallpaper v2.0.0 — Dikembangkan secara independen berlisensi GPLv3. Sepenuhnya bebas iklan, pelacak, dan telemetri tersembunyi."
  },
  en: {
    nav_features: "Features",
    nav_benchmarks: "Benchmarks",
    nav_install: "Installation",
    nav_troubleshoot: "Troubleshooting",
    hero_badge: "Native C++20 & Direct3D 11 Engine",
    hero_title: "Live Video Wallpapers with Zero Resource Impact",
    hero_desc: "The ultra-lightweight animated wallpaper engine for Windows. Powered by pure GPU hardware decoding (D3D11VA), mimalloc zero-purge memory tuning, and delay-loaded startup modules. Zero Chromium, battery friendly, and zero gaming performance compromise.",
    btn_download: "Download MiniWallpaper v2.0.0",
    btn_github: "View on GitHub",
    btn_guide: "User Guide",
    metric_ram: "Task Manager RAM",
    metric_cpu: "Idle CPU",
    metric_vram: "Dedicated VRAM",
    metric_runtime: "Zero Chromium / WPF",
    
    sec_features_badge: "Architecture & Innovation",
    sec_features_title: "Key Features of MiniWallpaper",
    sec_features_subtitle: "Engineered from line one for extreme resource efficiency without hindering productivity or gaming frame rates.",
    
    feat_hw_title: "Zero-Copy D3D11VA Decode",
    feat_hw_desc: "Video streams are decoded directly on GPU ASIC hardware (NVDEC / Intel QSV / AMD VCN) into D3D11 Texture2DArray SRVs with zero CPU-to-GPU memory copies.",
    
    feat_gpu_title: "Smart Dual-GPU Routing",
    feat_gpu_desc: "Automatically prioritizes the low-power Integrated GPU (iGPU) for rendering, allowing discrete high-power gaming GPUs (NVIDIA/AMD) to stay in 0% deep sleep.",
    
    feat_gov_title: "Gaming Auto-Sleep & Governor",
    feat_gov_desc: "Instantly auto-pauses when a fullscreen game is running. Enters Deep Sleep (0% CPU, 0 MB VRAM) when overall system RAM or VRAM reaches threshold (>= 80%).",
    
    feat_boot_title: "Adaptive Boot Readiness Gate",
    feat_boot_desc: "Windows boot remains lightning-fast with 9 heavy DLLs delay-loaded. The native static wallpaper displays at t=0s, and the video engine starts smoothly once CPU settles.",
    
    feat_lock_title: "Lock Screen & Screensaver Sync",
    feat_lock_desc: "Extract precise video snapshot frames and synchronize them effortlessly to Windows Lock Screen and the companion native screensaver MiniWallpaper.scr.",
    
    feat_opt_title: "1080p Video Auto-Optimizer",
    feat_opt_desc: "Built-in GPU pre-scaler that converts heavy 4K/high-bitrate videos into lightweight 1080p NV12 hardware streams, saving up to 75% GPU power.",
    
    sec_bench_badge: "Real World Telemetry",
    sec_bench_title: "Windows Task Manager Benchmarks",
    sec_bench_subtitle: "Direct live Task Manager measurement (Working Set - Private) running a 1080p 60 FPS video on Windows 11.",
    
    tb_metric: "Operational Metric",
    tb_miniwp: "MiniWallpaper v2.0.0",
    tb_other1: "Similar App A (WPF / .NET)",
    tb_other2: "Similar App B (CEF / Web)",
    tb_status: "Test Verdict",
    
    tb_ram_bg: "RAM (Background Daemon)",
    tb_ram_bg_mini: "0.36 MB",
    tb_ram_bg_other1: "65.0 - 120.0 MB",
    tb_ram_bg_other2: "50.0 - 95.0 MB",
    tb_ram_bg_stat: "Up to 99% less memory footprint",
    
    tb_ram_boot: "RAM during Windows Logon",
    tb_ram_boot_mini: "0.21 MB (38 modules)",
    tb_ram_boot_other1: "80.0 - 150.0 MB",
    tb_ram_boot_other2: "60.0 - 110.0 MB",
    tb_ram_boot_stat: "0.00 MB Disk I/O during logon",
    
    tb_cpu: "Average CPU Load",
    tb_cpu_mini: "0.00% - 0.20%",
    tb_cpu_other1: "1.20% - 4.50%",
    tb_cpu_other2: "0.80% - 2.50%",
    tb_cpu_stat: "SetWinEventHook without polling",
    
    tb_threads: "Active Thread Count",
    tb_threads_mini: "23 threads",
    tb_threads_other1: "48 - 72 threads",
    tb_threads_other2: "35 - 55 threads",
    tb_threads_stat: "Highly compact thread model",
    
    tb_stack: "Underlying Runtime",
    tb_stack_mini: "Native C++20 / Direct3D 11",
    tb_stack_other1: ".NET 8 / WPF / Chromium",
    tb_stack_other2: "Custom Engine / CEF Web",
    tb_stack_stat: "Zero runtime dependencies",
    
    sec_install_badge: "Quick Setup",
    sec_install_title: "Installation & Configuration",
    sec_install_subtitle: "No complex installer required. Completely portable and ready to run in seconds.",
    
    tab_portable: "1. Portable Quickstart",
    tab_autostart: "2. Windows Autostart",
    tab_screensaver: "3. Screensaver Mode",
    
    inst_step1_title: "Download & Extract Archive",
    inst_step1_desc: "Download the latest portable zip (MiniWallpaper-Portable-...-win64.zip) from the GitHub Releases page. Extract the entire ZIP content to a permanent folder of your choice (e.g., D:\\MiniWallpaper\\).",
    inst_step1_note: "Important: Do not move MiniWallpaper.exe out on its own without its companion DLL files and assets.",
    
    inst_step2_title: "Launch MiniWallpaper.exe",
    inst_step2_desc: "Double-click MiniWallpaper.exe. The application icon will appear in the System Tray next to the clock.",
    inst_step2_note: "If Windows SmartScreen appears ('Windows protected your PC'), click 'More info' and select 'Run anyway'. Open-source binaries do not include expensive commercial signing certificates.",
    
    inst_step3_title: "Add Your Favorite Videos",
    inst_step3_desc: "Right-click the tray icon -> Control Panel. Simply drag and drop video files (MP4, WebM, MKV, MOV) into the gallery to instantly play them as desktop wallpapers.",
    
    inst_auto_title: "Configure Windows Autostart",
    inst_auto_desc: "Open Control Panel -> General tab, and turn on 'Run on Windows Startup'. MiniWallpaper registers a clean Explorer autostart entry without polluting system registries.",
    
    inst_scr_title: "Install Screensaver (MiniWallpaper.scr)",
    inst_scr_desc: "Inside the application folder, locate MiniWallpaper.scr. Right-click it and select 'Install'. The Windows Screensaver settings dialog will open with MiniWallpaper selected.",
    
    sec_trouble_badge: "Help & Diagnostics",
    sec_trouble_title: "Troubleshooting & Common Questions",
    sec_trouble_subtitle: "Clear solutions for typical setup questions.",
    
    faq_q1: "Black screen or video does not appear on desktop?",
    faq_a1: "Ensure 'Show desktop icons' is checked (right-click desktop -> View -> check 'Show desktop icons'). Windows WorkerW requires desktop icons active to host the wallpaper child window behind them. Also verify your video file is not corrupted and uses standard codecs (H.264/HEVC/AV1/VP9).",
    
    faq_q2: "Windows SmartScreen ('Windows Protected Your PC') dialog shows up?",
    faq_a2: "This is completely normal for newly released open-source software distributed freely without expensive corporate certificates. Click 'More info' and click 'Run anyway'. The entire source code is 100% open and auditable on GitHub.",
    
    faq_q3: "Video stutters, drops frames, or GPU load feels high?",
    faq_a3: "Use the built-in 'Optimize Video (4K -> 1080p)' tool in the Wallpaper Gallery tab. 4K videos decode 4x more pixels than 1080p. Converting to hardware NV12 1080p will drop GPU load by up to 75%.",
    
    faq_q4: "Wallpaper stops playing while gaming or running heavy apps?",
    faq_a4: "This is an intended performance protection feature ('Auto-Pause on Fullscreen' and 'Gaming Auto-Sleep'). MiniWallpaper pauses decoding and clears VRAM to 0 MB so your game gets 100% GPU power. Playback resumes automatically once you exit the game.",
    
    faq_q5: "Video wallpaper audio is not playing?",
    faq_a5: "By default, wallpapers start muted for convenience. Open Control Panel -> General tab, toggle the Audio switch and adjust the volume slider to your desired level.",
    
    faq_q6: "Wallpaper resolution mismatch after connecting external monitors?",
    faq_a6: "MiniWallpaper listens to WM_DISPLAYCHANGE events dynamically. If you plugged in a monitor or changed resolution, click the Refresh icon in the gallery left rail or select target displays from the banner.",
    
    faq_q7: "How to reset all configuration back to defaults?",
    faq_a7: "Press Win + R, type %APPDATA%\\MiniWallpaper, and press Enter. Delete config.json and restart MiniWallpaper.exe. A fresh default configuration will be generated automatically.",
    
    footer_text: "MiniWallpaper v2.0.0 — Open source software licensed under GPLv3. Completely free of advertisements, telemetry, and background tracking."
  }
};

let currentLang = "id";

function setLanguage(lang) {
  if (!translations[lang]) return;
  currentLang = lang;
  localStorage.setItem("miniwp_lang", lang);
  
  const dict = translations[lang];
  document.querySelectorAll("[data-i18n]").forEach((el) => {
    const key = el.getAttribute("data-i18n");
    if (dict[key]) {
      el.textContent = dict[key];
    }
  });

  document.querySelectorAll(".lang-switch-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.getAttribute("data-lang") === lang);
  });
}

// Accordion Logic
function initAccordion() {
  document.querySelectorAll(".faq-header").forEach((header) => {
    header.addEventListener("click", () => {
      const item = header.parentElement;
      const isOpen = item.classList.contains("open");
      
      // Close all other items
      document.querySelectorAll(".faq-item").forEach((el) => el.classList.remove("open"));
      
      // Toggle current
      if (!isOpen) {
        item.classList.add("open");
      }
    });
  });
}

// Tab Switcher for Installation
function initTabs() {
  const tabButtons = document.querySelectorAll(".tab-btn");
  const tabContents = document.querySelectorAll(".tab-content-panel");
  
  tabButtons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const target = btn.getAttribute("data-tab");
      
      tabButtons.forEach((b) => b.classList.remove("active"));
      tabContents.forEach((c) => c.classList.add("hidden"));
      
      btn.classList.add("active");
      const targetPanel = document.getElementById(target);
      if (targetPanel) {
        targetPanel.classList.remove("hidden");
      }
    });
  });
}

// Copy Code Button
function initCopyButtons() {
  document.querySelectorAll(".copy-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const textToCopy = btn.getAttribute("data-copy");
      if (textToCopy) {
        navigator.clipboard.writeText(textToCopy).then(() => {
          const originalText = btn.textContent;
          btn.textContent = currentLang === "id" ? "Disalin!" : "Copied!";
          btn.style.color = "#34d399";
          setTimeout(() => {
            btn.textContent = originalText;
            btn.style.color = "";
          }, 2000);
        });
      }
    });
  });
}

// Mobile Menu Toggle
function initMobileMenu() {
  const toggleBtn = document.getElementById("mobile-menu-toggle");
  const mobileMenu = document.getElementById("mobile-menu");
  if (toggleBtn && mobileMenu) {
    toggleBtn.addEventListener("click", () => {
      mobileMenu.classList.toggle("hidden");
    });
    mobileMenu.querySelectorAll("a").forEach((link) => {
      link.addEventListener("click", () => {
        mobileMenu.classList.add("hidden");
      });
    });
  }
}

// Initialize on DOM Ready
document.addEventListener("DOMContentLoaded", () => {
  initAccordion();
  initTabs();
  initCopyButtons();
  initMobileMenu();

  // Load language preference
  const savedLang = localStorage.getItem("miniwp_lang") || "id";
  setLanguage(savedLang);

  document.querySelectorAll(".lang-switch-btn").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.preventDefault();
      const lang = btn.getAttribute("data-lang");
      setLanguage(lang);
    });
  });
});
