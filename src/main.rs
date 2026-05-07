mod claude;
mod daemon;
mod hid;
mod session;

use anyhow::Result;
use clap::{Parser, Subcommand};
use std::sync::Arc;
use tokio::sync::mpsc;
use tracing::{debug, error};
use tracing_subscriber;

use crate::claude::formatter::format_for_screen;
use crate::claude::status::ClaudeStatus;
use crate::daemon::{emit_event, emit_tcp_event, run_daemon, DEFAULT_SOCKET_PATH};
use crate::hid::device::{spawn_read_thread, VibeDevice, HID_READ_TIMEOUT_MS};
use crate::hid::reports::{AckReport, InputReport, REPORT_ID_ACK, REPORT_ID_INPUT};
use crate::hid::screen::ScreenController;
use crate::session::{
    current_dir_string, detect_proc_sessions, find_git_branch, SessionEvent, SessionStatus,
};

#[derive(Parser)]
#[command(name = "vibe-bridge")]
#[command(about = "Vibe Bridge - Physical interface for Vibe Coding Keyboard", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Scan and list all connected Vibe keyboards
    Scan,
    /// Connect to a keyboard and listen for events
    Listen {
        #[arg(long)]
        vid: Option<u16>,
        #[arg(long)]
        pid: Option<u16>,
    },
    /// Test screen output
    TestScreen {
        #[arg(long)]
        text: String,
        #[arg(long)]
        vid: Option<u16>,
        #[arg(long)]
        pid: Option<u16>,
        /// Wait for ACK after every HID packet instead of fire-and-forget.
        #[arg(long)]
        reliable: bool,
    },
    /// Send detected Claude status to screen
    SendClaude {
        #[arg(long)]
        vid: Option<u16>,
        #[arg(long)]
        pid: Option<u16>,
        /// Use static mock data instead of scanning this WSL for Claude.
        #[arg(long)]
        mock: bool,
        /// Wait for ACK after every HID packet instead of fire-and-forget.
        #[arg(long)]
        reliable: bool,
    },
    /// Run the persistent HID/session bridge.
    Daemon {
        #[arg(long, default_value = DEFAULT_SOCKET_PATH)]
        socket: String,
        #[arg(long)]
        vid: Option<u16>,
        #[arg(long)]
        pid: Option<u16>,
        #[arg(long)]
        reliable: bool,
        /// Also listen for JSON events over TCP, useful for other WSL distros.
        #[arg(long)]
        tcp: Option<String>,
        #[arg(long, default_value_t = 500)]
        poll_ms: u64,
    },
    /// Send one session event to the daemon socket.
    Emit {
        #[arg(long, default_value = DEFAULT_SOCKET_PATH)]
        socket: String,
        #[arg(long)]
        tcp: Option<String>,
        #[arg(long)]
        session_id: Option<String>,
        #[arg(long, default_value = "hook")]
        source: String,
        #[arg(long, default_value = "session")]
        tool: String,
        #[arg(long, default_value = "active")]
        state: String,
        #[arg(long)]
        cwd: Option<String>,
        #[arg(long)]
        branch: Option<String>,
        #[arg(long)]
        model: Option<String>,
        #[arg(long)]
        message: Option<String>,
        #[arg(long)]
        tokens: Option<u64>,
        #[arg(long = "context")]
        context_pct: Option<u8>,
        #[arg(long)]
        cost: Option<String>,
        #[arg(long = "quota")]
        quota_remaining: Option<String>,
        #[arg(long = "quota-reset")]
        quota_reset: Option<String>,
        #[arg(long)]
        reply: Option<String>,
    },
    /// Print shell integration snippet for bash/zsh terminals.
    HookScript {
        #[arg(long, default_value = DEFAULT_SOCKET_PATH)]
        socket: String,
        #[arg(long)]
        tcp: Option<String>,
    },
    /// List sessions detected from the current WSL/Linux process namespace.
    Sessions,
    /// Forward sessions detected in this Linux namespace to a daemon elsewhere.
    ForwardSessions {
        #[arg(long, default_value = DEFAULT_SOCKET_PATH)]
        socket: String,
        #[arg(long)]
        tcp: Option<String>,
        #[arg(long, default_value_t = 500)]
        poll_ms: u64,
        #[arg(long)]
        once: bool,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt::init();

    let cli = Cli::parse();

    match cli.command {
        Commands::Scan => {
            let devices = VibeDevice::find_all()?;
            println!("Found {} Vibe Keyboard devices:", devices.len());
            for dev in devices {
                println!("- Path: {:?}", dev.path());
                println!(
                    "  VID: {:04X}, PID: {:04X}",
                    dev.vendor_id(),
                    dev.product_id()
                );
                println!("  Manufacturer: {:?}", dev.manufacturer_string());
                println!("  Product: {:?}", dev.product_string());
                println!(
                    "  Usage Page: {:04X}, Usage: {:04X}",
                    dev.usage_page(),
                    dev.usage()
                );
                println!("----------------------------------");
            }
        }
        Commands::Listen { vid, pid } => {
            let device = open_device(vid, pid)?;
            let device = Arc::new(device);

            println!("Connected to device. Listening for reports...");

            let dev_clone = Arc::clone(&device);
            tokio::task::spawn_blocking(move || {
                let mut buf = [0u8; 64];
                loop {
                    match dev_clone.read_timeout(&mut buf, HID_READ_TIMEOUT_MS) {
                        Ok(n) => {
                            if n > 0 {
                                if buf[0] == REPORT_ID_INPUT {
                                    if let Some(report) = InputReport::parse(&buf[..n]) {
                                        println!("Input: {:?}", report);
                                    }
                                } else if buf[0] == REPORT_ID_ACK {
                                    if let Some(ack) = AckReport::parse(&buf[..n]) {
                                        debug!(
                                            "ACK received: seq={}, status={:?}",
                                            ack.seq, ack.status
                                        );
                                    }
                                } else {
                                    println!("Raw Report (ID {:02X}): {:02X?}", buf[0], &buf[..n]);
                                }
                            }
                        }
                        Err(e) => {
                            error!("Read error: {}", e);
                            break;
                        }
                    }
                }
            })
            .await?;
        }
        Commands::TestScreen {
            text,
            vid,
            pid,
            reliable,
        } => {
            let device = open_device(vid, pid)?;
            let device = Arc::new(device);

            let (ack_tx, ack_rx) = mpsc::channel::<AckReport>(256);
            spawn_read_thread(Arc::clone(&device), ack_tx);

            let screen = ScreenController::new(device, ack_rx, reliable);
            println!("Clearing screen...");
            screen.clear().await?;
            println!("Writing text: {}", text);
            screen.write_text(&text).await?;
            println!("Done.");
        }
        Commands::SendClaude {
            vid,
            pid,
            mock,
            reliable,
        } => {
            let device = open_device(vid, pid)?;
            let device = Arc::new(device);

            let (ack_tx, ack_rx) = mpsc::channel::<AckReport>(256);
            spawn_read_thread(Arc::clone(&device), ack_tx);

            let screen = ScreenController::new(device, ack_rx, reliable);
            let status = if mock {
                ClaudeStatus::get_mock()
            } else {
                ClaudeStatus::detect_running().unwrap_or_else(ClaudeStatus::get_mock)
            };
            let text = format_for_screen(&status);

            println!("Sending Claude status to screen...");
            screen.clear().await?;
            screen.write_text(&text).await?;
            println!("Done.");
        }
        Commands::Daemon {
            socket,
            vid,
            pid,
            reliable,
            tcp,
            poll_ms,
        } => {
            run_daemon(&socket, tcp.as_deref(), vid, pid, reliable, poll_ms).await?;
        }
        Commands::Emit {
            socket,
            tcp,
            session_id,
            source,
            tool,
            state,
            cwd,
            branch,
            model,
            message,
            tokens,
            context_pct,
            cost,
            quota_remaining,
            quota_reset,
            reply,
        } => {
            let project_dir = cwd.or_else(current_dir_string);
            let git_branch = branch.or_else(|| {
                project_dir
                    .as_deref()
                    .and_then(|cwd| find_git_branch(std::path::Path::new(cwd)))
            });
            let event = SessionEvent {
                session_id,
                source: Some(source),
                tool: Some(tool),
                state: Some(state),
                project_dir,
                git_branch,
                model,
                message,
                tokens,
                context_pct: context_pct.map(|pct| pct.min(100)),
                cost,
                quota_remaining,
                quota_reset,
                reply,
            };
            if let Some(addr) = tcp {
                emit_tcp_event(&addr, &event).await?;
            } else {
                emit_event(&socket, &event)?;
            }
        }
        Commands::HookScript { socket, tcp } => {
            print_hook_script(&socket, tcp.as_deref());
        }
        Commands::Sessions => {
            for session in detect_proc_sessions() {
                println!(
                    "{:<7} {:<7} {:<20} {}",
                    session.source, session.tool, session.project_dir, session.state
                );
                if !session.message.is_empty() {
                    println!("  {}", session.message);
                }
                let mut meta = Vec::new();
                if !session.model.is_empty() {
                    meta.push(format!("model {}", session.model));
                }
                if let Some(tokens) = session.tokens {
                    meta.push(format!("tok {}", tokens));
                }
                if let Some(context_pct) = session.context_pct {
                    meta.push(format!("ctx {}%", context_pct));
                }
                if let Some(cost) = session.cost {
                    meta.push(format!("cost {}", cost));
                }
                if let Some(quota) = session.quota_remaining {
                    meta.push(format!("quota {}", quota));
                }
                if let Some(reset) = session.quota_reset {
                    meta.push(format!("reset {}", reset));
                }
                let meta = meta.join("  ");
                if !meta.is_empty() {
                    println!("  {}", meta);
                }
                if let Some(reply) = session.reply {
                    println!("  reply {}", reply);
                }
            }
        }
        Commands::ForwardSessions {
            socket,
            tcp,
            poll_ms,
            once,
        } => loop {
            let count = forward_detected_sessions(&socket, tcp.as_deref()).await?;
            println!("forwarded {} sessions", count);
            if once {
                break;
            }
            tokio::time::sleep(std::time::Duration::from_millis(poll_ms.max(100))).await;
        },
    }

    Ok(())
}

