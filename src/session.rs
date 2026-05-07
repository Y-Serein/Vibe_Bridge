use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const CLAUDE_PRICING: &[(&str, f64, f64)] = &[
    ("opus-4", 15.0, 75.0),
    ("sonnet-4", 3.0, 15.0),
    ("haiku-4", 0.80, 4.0),
    ("claude-3-5-sonnet", 3.0, 15.0),
    ("claude-3-opus", 15.0, 75.0),
];

const OPENAI_PRICING: &[(&str, f64, f64, f64)] = &[
    ("gpt-5", 1.25, 0.125, 10.0),
    ("gpt-4.1", 2.0, 0.5, 8.0),
    ("o4", 1.10, 0.275, 4.40),
    ("o3", 2.0, 0.5, 8.0),
];

const GEMINI_PRICING: &[(&str, f64, f64)] = &[
    ("gemini-3-flash", 0.30, 2.50),
    ("gemini-2.5-flash", 0.30, 2.50),
    ("gemini-2.5-pro", 1.25, 10.0),
    ("gemini-1.5-pro", 1.25, 5.0),
    ("gemini-1.5-flash", 0.075, 0.30),
];

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SessionEvent {
    #[serde(default)]
    pub session_id: Option<String>,
    #[serde(default)]
    pub source: Option<String>,
    #[serde(default)]
    pub tool: Option<String>,
    #[serde(default)]
    pub state: Option<String>,
    #[serde(default)]
    pub project_dir: Option<String>,
    #[serde(default)]
    pub git_branch: Option<String>,
    #[serde(default)]
    pub model: Option<String>,
    #[serde(default)]
    pub message: Option<String>,
    #[serde(default)]
    pub tokens: Option<u64>,
    #[serde(default)]
    pub context_pct: Option<u8>,
    #[serde(default)]
    pub cost: Option<String>,
    #[serde(default)]
    pub quota_remaining: Option<String>,
    #[serde(default)]
    pub quota_reset: Option<String>,
    #[serde(default)]
    pub reply: Option<String>,
}

#[derive(Debug, Clone)]
pub struct SessionStatus {
    pub session_id: String,
    pub source: String,
    pub tool: String,
    pub state: String,
    pub project_dir: String,
    pub git_branch: String,
    pub model: String,
    pub message: String,
    pub tokens: Option<u64>,
    pub context_pct: Option<u8>,
    pub cost: Option<String>,
    pub quota_remaining: Option<String>,
    pub quota_reset: Option<String>,
    pub reply: Option<String>,
    pub updated_ms: u64,
    pub auto: bool,
}

#[derive(Debug, Default)]
pub struct SessionRegistry {
    sessions: BTreeMap<String, SessionStatus>,
}

impl SessionRegistry {
    pub fn apply_event(&mut self, event: SessionEvent) {
        let cwd = event
            .project_dir
            .unwrap_or_else(|| current_dir_string().unwrap_or_else(|| "?".to_string()));
        let source = event.source.unwrap_or_else(|| "hook".to_string());
        let tool = event.tool.unwrap_or_else(|| "session".to_string());
        let session_id = event
            .session_id
            .unwrap_or_else(|| format!("{}:{}", source, cwd));
        let branch = event
            .git_branch
            .or_else(|| find_git_branch(Path::new(&cwd)))
            .unwrap_or_else(|| "unknown".to_string());

        let mut next = SessionStatus {
            session_id: session_id.clone(),
            source,
            tool,
            state: event.state.unwrap_or_else(|| "active".to_string()),
            project_dir: cwd,
            git_branch: branch,
            model: event.model.unwrap_or_default(),
            message: event.message.unwrap_or_default(),
            tokens: event.tokens,
            context_pct: event.context_pct.map(|pct| pct.min(100)),
            cost: clean_optional(event.cost),
            quota_remaining: clean_optional(event.quota_remaining),
            quota_reset: clean_optional(event.quota_reset),
            reply: clean_optional(event.reply),
            updated_ms: now_ms(),
            auto: false,
        };
        if let Some(prev) = self.sessions.get(&session_id) {
            if same_visible_content(prev, &next) {
                next.updated_ms = prev.updated_ms;
            }
        }

        self.sessions.insert(session_id, next);
    }

    pub fn replace_auto(&mut self, detected: Vec<SessionStatus>) {
        let detected_by_id = detected
            .into_iter()
            .map(|s| (s.session_id.clone(), s))
            .collect::<BTreeMap<_, _>>();

        self.sessions
            .retain(|id, existing| !existing.auto || detected_by_id.contains_key(id));

        for (id, mut session) in detected_by_id {
            if let Some(prev) = self.sessions.get(&id) {
                if same_visible_content(prev, &session) {
                    session.updated_ms = prev.updated_ms;
                }
            }
            self.sessions.insert(id, session);
        }
    }

