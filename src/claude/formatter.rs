use super::status::ClaudeStatus;
use crate::session::SessionStatus;

const VALUE_WIDTH: usize = 86;
const SESSION_ROWS: usize = 4;
const SCREEN_WIDTH: usize = 94;
const DETAIL_WIDTH: usize = 90;
const CONTEXT_BAR_WIDTH: usize = 16;

pub fn format_for_screen(status: &ClaudeStatus) -> String {
    let mut out = String::new();

    out.push_str("\x1b[2J\x1b[H");
    out.push_str("\x1b[1;38;2;255;196;0mVIBE BRIDGE\x1b[0m");
    out.push_str("  \x1b[38;2;167;122;61mClaude status\x1b[0m\r\n");
    out.push_str("\x1b[38;2;106;65;12m");
    out.push_str("-------------------------------------------------------------------------------");
    out.push_str("\x1b[0m\r\n");

    push_field(&mut out, "Model", &status.model);
    push_field(&mut out, "State", &status.state);
    push_field(&mut out, "Project", &status.project_dir);

    let tokens = status
        .tokens
        .map(|t| t.to_string())
        .unwrap_or_else(|| "n/a".to_string());
    push_field(&mut out, "Tokens", &tokens);

    out.push_str("\r\n");
    out.push_str("\x1b[38;2;70;200;0mREADY\x1b[0m ");
    out.push_str("\x1b[38;2;167;122;61m");
    out.push_str("HID output report 0x20 -> LCD terminal");
    out.push_str("\x1b[0m\r\n");

    out
}

pub fn format_sessions_for_screen(sessions: &[SessionStatus]) -> String {
    let mut out = String::new();

    out.push_str("\x1b[2J\x1b[H");
    out.push_str("\x1b[1;38;2;255;196;0mVIBE BRIDGE\x1b[0m");
    out.push_str("  \x1b[38;2;167;122;61mactive sessions\x1b[0m\r\n");
    out.push_str("\x1b[38;2;106;65;12m");
    out.push_str(&"-".repeat(SCREEN_WIDTH));
    out.push_str("\x1b[0m\r\n");

    if sessions.is_empty() {
        out.push_str("\x1b[38;2;167;122;61mNo terminal or AI sessions detected\x1b[0m\r\n");
        out.push_str("\r\nRun shell hooks or start Claude/Codex in this WSL.\r\n");
        return out;
    }

    for (idx, session) in sessions.iter().take(SESSION_ROWS).enumerate() {
        let color = match session.tool.as_str() {
            "Claude" => "\x1b[38;2;0;210;140m",
            "Codex" => "\x1b[38;2;93;173;255m",
            "Gemini" => "\x1b[38;2;255;126;95m",
            "Shell" => "\x1b[38;2;255;196;0m",
            _ => "\x1b[38;2;255;227;162m",
        };

        out.push_str(color);
        out.push_str(&fit_end(&session_title(idx + 1, session), SCREEN_WIDTH));
        out.push_str("\x1b[0m\r\n");

        if let Some(reply) = &session.reply {
            out.push_str("  \x1b[38;2;216;235;255m");
            out.push_str(&fit_end(&format!("> {}", reply), DETAIL_WIDTH));
            out.push_str("\x1b[0m\r\n");
        } else if !session.message.is_empty() {
            out.push_str("  \x1b[38;2;167;122;61m");
            out.push_str(&fit_end(&format!("? {}", session.message), DETAIL_WIDTH));
            out.push_str("\x1b[0m\r\n");
        }

        if idx + 1 < sessions.len().min(SESSION_ROWS) {
            out.push_str("\r\n");
        }
    }

    out
}

fn push_field(out: &mut String, label: &str, value: &str) {
    out.push_str("\x1b[38;2;167;122;61m");
    out.push_str(&format!("{:<8}", label));
    out.push_str("\x1b[0m ");
    out.push_str(&fit_middle(value, VALUE_WIDTH));
    out.push_str("\r\n");
}

fn fit_middle(value: &str, max_chars: usize) -> String {
    let len = value.chars().count();
    if len <= max_chars {
        return value.to_string();
    }
    if max_chars <= 3 {
        return ".".repeat(max_chars);
    }

    let keep_left = (max_chars - 3) / 2;
    let keep_right = max_chars - 3 - keep_left;
    let left: String = value.chars().take(keep_left).collect();
    let right: String = value
        .chars()
        .rev()
        .take(keep_right)
        .collect::<Vec<_>>()
        .into_iter()
        .rev()
        .collect();

    format!("{}...{}", left, right)
}

fn fit_end(value: &str, max_chars: usize) -> String {
    let len = value.chars().count();
    if len <= max_chars {
        return value.to_string();
    }
    if max_chars <= 1 {
        return ".".repeat(max_chars);
    }

    let keep = max_chars - 1;
    let left: String = value.chars().take(keep).collect();
    format!("{}.", left)
}

fn short_project(path: &str) -> String {
    std::path::Path::new(path)
        .file_name()
        .and_then(|name| name.to_str())
        .filter(|name| !name.is_empty())
        .unwrap_or(path)
        .to_string()
}

fn session_title(index: usize, session: &SessionStatus) -> String {
    let project = short_project(&session.project_dir);
    let mut parts = vec![format!("{}.", index), session.tool.clone()];
    if !session.model.is_empty() {
        parts.push(short_model(&session.model));
    }
    parts.push(fit_end(&project, 18));
    if let Some(tokens) = session.tokens {
        parts.push(compact_tokens(tokens));
    }
    if let Some(cost) = &session.cost {
        parts.push(compact_cost(cost));
    }
    if let Some(quota) = &session.quota_remaining {
        parts.push(fit_end(quota, 12));
    }
    if let Some(reset) = &session.quota_reset {
        parts.push(format!("R {}", fit_end(reset, 8)));
    }
    if let Some(context_pct) = session.context_pct {
        parts.push(format!("ctx {}", progress_bar(context_pct)));
    }
    if session.source != "wsl" && !session.source.is_empty() {
        parts.push(fit_end(&session.source, 12));
    }
    parts.push(session.state.clone());

    parts.join("  ")
}

fn short_model(model: &str) -> String {
    let model = model
        .strip_prefix("claude-")
        .or_else(|| model.strip_prefix("gemini-"))
        .unwrap_or(model);
    fit_end(model, 24)
}

fn compact_tokens(tokens: u64) -> String {
    if tokens >= 1_000_000 {
        format!("{:.1}M", tokens as f64 / 1_000_000.0)
    } else if tokens >= 10_000 {
        format!("{:.1}k", tokens as f64 / 1_000.0)
    } else if tokens >= 1_000 {
        format!("{:.0}k", tokens as f64 / 1_000.0)
    } else {
        tokens.to_string()
    }
}

fn compact_cost(cost: &str) -> String {
    let is_estimate = cost.contains('~');
    let numeric = cost
        .trim()
        .trim_start_matches('$')
        .trim_start_matches('~')
        .trim_start_matches('$');
    let Ok(value) = numeric.parse::<f64>() else {
        return fit_end(cost, 8);
    };
    if is_estimate {
        format!("~${value:.2}")
    } else {
        format!("${value:.2}")
    }
}

fn progress_bar(percent: u8) -> String {
    let percent = percent.min(100);
    let filled = ((percent as usize * CONTEXT_BAR_WIDTH) + 50) / 100;
    let mut bar = String::from("[");
    for idx in 0..CONTEXT_BAR_WIDTH {
        bar.push(if idx < filled { '#' } else { '-' });
    }
    bar.push(']');
    format!("{bar} {percent}%")
}