async fn forward_detected_sessions(socket: &str, tcp: Option<&str>) -> Result<usize> {
    let sessions = detect_proc_sessions();
    let count = sessions.len();
    for session in sessions {
        let event = session_event_from_status(session);
        if let Some(addr) = tcp {
            emit_tcp_event(addr, &event).await?;
        } else {
            emit_event(socket, &event)?;
        }
    }
    Ok(count)
}

fn session_event_from_status(session: SessionStatus) -> SessionEvent {
    SessionEvent {
        session_id: Some(session.session_id),
        source: Some(session.source),
        tool: Some(session.tool),
        state: Some(session.state),
        project_dir: Some(session.project_dir),
        git_branch: Some(session.git_branch),
        model: empty_to_none(session.model),
        message: empty_to_none(session.message),
        tokens: session.tokens,
        context_pct: session.context_pct,
        cost: session.cost,
        quota_remaining: session.quota_remaining,
        quota_reset: session.quota_reset,
        reply: session.reply,
    }
}

fn empty_to_none(value: String) -> Option<String> {
    if value.is_empty() {
        None
    } else {
        Some(value)
    }
}

fn open_device(vid: Option<u16>, pid: Option<u16>) -> Result<VibeDevice> {
    if let (Some(v), Some(p)) = (vid, pid) {
        VibeDevice::open_by_vid_pid(v, p)
    } else {
        let list = VibeDevice::find_all()?;
        if list.is_empty() {
            return Err(anyhow::anyhow!(
                "No Vibe Keyboard found. Use --vid/--pid or check Scan."
            ));
        }
        VibeDevice::open_by_path(list[0].path())
    }
}

