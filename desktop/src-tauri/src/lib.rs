// Ember Desktop · Tauri 宿主
//
// 这里只做三件事：开一个透明无边框的窗口、给窗口铺系统材质（Mica/Acrylic）、
// 让前端通过 shell 插件拉起自带的 agent sidecar。业务逻辑全在 C++ 与 React 里。
use tauri::utils::config::WindowEffectsConfig;
use tauri::utils::WindowEffect;
use tauri::window::Color;
use tauri::{Manager, WebviewWindow};

/// 真实的 Windows build 号。
///
/// 为什么不直接 try-mica-then-acrylic：Win10 上 `set_effects(Mica)` 会返回 Ok 但
/// 什么也不做（静默失败），回退逻辑根本不会触发。GetVersionEx 在没声明 manifest
/// 时同样会撒谎（永远 6.2），所以直接问 ntdll 的 RtlGetVersion。
#[cfg(windows)]
fn windows_build() -> u32 {
    #[repr(C)]
    struct OsVersionInfoW {
        size: u32,
        major: u32,
        minor: u32,
        build: u32,
        platform: u32,
        csd: [u16; 128],
    }

    #[link(name = "ntdll")]
    extern "system" {
        fn RtlGetVersion(info: *mut OsVersionInfoW) -> i32;
    }

    let mut info = OsVersionInfoW {
        size: std::mem::size_of::<OsVersionInfoW>() as u32,
        major: 0,
        minor: 0,
        build: 0,
        platform: 0,
        csd: [0; 128],
    };
    match unsafe { RtlGetVersion(&mut info) } {
        0 => info.build,
        _ => 0,
    }
}

#[cfg(not(windows))]
fn windows_build() -> u32 {
    0
}

/// 窗口材质：只在 Win11（build 22000+）上给 Mica。
///
/// Win10 上不给材质是有意的：Acrylic 在 Win10 v1903+ 拖动/缩放窗口时掉帧严重
/// （tauri-utils 自己的文档就写着 "bad performance when resizing/dragging on
/// Windows 10 v1903+"），而本设计的材质只在窗口四周 8px 外圈可见，代价远大于收益。
/// Win10 就让窗口纯透明：外圈透出真实桌面，拖动跟手。
fn apply_backdrop(window: &WebviewWindow) {
    // WebView2 自带不透明底色，得先让出 alpha=0，桌面才透得上来
    // （Windows 上非 0 的 alpha 会被强制成 255）
    let _ = window.set_background_color(Some(Color(0, 0, 0, 0)));

    let build = windows_build();
    if build < 22000 {
        log::info!("window backdrop: none (win10 build {build}: acrylic stutters when dragging)");
        return;
    }
    match window.set_effects(Some(WindowEffectsConfig {
        effects: vec![WindowEffect::Mica],
        ..Default::default()
    })) {
        Ok(()) => log::info!("window backdrop: mica on build {build}"),
        Err(err) => log::warn!("window backdrop: mica failed on build {build}: {err}"),
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .setup(|app| {
            // 日志插件先起，材质那段才写得出日志
            if cfg!(debug_assertions) {
                app.handle().plugin(
                    tauri_plugin_log::Builder::default()
                        .level(log::LevelFilter::Info)
                        .build(),
                )?;
            }
            if let Some(window) = app.get_webview_window("main") {
                apply_backdrop(&window);
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}