    pub fn visible_sessions(&self) -> Vec<SessionStatus> {
        let mut sessions = self.sessions.values().cloned().collect::<Vec<_>>();
        sessions.sort_by(|a, b| {
            b.updated_ms
                .cmp(&a.updated_ms)
                .then_with(|| a.source.cmp(&b.source))
                .then_with(|| a.project_dir.cmp(&b.project_dir))
        });
        sessions
    }
}

fn same_visible_content(a: &SessionStatus, b: &SessionStatus) -> bool {
    a.source == b.source
        && a.tool == b.tool
        && a.state == b.state
        && a.project_dir == b.project_dir
        && a.git_branch == b.git_branch
        && a.model == b.model
        && a.message == b.message
        && a.tokens == b.tokens
        && a.context_pct == b.context_pct
        && a.cost == b.cost
        && a.quota_remaining == b.quota_remaining
        && a.quota_reset == b.quota_reset
        && a.reply == b.reply
        && a.auto == b.auto
}

pub fn detect_proc_sessions() -> Vec<SessionStatus> {
    let own_pid = std::process::id();
    let mut sessions = BTreeMap::<String, SessionStatus>::new();

    let Ok(entries) = fs::read_dir("/proc") else {
        return Vec::new();
    };

    for entry in entries.flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        if pid == own_pid {
            continue;
        }

        let proc_dir = entry.path();
        let cmdline = read_cmdline(&proc_dir).unwrap_or_default();
        let comm = fs::read_to_string(proc_dir.join("comm")).unwrap_or_default();
        let name = if cmdline.is_empty() {
            comm.trim().to_string()
        } else {
            cmdline.clone()
        };
        let name_lc = name.to_ascii_lowercase();

        let Some(tool) = classify_process(&name_lc, &proc_dir) else {
            continue;
        };

        let cwd = fs::read_link(proc_dir.join("cwd")).unwrap_or_else(|_| PathBuf::from("?"));
        let project_dir = cwd.display().to_string();
        let git_branch = find_git_branch(&cwd).unwrap_or_else(|| "unknown".to_string());
        let source = infer_source(&proc_dir);
        let tty = tty_name(&proc_dir).or_else(|| ancestor_tty(pid, 8));
        let session_id = auto_session_id(&tool, &source, &project_dir, tty.as_deref());
        let message = passive_message(&tool);

        let mut session = SessionStatus {
            session_id,
            source,
            tool,
            state: "detected".to_string(),
            project_dir,
            git_branch,
            model: infer_model(&cmdline),
            message,
            tokens: None,
            context_pct: None,
            cost: None,
            quota_remaining: None,
            quota_reset: None,
            reply: None,
            updated_ms: now_ms(),
            auto: true,
        };
        enrich_detected_session(&mut session);
        merge_auto_session(&mut sessions, session);
    }

    sessions.into_iter().map(|(_, session)| session).collect()
}

pub fn find_git_branch(start: &Path) -> Option<String> {
    for dir in start.ancestors() {
        let git_path = dir.join(".git");
        let head_path = if git_path.is_dir() {
            git_path.join("HEAD")
        } else if git_path.is_file() {
            let git_file = fs::read_to_string(&git_path).ok()?;
            let git_dir = git_file.strip_prefix("gitdir:")?.trim();
            resolve_git_path(dir, git_dir).join("HEAD")
        } else {
            continue;
        };

        let head = fs::read_to_string(head_path).ok()?;
        let head = head.trim();
        if let Some(branch) = head.strip_prefix("ref: refs/heads/") {
            return Some(branch.to_string());
        }
        return Some(short_sha(head));
    }

    None
}

pub fn current_dir_string() -> Option<String> {
    std::env::current_dir()
        .ok()
        .map(|path| path.display().to_string())
}

fn classify_process(name_lc: &str, proc_dir: &Path) -> Option<String> {
    if name_lc.contains("vibe-bridge") {
        return None;
    }
    if name_lc.contains("claude") || name_lc.contains("@anthropic-ai/claude-code") {
        return Some("Claude".to_string());
    }
    if name_lc.contains("codex") || name_lc.contains("openai") {
        return Some("Codex".to_string());
    }
    if name_lc.contains("gemini") || name_lc.contains("@google/gemini-cli") {
        return Some("Gemini".to_string());
    }
    if is_shell_process(name_lc) && has_tty(proc_dir) {
        return Some("Shell".to_string());
    }
    None
}

fn is_shell_process(name_lc: &str) -> bool {
    name_lc == "bash"
        || name_lc == "zsh"
        || name_lc == "fish"
        || name_lc == "nu"
        || name_lc == "pwsh"
        || name_lc.contains("/bash")
        || name_lc.contains("/zsh")
        || name_lc.contains("/fish")
        || name_lc.contains("/nu ")
        || name_lc.contains("/pwsh")
        || name_lc.contains("tmux")
}

fn has_tty(proc_dir: &Path) -> bool {
    tty_name(proc_dir).is_some()
}