fn print_hook_script(socket: &str, tcp: Option<&str>) {
    let bin = std::env::current_exe()
        .ok()
        .map(|path| path.display().to_string())
        .unwrap_or_else(|| "vibe-bridge".to_string());
    let tcp = tcp.unwrap_or("");
    println!(
        r#"# Vibe Bridge shell hook. Source this from ~/.bashrc or ~/.zshrc.
export VIBE_BRIDGE_SOCKET="{socket}"
export VIBE_BRIDGE_TCP="${{VIBE_BRIDGE_TCP:-{tcp}}}"
export VIBE_BRIDGE_BIN="${{VIBE_BRIDGE_BIN:-{bin}}}"

if [ -z "${{VIBE_BRIDGE_TTY:-}}" ]; then
  VIBE_BRIDGE_TTY="$(tty 2>/dev/null || printf 'notty')"
  export VIBE_BRIDGE_TTY
fi
export VIBE_BRIDGE_SESSION_ID="${{VIBE_BRIDGE_SESSION_ID:-shell:${{VIBE_BRIDGE_TTY}}:$$}}"

_vibe_bridge_emit() {{
  if [ -n "${{VIBE_BRIDGE_TCP:-}}" ]; then
    command "$VIBE_BRIDGE_BIN" emit --tcp "$VIBE_BRIDGE_TCP" \
      --session-id "$VIBE_BRIDGE_SESSION_ID" \
      --source "${{TERM_PROGRAM:-wsl}}" \
      --tool shell \
      --state "$1" \
      --cwd "$PWD" \
      --message "${{2:-}}" >/dev/null 2>&1 || true
  else
    command "$VIBE_BRIDGE_BIN" emit --socket "$VIBE_BRIDGE_SOCKET" \
      --session-id "$VIBE_BRIDGE_SESSION_ID" \
      --source "${{TERM_PROGRAM:-wsl}}" \
      --tool shell \
      --state "$1" \
      --cwd "$PWD" \
      --message "${{2:-}}" >/dev/null 2>&1 || true
  fi
}}

_vibe_bridge_prompt() {{
  local code="$?"
  _vibe_bridge_emit "idle:$code" "${{VIBE_BRIDGE_LAST_CMD:-}}"
}}

_vibe_bridge_preexec_bash() {{
  case "$BASH_COMMAND" in
    *_vibe_bridge_*|*vibe-bridge\ emit*) return ;;
  esac
  VIBE_BRIDGE_LAST_CMD="$BASH_COMMAND"
  _vibe_bridge_emit running "$BASH_COMMAND"
}}

if [ -n "$BASH_VERSION" ]; then
  trap _vibe_bridge_preexec_bash DEBUG
  PROMPT_COMMAND="_vibe_bridge_prompt${{PROMPT_COMMAND:+;$PROMPT_COMMAND}}"
elif [ -n "$ZSH_VERSION" ]; then
  autoload -Uz add-zsh-hook
  _vibe_bridge_preexec_zsh() {{
    VIBE_BRIDGE_LAST_CMD="$1"
    _vibe_bridge_emit running "$1"
  }}
  add-zsh-hook preexec _vibe_bridge_preexec_zsh
  add-zsh-hook precmd _vibe_bridge_prompt
fi
"#
    );
}
