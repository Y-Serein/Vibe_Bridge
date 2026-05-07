use serde::Serialize;

use crate::session::detect_proc_sessions;

#[derive(Debug, Clone, Serialize)]
pub struct ClaudeStatus {
    pub model: String,
    pub project_dir: String,
    pub git_branch: String,
    pub state: String, // e.g., "Thinking", "Idle", "Ready"
    pub tokens: Option<u64>,
}

impl ClaudeStatus {
    pub fn detect_running() -> Option<Self> {
        detect_proc_sessions()
            .into_iter()
            .find(|session| session.tool == "Claude")
            .map(|session| Self {
                model: if session.model.is_empty() {
                    "Claude Code".to_string()
                } else {
                    format!("Claude {}", session.model)
                },
                project_dir: session.project_dir,
                git_branch: session.git_branch,
                state: session.state,
                tokens: session.tokens,
            })
    }

    pub fn get_mock() -> Self {
        Self {
            model: "Claude 3.5 Sonnet".into(),
            project_dir: "/home/rv_nano/Sipeed/rv_nano".into(),
            git_branch: "main".into(),
            state: "Thinking...".into(),
            tokens: Some(1240),
        }
    }
}