fn tty_name(proc_dir: &Path) -> Option<String> {
    fs::read_link(proc_dir.join("fd/0"))
        .ok()
        .and_then(|target| {
            let s = target.display().to_string();
            if s.contains("/dev/pts/") || s.contains("/dev/tty") {
                Some(s)
            } else {
                None
            }
        })
}

fn ancestor_tty(mut pid: u32, max_depth: usize) -> Option<String> {
    for _ in 0..max_depth {
        let ppid = read_ppid(pid)?;
        if ppid == 0 || ppid == pid {
            return None;
        }
        let proc_dir = PathBuf::from(format!("/proc/{ppid}"));
        if let Some(tty) = tty_name(&proc_dir) {
            return Some(tty);
        }
        pid = ppid;
    }
    None
}

fn read_ppid(pid: u32) -> Option<u32> {
    let stat = fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    let after_comm = stat.rsplit_once(") ")?.1;
    let mut fields = after_comm.split_whitespace();
    fields.next()?;
    fields.next()?.parse::<u32>().ok()
}

fn infer_source(proc_dir: &Path) -> String {
    let env = read_environ(proc_dir)
        .unwrap_or_default()
        .to_ascii_lowercase();
    if env.contains("term_program=vscode") || env.contains("vscode_") {
        "vscode".to_string()
    } else if env.contains("tmux=") {
        "tmux".to_string()
    } else if env.contains("wsl_interop=")
        || Path::new("/proc/sys/fs/binfmt_misc/WSLInterop").exists()
    {
        "wsl".to_string()
    } else {
        "terminal".to_string()
    }
}

fn infer_model(cmdline: &str) -> String {
    let tokens = cmdline.split_whitespace().collect::<Vec<_>>();
    for (idx, token) in tokens.iter().enumerate() {
        let token_lc = token.to_ascii_lowercase();
        if (token_lc == "--model" || token_lc == "-m") && idx + 1 < tokens.len() {
            return clean_model_token(tokens[idx + 1]);
        }
        if token_lc.starts_with("--model=") {
            return clean_model_token(&token["--model=".len()..]);
        }
        if token_lc.starts_with("model=") {
            return clean_model_token(&token["model=".len()..]);
        }
    }

    for token in &tokens {
        if let Some(model) = model_candidate(token) {
            return model;
        }
    }

    let cmdline_lc = cmdline.to_ascii_lowercase();
    if cmdline_lc.contains("opus") {
        "Opus".to_string()
    } else if cmdline_lc.contains("haiku") {
        "Haiku".to_string()
    } else if cmdline_lc.contains("sonnet") {
        "Sonnet".to_string()
    } else {
        String::new()
    }
}

fn model_candidate(token: &str) -> Option<String> {
    let cleaned = clean_model_token(token);
    let lc = cleaned.to_ascii_lowercase();
    if lc.starts_with("gpt-")
        || lc.starts_with("o1")
        || lc.starts_with("o3")
        || lc.starts_with("o4")
        || lc.starts_with("claude-")
        || lc.starts_with("gemini-")
    {
        Some(cleaned)
    } else {
        None
    }
}

fn clean_model_token(token: &str) -> String {
    token
        .trim_matches(|c: char| {
            c == '"'
                || c == '\''
                || c == '`'
                || c == ','
                || c == ';'
                || c == ')'
                || c == ']'
                || c == '}'
        })
        .to_string()
}

fn read_cmdline(proc_dir: &Path) -> Option<String> {
    let bytes = fs::read(proc_dir.join("cmdline")).ok()?;
    if bytes.is_empty() {
        return None;
    }

    let parts = bytes
        .split(|b| *b == 0)
        .filter(|part| !part.is_empty())
        .map(|part| String::from_utf8_lossy(part));

    Some(parts.collect::<Vec<_>>().join(" "))
}

fn read_environ(proc_dir: &Path) -> Option<String> {
    let bytes = fs::read(proc_dir.join("environ")).ok()?;
    if bytes.is_empty() {
        return None;
    }

    let parts = bytes
        .split(|b| *b == 0)
        .filter(|part| !part.is_empty())
        .map(|part| String::from_utf8_lossy(part));

    Some(parts.collect::<Vec<_>>().join("\n"))
}

fn auto_session_id(tool: &str, source: &str, project_dir: &str, tty: Option<&str>) -> String {
    let tool_key = tool.to_ascii_lowercase();
    let location = tty.unwrap_or("no-tty");
    format!("auto:{tool_key}:{source}:{location}:{project_dir}")
}

fn passive_message(tool: &str) -> String {
    match tool {
        "Shell" => String::new(),
        _ => String::new(),
    }
}

