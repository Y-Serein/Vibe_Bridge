use anyhow::Result;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream, UnixDatagram};
use tokio::sync::mpsc;
use tokio::time::{self, Duration};
use tracing::{info, warn};

use crate::claude::formatter::format_sessions_for_screen;
use crate::hid::device::{spawn_read_thread, VibeDevice};
use crate::hid::reports::AckReport;
use crate::hid::screen::ScreenController;
use crate::session::{detect_proc_sessions, SessionEvent, SessionRegistry};

pub const DEFAULT_SOCKET_PATH: &str = "/tmp/vibe-bridge.sock";

pub async fn run_daemon(
    socket_path: &str,
    tcp_addr: Option<&str>,
    vid: Option<u16>,
    pid: Option<u16>,
    reliable: bool,
    poll_ms: u64,
) -> Result<()> {
    let device = open_device(vid, pid)?;
    let device = Arc::new(device);
    let (ack_tx, ack_rx) = mpsc::channel::<AckReport>(256);
    spawn_read_thread(Arc::clone(&device), ack_tx);
    let screen = ScreenController::new(device, ack_rx, reliable);

    if Path::new(socket_path).exists() {
        std::fs::remove_file(socket_path)?;
    }
    let socket = UnixDatagram::bind(socket_path)?;
    std::fs::set_permissions(socket_path, std::fs::Permissions::from_mode(0o666))?;
    info!("vibe-bridge daemon listening on {}", socket_path);

    let (tcp_tx, mut tcp_rx) = mpsc::channel::<SessionEvent>(128);
    let _tcp_tx_guard = tcp_tx.clone();
    if let Some(addr) = tcp_addr.filter(|addr| !addr.is_empty()) {
        spawn_tcp_listener(addr, tcp_tx).await?;
    }

    let mut registry = SessionRegistry::default();
    registry.replace_auto(detect_proc_sessions());
    let mut last_frame = String::new();
    render_if_changed(&screen, &registry, &mut last_frame).await?;
    let mut buf = vec![0u8; 8192];
    let mut ticker = time::interval(Duration::from_millis(poll_ms.max(100)));

    loop {
        tokio::select! {
            recv = socket.recv(&mut buf) => {
                match recv {
                    Ok(n) if n > 0 => {
                        match serde_json::from_slice::<SessionEvent>(&buf[..n]) {
                            Ok(event) => {
                                registry.apply_event(event);
                                render_if_changed(&screen, &registry, &mut last_frame).await?;
                            }
                            Err(err) => warn!("invalid daemon event: {}", err),
                        }
                    }
                    Ok(_) => {}
                    Err(err) => warn!("socket recv failed: {}", err),
                }
            }
            event = tcp_rx.recv() => {
                if let Some(event) = event {
                    registry.apply_event(event);
                    render_if_changed(&screen, &registry, &mut last_frame).await?;
                }
            }
            _ = ticker.tick() => {
                registry.replace_auto(detect_proc_sessions());
                render_if_changed(&screen, &registry, &mut last_frame).await?;
            }
        }
    }
}

pub fn emit_event(socket_path: &str, event: &SessionEvent) -> Result<()> {
    let socket = std::os::unix::net::UnixDatagram::unbound()?;
    let payload = serde_json::to_vec(event)?;
    socket.send_to(&payload, socket_path)?;
    Ok(())
}

pub async fn emit_tcp_event(addr: &str, event: &SessionEvent) -> Result<()> {
    let mut stream = TcpStream::connect(addr).await?;
    let mut payload = serde_json::to_vec(event)?;
    payload.push(b'\n');
    stream.write_all(&payload).await?;
    Ok(())
}

async fn spawn_tcp_listener(addr: &str, tx: mpsc::Sender<SessionEvent>) -> Result<()> {
    let listener = TcpListener::bind(addr).await?;
    info!("vibe-bridge daemon listening on tcp {}", addr);

    tokio::spawn(async move {
        loop {
            match listener.accept().await {
                Ok((stream, _peer)) => {
                    let tx = tx.clone();
                    tokio::spawn(async move {
                        if let Err(err) = read_tcp_event(stream, tx).await {
                            warn!("tcp event failed: {}", err);
                        }
                    });
                }
                Err(err) => warn!("tcp accept failed: {}", err),
            }
        }
    });

    Ok(())
}

async fn read_tcp_event(stream: TcpStream, tx: mpsc::Sender<SessionEvent>) -> Result<()> {
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    reader.read_line(&mut line).await?;
    if line.trim().is_empty() {
        return Ok(());
    }
    let event = serde_json::from_str::<SessionEvent>(line.trim())?;
    tx.send(event).await?;
    Ok(())
}

fn open_device(vid: Option<u16>, pid: Option<u16>) -> Result<VibeDevice> {
    if let (Some(v), Some(p)) = (vid, pid) {
        VibeDevice::open_by_vid_pid(v, p)
    } else {
        let list = VibeDevice::find_all()?;
        if list.is_empty() {
            return Err(anyhow::anyhow!(
                "No Vibe Keyboard found. Use --vid/--pid or check scan."
            ));
        }
        VibeDevice::open_by_path(list[0].path())
    }
}

async fn render_if_changed(
    screen: &ScreenController,
    registry: &SessionRegistry,
    last_frame: &mut String,
) -> Result<()> {
    let sessions = registry.visible_sessions();
    let frame = format_sessions_for_screen(&sessions);
    if &frame != last_frame {
        screen.write_text(&frame).await?;
        *last_frame = frame;
    }
    Ok(())
}
