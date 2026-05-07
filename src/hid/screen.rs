use anyhow::{anyhow, Result};
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};
use tokio::time::{timeout, Duration, Instant};
use tracing::{debug, warn};

use super::device::VibeDevice;
use super::reports::{AckReport, AckStatus, ScreenCmd, REPORT_ID_OUTPUT};

const MAX_PAYLOAD: usize = 60;
const ACK_TIMEOUT: Duration = Duration::from_millis(500);
const RETRY_DELAY: Duration = Duration::from_millis(40);
const MAX_RETRIES: usize = 3;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum AckWaitError {
    Timeout,
    Closed,
}

pub struct ScreenController {
    device: Arc<VibeDevice>,
    seq: Mutex<u8>,
    ack_rx: Mutex<mpsc::Receiver<AckReport>>,
    reliable: bool,
}

impl ScreenController {
    pub fn new(device: Arc<VibeDevice>, ack_rx: mpsc::Receiver<AckReport>, reliable: bool) -> Self {
        Self {
            device,
            seq: Mutex::new(0),
            ack_rx: Mutex::new(ack_rx),
            reliable,
        }
    }

    pub async fn send_cmd(&self, cmd: ScreenCmd, text: &str) -> Result<()> {
        let bytes = text.as_bytes();

        if bytes.is_empty() {
            self.send_packet(cmd, &[]).await?;
        } else {
            for chunk in bytes.chunks(MAX_PAYLOAD) {
                self.send_packet(cmd, chunk).await?;
            }
        }
        Ok(())
    }

    async fn next_seq(&self) -> u8 {
        let mut seq_guard = self.seq.lock().await;
        let seq = *seq_guard;
        *seq_guard = seq_guard.wrapping_add(1);
        seq
    }

    async fn send_packet(&self, cmd: ScreenCmd, payload: &[u8]) -> Result<()> {
        let seq = self.next_seq().await;
        let mut report = vec![0u8; 64];
        report[0] = REPORT_ID_OUTPUT;
        report[1] = cmd as u8;
        report[2] = seq;
        report[3] = payload.len() as u8;
        report[4..4 + payload.len()].copy_from_slice(payload);

        for attempt in 1..=MAX_RETRIES {
            debug!(
                "Sending Output Report attempt={} seq={} data={:02X?}",
                attempt,
                seq,
                &report[..4 + payload.len()]
            );

            let written = self.device.write(&report)?;
            if written != report.len() {
                return Err(anyhow!(
                    "short HID write: expected {} bytes, wrote {}",
                    report.len(),
                    written
                ));
            }

            if !self.reliable {
                return Ok(());
            }

            match self.wait_for_ack(seq).await {
                Ok(AckStatus::Ok) => return Ok(()),
                Ok(AckStatus::Busy) => {
                    warn!("Device busy for seq={}, retrying", seq);
                    tokio::time::sleep(RETRY_DELAY).await;
                }
                Ok(AckStatus::Err) => return Err(anyhow!("Device reported error")),
                Err(AckWaitError::Timeout) => {
                    warn!("ACK timeout for seq={}, retrying", seq);
                    tokio::time::sleep(RETRY_DELAY).await;
                }
                Err(AckWaitError::Closed) => return Err(anyhow!("ACK channel closed")),
            }
        }

        Err(anyhow!("HID command seq={} failed after retries", seq))
    }

    async fn wait_for_ack(&self, seq: u8) -> std::result::Result<AckStatus, AckWaitError> {
        let mut rx = self.ack_rx.lock().await;
        let deadline = Instant::now() + ACK_TIMEOUT;

        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(AckWaitError::Timeout);
            }

            match timeout(remaining, rx.recv()).await {
                Ok(Some(ack)) => {
                    if ack.seq != seq {
                        warn!("ACK seq mismatch: expected {}, got {}", seq, ack.seq);
                        continue;
                    }
                    return Ok(ack.status);
                }
                Ok(None) => return Err(AckWaitError::Closed),
                Err(_) => return Err(AckWaitError::Timeout),
            }
        }
    }

    pub async fn clear(&self) -> Result<()> {
        self.send_cmd(ScreenCmd::Clear, "").await
    }

    pub async fn write_text(&self, text: &str) -> Result<()> {
        self.send_cmd(ScreenCmd::Write, text).await
    }

    #[allow(dead_code)]
    pub async fn set_cursor(&self, row: u8, col: u8) -> Result<()> {
        let text = format!("{},{}", row, col);
        self.send_cmd(ScreenCmd::SetCursor, &text).await
    }

    #[allow(dead_code)]
    pub async fn newline(&self) -> Result<()> {
        self.send_cmd(ScreenCmd::Newline, "").await
    }

    #[allow(dead_code)]
    pub async fn backspace(&self) -> Result<()> {
        self.send_cmd(ScreenCmd::Backspace, "").await
    }
}