fn merge_auto_session(sessions: &mut BTreeMap<String, SessionStatus>, candidate: SessionStatus) {
    if let Some(existing) = sessions.get_mut(&candidate.session_id) {
        if existing.model.is_empty() && !candidate.model.is_empty() {
            existing.model = candidate.model;
        }
        if existing.git_branch == "unknown" && candidate.git_branch != "unknown" {
            existing.git_branch = candidate.git_branch;
        }
        if existing.message.is_empty() && !candidate.message.is_empty() {
            existing.message = candidate.message;
        }
        if existing.tokens.is_none() && candidate.tokens.is_some() {
            existing.tokens = candidate.tokens;
        }
        if existing.context_pct.is_none() && candidate.context_pct.is_some() {
            existing.context_pct = candidate.context_pct;
        }
        if existing.cost.is_none() && candidate.cost.is_some() {
            existing.cost = candidate.cost;
        }
        if existing.reply.is_none() && candidate.reply.is_some() {
            existing.reply = candidate.reply;
        }
        return;
    }

    sessions.insert(candidate.session_id.clone(), candidate);
}

fn enrich_detected_session(session: &mut SessionStatus) {
    match session.tool.as_str() {
        "Codex" => enrich_codex_session(session),
        "Claude" => enrich_claude_session(session),
        "Gemini" => enrich_gemini_session(session),
        _ => {}
    }
}

fn enrich_codex_session(session: &mut SessionStatus) {
    let usage = latest_codex_usage();
    if session.model.is_empty() {
        session.model = usage
            .as_ref()
            .and_then(|usage| usage.model.clone())
            .or_else(latest_codex_model)
            .or_else(codex_config_model)
            .unwrap_or_default();
    }
    if let Some(usage) = usage {
        if session.tokens.is_none() && usage.total_tokens > 0 {
            session.tokens = Some(usage.total_tokens);
        }
        if session.context_pct.is_none() && usage.input_tokens > 0 && !session.model.is_empty() {
            session.context_pct = context_percent(usage.input_tokens, &session.model);
        }
        if session.cost.is_none() && !session.model.is_empty() {
            session.cost = estimate_openai_cost(&session.model, &usage);
        }
    }
}

fn enrich_claude_session(session: &mut SessionStatus) {
    let Some(path) = find_claude_transcript_for_session(session) else {
        return;
    };
    let Some(tail) = tail_string(&path, 4 * 1024 * 1024) else {
        return;
    };
    let data = parse_claude_transcript_tail(&tail);

    if session.model.is_empty() && !data.model.is_empty() {
        session.model = data.model;
    }
    let tokens = data.tokens_in.saturating_add(data.tokens_out);
    if session.tokens.is_none() && tokens > 0 {
        session.tokens = Some(tokens);
    }
    if session.cost.is_none() && data.cost_usd > 0.0 {
        session.cost = Some(format!("${:.4}", data.cost_usd));
    }
    if session.context_pct.is_none() && data.last_input_tokens > 0 && !session.model.is_empty() {
        session.context_pct = context_percent(data.last_input_tokens, &session.model);
    }
    if session.message.is_empty() && !data.last_message.is_empty() {
        session.message = data.last_message;
    }
    if session.reply.is_none() && !data.last_ai_output.is_empty() {
        session.reply = Some(data.last_ai_output);
    }
    if session.state == "detected" && !data.inferred_status.is_empty() {
        session.state = data.inferred_status;
    }
}

fn enrich_gemini_session(session: &mut SessionStatus) {
    let data = find_gemini_transcript_for_session(session)
        .and_then(|path| tail_string(&path, 4 * 1024 * 1024))
        .map(|tail| parse_gemini_transcript_tail(&tail));

    if let Some(data) = data {
        if session.model.is_empty() && !data.model.is_empty() {
            session.model = data.model;
        }
        if session.tokens.is_none() && data.tokens_total > 0 {
            session.tokens = Some(data.tokens_total);
        }
        if session.cost.is_none() && data.tokens_input > 0 && data.tokens_output > 0 {
            session.cost =
                estimate_gemini_cost(&session.model, data.tokens_input, data.tokens_output);
        }
        if session.context_pct.is_none() && data.last_input_tokens > 0 && !session.model.is_empty()
        {
            session.context_pct = context_percent(data.last_input_tokens, &session.model);
        }
        if session.message.is_empty() && !data.last_message.is_empty() {
            session.message = data.last_message;
        }
        if session.reply.is_none() && !data.last_ai_output.is_empty() {
            session.reply = Some(data.last_ai_output);
        }
        if session.state == "detected" && !data.inferred_status.is_empty() {
            session.state = data.inferred_status;
        }
    }

    if session.model.is_empty() {
        session.model = gemini_settings_model().unwrap_or_default();
    }
}

fn latest_codex_model() -> Option<String> {
    let tail = tail_string(&home_path(".codex/log/codex-tui.log")?, 256 * 1024)?;
    for line in tail.lines().rev() {
        if let Some(value) = value_after_marker(line, "model=") {
            if model_candidate(&value).is_some() {
                return Some(value);
            }
        }
    }
    None
}

