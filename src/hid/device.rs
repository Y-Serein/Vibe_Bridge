use anyhow::{anyhow, Result};
use hidapi::{DeviceInfo, HidApi, HidDevice};
use std::sync::{Arc, Mutex};
use tokio::sync::mpsc;
use tracing::{error, info};

use super::reports::{
    AckReport, InputReport, REPORT_ID_ACK, REPORT_ID_INPUT, VIBE_KEYBOARD_USAGE,
    VIBE_KEYBOARD_USAGE_PAGE,
};

pub const HID_READ_TIMEOUT_MS: i32 = 2;

pub struct VibeDevice {
    device: Mutex<HidDevice>,
}

impl VibeDevice {
    pub fn find_all() -> Result<Vec<DeviceInfo>> {
        let api = HidApi::new()?;
        let mut list = Vec::new();
        for device in api.device_list() {
            if device.usage_page() == VIBE_KEYBOARD_USAGE_PAGE
                && device.usage() == VIBE_KEYBOARD_USAGE
            {
                list.push(device.clone());
            }
        }
        Ok(list)
    }

    pub fn open_by_vid_pid(vid: u16, pid: u16) -> Result<Self> {
        let api = HidApi::new()?;
        let device = api.open(vid, pid)?;
        Ok(VibeDevice {
            device: Mutex::new(device),
        })
    }

    pub fn open_by_path(path: &std::ffi::CStr) -> Result<Self> {
        let api = HidApi::new()?;
        let device = api.open_path(path)?;
        Ok(VibeDevice {
            device: Mutex::new(device),
        })
    }

    pub fn read_timeout(&self, buf: &mut [u8], timeout_ms: i32) -> Result<usize> {
        let dev = self
            .device
            .lock()
            .map_err(|_| anyhow!("Mutex lock failed"))?;
        dev.read_timeout(buf, timeout_ms)
            .map_err(|e| anyhow!("HID read error: {}", e))
    }

    pub fn write(&self, data: &[u8]) -> Result<usize> {
        let dev = self
            .device
            .lock()
            .map_err(|_| anyhow!("Mutex lock failed"))?;
        dev.write(data)
            .map_err(|e| anyhow!("HID write error: {}", e))
    }
}

pub fn spawn_read_thread(device: Arc<VibeDevice>, ack_tx: mpsc::Sender<AckReport>) {
    tokio::task::spawn_blocking(move || {
        let mut buf = [0u8; 64];
        loop {
            match device.read_timeout(&mut buf, HID_READ_TIMEOUT_MS) {
                Ok(n) => {
                    if n == 0 {
                        continue;
                    }
                    if buf[0] == REPORT_ID_INPUT {
                        if let Some(report) = InputReport::parse(&buf[..n]) {
                            info!("Input: {:?}", report);
                        }
                    } else if buf[0] == REPORT_ID_ACK {
                        if let Some(ack) = AckReport::parse(&buf[..n]) {
                            let _ = ack_tx.try_send(ack);
                        }
                    }
                }
                Err(e) => {
                    error!("Read thread error: {}", e);
                    break;
                }
            }
        }
    });
}