fn latest_codex_usage() -> Option<CodexUsageData> {
    let tail = tail_string(&home_path(".codex/log/codex-tui.log")?, 512 * 1024)?;
    for line in tail.lines().rev() {
        if !line.contains("codex.turn.token_usage.total_tokens=") {
            continue;
        }

        let mut usage = CodexUsageData {
            model: value_after_marker(line, "model=")
                .filter(|model| model_candidate(model).is_some()),
            input_tokens: value_after_marker(line, "codex.turn.token_usage.input_tokens=")
                .and_then(|value| value.parse::<u64>().ok())
                .unwrap_or(0),
            cached_input_tokens: value_after_marker(
                line,
                "codex.turn.token_usage.cached_input_tokens=",
            )
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(0),
            non_cached_input_tokens: value_after_marker(
                line,
                "codex.turn.token_usage.non_cached_input_tokens=",
            )
            .and_then(|value| value.parse::<u64>().ok())
            .unwrap_or(0),
            output_tokens: value_after_marker(line, "codex.turn.token_usage.output_tokens=")
                .and_then(|value| value.parse::<u64>().ok())
                .unwrap_or(0),
            total_tokens: value_after_marker(line, "codex.turn.token_usage.total_tokens=")
                .and_then(|value| value.parse::<u64>().ok())
                .unwrap_or(0),
        };
        if usage.non_cached_input_tokens == 0 {
            usage.non_cached_input_tokens =
                usage.input_tokens.saturating_sub(usage.cached_input_tokens);
        }
        if usage.total_tokens > 0 {
            return Some(usage);
        }
    }
    None
}

fn codex_config_model() -> Option<String> {
    let text = fs::read_to_string(home_path(".codex/config.toml")?).ok()?;
    for line in text.lines() {
        let line = line.trim();
        if let Some(value) = line.strip_prefix("model =") {
            let model = clean_model_token(value.trim());
            if !model.is_empty() {
                return Some(model);
            }
        }
    }
    None
}

fn gemini_settings_model() -> Option<String> {
    let text = fs::read_to_string(home_path(".gemini/settings.json")?).ok()?;
    let value = serde_json::from_str::<Value>(&text).ok()?;
    find_json_string(&value, &["model", "defaultModel"]).filter(|model| !model.is_empty())
}

#[derive(Debug)]
struct CodexUsageData {
    model: Option<String>,
    input_tokens: u64,
    cached_input_tokens: u64,
    non_cached_input_tokens: u64,
    output_tokens: u64,
    total_tokens: u64,
}

#[derive(Debug, Default)]
struct ClaudeTranscriptData {
    model: String,
    tokens_in: u64,
    tokens_out: u64,
    last_input_tokens: u64,
    cost_usd: f64,
    last_message: String,
    last_ai_output: String,
    inferred_status: String,
}

#[derive(Debug, Default)]
struct GeminiTranscriptData {
    model: String,
    tokens_input: u64,
    tokens_output: u64,
    tokens_total: u64,
    last_input_tokens: u64,
    last_message: String,
    last_ai_output: String,
    inferred_status: String,
}

#[derive(Debug)]
enum ClaudeJsonlEntry {
    Assistant {
        model: Option<String>,
        usage: Option<ClaudeUsageData>,
        content: Option<String>,
    },
    Human {
        content: String,
    },
}

#[derive(Debug)]
struct ClaudeUsageData {
    input_tokens: u64,
    output_tokens: u64,
}

#[derive(Debug)]
struct GeminiUsageData {
    input_tokens: u64,
    output_tokens: u64,
    total_tokens: u64,
}

fn find_claude_transcript_for_session(session: &SessionStatus) -> Option<PathBuf> {
    let root = home_path(".claude/projects")?;
    let project_dir = session.project_dir.as_str();
    let mut best_match = None::<(SystemTime, PathBuf)>;
    let mut best_any = None::<(SystemTime, PathBuf)>;
    let mut stack = vec![root];

    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            if metadata.is_dir() {
                stack.push(path);
                continue;
            }
            if !metadata.is_file() || path.extension().and_then(|ext| ext.to_str()) != Some("jsonl")
            {
                continue;
            }

            let modified = metadata.modified().unwrap_or(SystemTime::UNIX_EPOCH);
            if is_newer(&best_any, modified) {
                best_any = Some((modified, path.clone()));
            }

            if transcript_matches_project(&path, project_dir) && is_newer(&best_match, modified) {
                best_match = Some((modified, path));
            }
        }
    }

    best_match.or(best_any).map(|(_, path)| path)
}

fn transcript_matches_project(path: &Path, project_dir: &str) -> bool {
    if project_dir.is_empty() || project_dir == "?" {
        return false;
    }
    let Some(parent) = path.parent() else {
        return false;
    };
    let Some(name) = parent.file_name().and_then(|name| name.to_str()) else {
        return false;
    };
    resolve_cwd_from_claude_project_hash(name) == project_dir
}

fn find_gemini_transcript_for_session(session: &SessionStatus) -> Option<PathBuf> {
    let root = home_path(".gemini/tmp")?;
    let project_dir = session.project_dir.as_str();
    let mut best_match = None::<(SystemTime, PathBuf)>;
    let mut best_any = None::<(SystemTime, PathBuf)>;
    let mut stack = vec![root];

    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(metadata) = entry.metadata() else {
                continue;
            };
            if metadata.is_dir() {
                stack.push(path);
                continue;
            }
            if !metadata.is_file() || path.extension().and_then(|ext| ext.to_str()) != Some("jsonl")
            {
                continue;
            }
            if path
                .parent()
                .and_then(|parent| parent.file_name())
                .and_then(|name| name.to_str())
                != Some("chats")
            {
                continue;
            }

            let modified = metadata.modified().unwrap_or(SystemTime::UNIX_EPOCH);
            if is_newer(&best_any, modified) {
                best_any = Some((modified, path.clone()));
            }
            if gemini_transcript_matches_project(&path, project_dir)
                && is_newer(&best_match, modified)
            {
                best_match = Some((modified, path));
            }
        }
    }

    best_match.or(best_any).map(|(_, path)| path)
}

fn gemini_transcript_matches_project(path: &Path, project_dir: &str) -> bool {
    if project_dir.is_empty() || project_dir == "?" {
        return false;
    }
    let Some(project_root_path) = path
        .parent()
        .and_then(|chats| chats.parent())
        .map(|project| project.join(".project_root"))
    else {
        return false;
    };
    let Ok(root) = fs::read_to_string(project_root_path) else {
        return false;
    };
    let root = root.trim();
    !root.is_empty() && project_dir.starts_with(root)
}

fn is_newer(current: &Option<(SystemTime, PathBuf)>, candidate: SystemTime) -> bool {
    current
        .as_ref()
        .map(|(previous, _)| candidate > *previous)
        .unwrap_or(true)
}

fn parse_claude_transcript_tail(tail: &str) -> ClaudeTranscriptData {
    let mut data = ClaudeTranscriptData::default();
    for line in tail.lines() {
        match parse_claude_jsonl_line(line) {
            Some(ClaudeJsonlEntry::Assistant {
                model,
                usage,
                content,
            }) => {
                if let Some(model) = model {
                    data.model = model;
                }
                if let Some(content) = content.filter(|content| !content.is_empty()) {
                    data.last_ai_output = truncate_chars(&content, 120);
                    data.inferred_status = "writing".to_string();
                }
                if let Some(usage) = usage {
                    data.tokens_in = data.tokens_in.saturating_add(usage.input_tokens);
                    data.tokens_out = data.tokens_out.saturating_add(usage.output_tokens);
                    data.last_input_tokens = usage.input_tokens;
                }
            }
            Some(ClaudeJsonlEntry::Human { content }) => {
                data.last_message = truncate_chars(&content, 120);
                data.inferred_status = "thinking".to_string();
            }
            None => {}
        }
    }

    if !data.model.is_empty() && (data.tokens_in > 0 || data.tokens_out > 0) {
        data.cost_usd = calculate_claude_cost(&data.model, data.tokens_in, data.tokens_out);
    }
    data
}

fn parse_gemini_transcript_tail(tail: &str) -> GeminiTranscriptData {
    let mut data = GeminiTranscriptData::default();
    let mut seen_token_ids = BTreeSet::<String>::new();

    for line in tail.lines() {
        let Ok(value) = serde_json::from_str::<Value>(line.trim()) else {
            continue;
        };
        let Some(obj) = value.as_object() else {
            continue;
        };
        let Some(typ) = obj.get("type").and_then(|value| value.as_str()) else {
            continue;
        };
        let should_count_tokens = obj
            .get("id")
            .and_then(|value| value.as_str())
            .map(|id| seen_token_ids.insert(id.to_string()))
            .unwrap_or(true);

        if typ == "user" || typ == "human" {
            let content = obj.get("content").and_then(json_text_content).or_else(|| {
                obj.get("message")
                    .and_then(|message| message.get("content"))
                    .and_then(json_text_content)
            });
            if let Some(content) = content.filter(|content| !content.is_empty()) {
                data.last_message = truncate_chars(&content, 120);
                data.inferred_status = "thinking".to_string();
            }
            continue;
        }

        if typ != "gemini" {
            continue;
        }

        if let Some(model) = obj
            .get("model")
            .and_then(|value| value.as_str())
            .filter(|model| !model.is_empty())
        {
            data.model = model.to_string();
        }

        if let Some(content) = obj
            .get("content")
            .and_then(json_text_content)
            .filter(|content| !content.is_empty())
        {
            data.last_ai_output = truncate_chars(&content, 120);
            data.inferred_status = "writing".to_string();
        } else if let Some(thought) = obj
            .get("thoughts")
            .and_then(gemini_thought_text)
            .filter(|thought| !thought.is_empty())
        {
            data.last_ai_output = truncate_chars(&thought, 120);
            if data.inferred_status.is_empty() {
                data.inferred_status = "thinking".to_string();
            }
        }

        if should_count_tokens {
            if let Some(tokens) = obj.get("tokens").and_then(parse_gemini_tokens) {
                data.tokens_input = data.tokens_input.saturating_add(tokens.input_tokens);
                data.tokens_output = data.tokens_output.saturating_add(tokens.output_tokens);
                data.tokens_total = data.tokens_total.saturating_add(tokens.total_tokens);
                data.last_input_tokens = tokens.input_tokens;
            }
        }
    }

    data
}

fn parse_claude_jsonl_line(line: &str) -> Option<ClaudeJsonlEntry> {
    let value: Value = serde_json::from_str(line.trim()).ok()?;
    let obj = value.as_object()?;
    let typ = obj.get("type")?.as_str()?;

    if typ == "user" || typ == "human" {
        let content = obj
            .get("message")
            .and_then(|message| message.get("content"))
            .and_then(json_text_content)
            .or_else(|| obj.get("content").and_then(json_text_content));
        return content
            .filter(|content| !content.is_empty())
            .map(|content| ClaudeJsonlEntry::Human { content });
    }

    if typ != "assistant" {
        return None;
    }

    let model = obj
        .get("message")
        .and_then(|message| message.get("model"))
        .or_else(|| obj.get("model"))
        .and_then(|value| value.as_str())
        .map(|value| value.to_string());

    let usage = obj
        .get("usage")
        .or_else(|| obj.get("message").and_then(|message| message.get("usage")))
        .and_then(parse_claude_usage);

    let content = obj
        .get("message")
        .and_then(|message| message.get("content"))
        .and_then(json_text_content)
        .or_else(|| obj.get("content").and_then(json_text_content));

    Some(ClaudeJsonlEntry::Assistant {
        model,
        usage,
        content,
    })
}

fn parse_claude_usage(value: &Value) -> Option<ClaudeUsageData> {
    let input_tokens = value
        .get("input_tokens")
        .and_then(|value| value.as_u64())
        .unwrap_or(0)
        .saturating_add(
            value
                .get("cache_read_input_tokens")
                .and_then(|value| value.as_u64())
                .unwrap_or(0),
        );
    let output_tokens = value
        .get("output_tokens")
        .and_then(|value| value.as_u64())
        .unwrap_or(0);
    if input_tokens > 0 || output_tokens > 0 {
        Some(ClaudeUsageData {
            input_tokens,
            output_tokens,
        })
    } else {
        None
    }
}

fn parse_gemini_tokens(value: &Value) -> Option<GeminiUsageData> {
    let input_tokens = value
        .get("input")
        .and_then(|value| value.as_u64())
        .unwrap_or(0);
    let output_tokens = value
        .get("output")
        .and_then(|value| value.as_u64())
        .unwrap_or(0)
        .saturating_add(
            value
                .get("thoughts")
                .and_then(|value| value.as_u64())
                .unwrap_or(0),
        );
    let total_tokens = value
        .get("total")
        .and_then(|value| value.as_u64())
        .unwrap_or_else(|| input_tokens.saturating_add(output_tokens));

    if input_tokens > 0 || output_tokens > 0 || total_tokens > 0 {
        Some(GeminiUsageData {
            input_tokens,
            output_tokens,
            total_tokens,
        })
    } else {
        None
    }
}

fn gemini_thought_text(value: &Value) -> Option<String> {
    value.as_array().and_then(|thoughts| {
        thoughts.iter().rev().find_map(|thought| {
            thought
                .get("description")
                .and_then(|value| value.as_str())
                .or_else(|| thought.get("subject").and_then(|value| value.as_str()))
                .map(|text| text.to_string())
        })
    })
}

fn json_text_content(value: &Value) -> Option<String> {
    if let Some(text) = value.as_str() {
        return Some(text.to_string());
    }

    value.as_array().and_then(|blocks| {
        blocks.iter().find_map(|block| {
            if block.get("type")?.as_str()? == "text" {
                block.get("text")?.as_str().map(|text| text.to_string())
            } else {
                None
            }
        })
    })
}

fn calculate_claude_cost(model: &str, tokens_in: u64, tokens_out: u64) -> f64 {
    let model = model.to_ascii_lowercase();
    let (input_price, output_price) = CLAUDE_PRICING
        .iter()
        .find(|(name, _, _)| model.contains(name))
        .map(|(_, input, output)| (*input, *output))
        .unwrap_or((3.0, 15.0));

    (tokens_in as f64 * input_price / 1_000_000.0)
        + (tokens_out as f64 * output_price / 1_000_000.0)
}

fn estimate_openai_cost(model: &str, usage: &CodexUsageData) -> Option<String> {
    let model = model.to_ascii_lowercase();
    let (input_price, cached_input_price, output_price) = OPENAI_PRICING
        .iter()
        .find(|(name, _, _, _)| model.contains(name))
        .map(|(_, input, cached, output)| (*input, *cached, *output))?;

    let cost = (usage.non_cached_input_tokens as f64 * input_price / 1_000_000.0)
        + (usage.cached_input_tokens as f64 * cached_input_price / 1_000_000.0)
        + (usage.output_tokens as f64 * output_price / 1_000_000.0);

    if cost > 0.0 {
        Some(format!("$~{cost:.4}"))
    } else {
        None
    }
}

fn estimate_gemini_cost(model: &str, tokens_in: u64, tokens_out: u64) -> Option<String> {
    if model.is_empty() {
        return None;
    }
    let model = model.to_ascii_lowercase();
    let (input_price, output_price) = GEMINI_PRICING
        .iter()
        .find(|(name, _, _)| model.contains(name))
        .map(|(_, input, output)| (*input, *output))
        .or_else(|| {
            if model.contains("flash") {
                Some((0.30, 2.50))
            } else if model.contains("pro") {
                Some((1.25, 10.0))
            } else {
                None
            }
        })?;

    let cost = (tokens_in as f64 * input_price / 1_000_000.0)
        + (tokens_out as f64 * output_price / 1_000_000.0);
    if cost > 0.0 {
        Some(format!("$~{cost:.4}"))
    } else {
        None
    }
}

fn context_percent(tokens_in_context: u64, model: &str) -> Option<u8> {
    let window = context_window(model)?;
    if window == 0 {
        return None;
    }
    let pct = tokens_in_context
        .saturating_mul(100)
        .saturating_add(window.saturating_sub(1))
        / window;
    Some(pct.min(100) as u8)
}

fn context_window(model: &str) -> Option<u64> {
    let model = model.to_ascii_lowercase();
    if model.contains("gemini") {
        Some(1_000_000)
    } else if model.contains("gpt-5") {
        Some(400_000)
    } else if model.contains("gpt-4.1") {
        Some(1_000_000)
    } else if model.contains("o3") || model.contains("o4") {
        Some(200_000)
    } else if model.contains("claude") || model.contains("sonnet") || model.contains("opus") {
        Some(200_000)
    } else {
        None
    }
}

fn resolve_cwd_from_claude_project_hash(hash: &str) -> String {
    let s = hash.strip_prefix('-').unwrap_or(hash);
    let parts = s.split('-').collect::<Vec<_>>();
    if parts.is_empty() {
        return format!("/{}", s.replace('-', "/"));
    }

    let mut path = String::from("/");
    let mut i = 0;
    while i < parts.len() {
        let mut best_j = i + 1;
        let mut j = parts.len();
        while j > i + 1 {
            let candidate_segment = parts[i..j].join("-");
            let candidate_path = format!("{}{}", path, candidate_segment);
            if Path::new(&candidate_path).exists() {
                best_j = j;
                break;
            }
            j -= 1;
        }
        path.push_str(&parts[i..best_j].join("-"));
        i = best_j;
        if i < parts.len() {
            path.push('/');
        }
    }
    path
}

fn truncate_chars(value: &str, max_chars: usize) -> String {
    let mut out = value.chars().take(max_chars).collect::<String>();
    if value.chars().count() > max_chars {
        out.push_str("...");
    }
    out
}

fn find_json_string(value: &Value, names: &[&str]) -> Option<String> {
    match value {
        Value::Object(map) => {
            for name in names {
                if let Some(Value::String(found)) = map.get(*name) {
                    return Some(found.clone());
                }
            }
            map.values()
                .find_map(|child| find_json_string(child, names))
        }
        Value::Array(values) => values
            .iter()
            .rev()
            .find_map(|child| find_json_string(child, names)),
        _ => None,
    }
}

fn tail_string(path: &Path, max_bytes: u64) -> Option<String> {
    let mut file = File::open(path).ok()?;
    let len = file.metadata().ok()?.len();
    let start = len.saturating_sub(max_bytes);
    file.seek(SeekFrom::Start(start)).ok()?;

    let mut buf = Vec::with_capacity((len - start) as usize);
    file.read_to_end(&mut buf).ok()?;
    Some(String::from_utf8_lossy(&buf).into_owned())
}

fn value_after_marker(line: &str, marker: &str) -> Option<String> {
    let start = line.find(marker)? + marker.len();
    let value = line[start..]
        .chars()
        .take_while(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.' | ':'))
        .collect::<String>();
    if value.is_empty() {
        None
    } else {
        Some(value)
    }
}

fn home_path(relative: &str) -> Option<PathBuf> {
    let home = std::env::var_os("HOME")?;
    Some(PathBuf::from(home).join(relative))
}

fn clean_optional(value: Option<String>) -> Option<String> {
    value.filter(|s| !s.trim().is_empty())
}

fn resolve_git_path(repo_dir: &Path, git_dir: &str) -> PathBuf {
    let path = Path::new(git_dir);
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        repo_dir.join(path)
    }
}

fn short_sha(s: &str) -> String {
    if s.len() > 12 && s.chars().all(|c| c.is_ascii_hexdigit()) {
        s.chars().take(12).collect()
    } else if s.is_empty() {
        "unknown".to_string()
    } else {
        s.to_string()
    }
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
